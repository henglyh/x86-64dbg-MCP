/*
 * x64dbg-mcp: Model Context Protocol 服务器插件
 *
 * tools.c - MCP 工具集实现
 *
 * 将 x64dbg 调试器能力暴露为 MCP tools。所有工具在 MCP 请求线程中
 * 执行，通过 bridge API（Dbg*）与调试器交互，不阻塞 GUI 线程。
 */
#include "mcp.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

#include <jansson.h>

#include "_plugins.h"
#include "bridgemain.h"
#include "_dbgfunctions.h"

/* ================= 内部辅助 ================= */

/* 数值 → "0x..." 十六进制字符串（JSON 用） */
static json_t* mcp_json_hex(duint value)
{
    char buf[24];
    mcp_format_hex(buf, sizeof(buf), (uintptr_t)value);
    return json_string(buf);
}

/* 构造错误文本（is_error=true） */
static char* tool_error(const char* fmt, ...)
{
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    return _strdup(buf);
}

/* 构造成功结果（JSON 对象文本） */
static char* tool_ok(json_t* obj)
{
    char* text = json_dumps(obj, JSON_COMPACT | JSON_ENSURE_ASCII);
    if(!text)
        text = _strdup("{}");
    json_decref(obj);
    return text;
}

/* 构造失败结果（JSON 对象文本，支持 printf 格式） */
static char* tool_fail(const char* fmt, ...)
{
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    json_t* obj = json_object();
    json_object_set_new(obj, "error", json_string(buf));
    return tool_ok(obj);
}

/*
 * 解析地址/数值参数：优先作为 x64dbg 表达式（eip、模块+偏移等），
 * 失败时按纯数字（0x/0b/十进制）解析。
 */
static bool parse_number(McpServer* server, json_t* args, const char* key,
                         duint* out)
{
    (void)server;
    json_t* v = args ? json_object_get(args, key) : NULL;
    if(!v)
        return false;
    if(json_is_integer(v))
    {
        *out = (duint)json_integer_value(v);
        return true;
    }
    if(json_is_string(v))
    {
        const char* s = json_string_value(v);
        if(!s || !*s)
            return false;
        /* 先按表达式求值 */
        if(DbgIsValidExpression(s))
        {
            bool ok = false;
            duint val = DbgEval(s, &ok);
            if(ok)
            {
                *out = val;
                return true;
            }
        }
        /* 纯数字兜底 */
        char* end = NULL;
        unsigned long long n = _strtoui64(s, &end, 0);
        if(end != s)
        {
            *out = (duint)n;
            return true;
        }
    }
    return false;
}

/* 获取字符串参数（返回 NULL 表示不存在） */
static const char* get_string(json_t* args, const char* key)
{
    json_t* v = args ? json_object_get(args, key) : NULL;
    if(!v || !json_is_string(v))
        return NULL;
    return json_string_value(v);
}

/* 获取布尔参数 */
static bool get_bool(json_t* args, const char* key, bool def)
{
    json_t* v = args ? json_object_get(args, key) : NULL;
    if(!v)
        return def;
    if(json_is_boolean(v))
        return json_is_true(v);
    if(json_is_integer(v))
        return json_integer_value(v) != 0;
    return def;
}

/* 获取整数参数（默认值） */
static duint get_int(json_t* args, const char* key, duint def)
{
    json_t* v = args ? json_object_get(args, key) : NULL;
    if(!v)
        return def;
    if(json_is_integer(v))
        return (duint)json_integer_value(v);
    if(json_is_string(v))
    {
        const char* s = json_string_value(v);
        char* end = NULL;
        unsigned long long n = _strtoui64(s, &end, 0);
        if(end != s)
            return (duint)n;
    }
    return def;
}

/* 检查调试器是否已暂停；未暂停时自动暂停并等待（最多 5 秒） */
static bool ensure_paused(char** err)
{
    if(!DbgIsDebugging())
    {
        *err = _strdup("No process is being debugged");
        return false;
    }
    if(!DbgIsRunning())
        return true;

    DbgCmdExec("pause");
    for(int i = 0; i < 50; i++)
    {
        if(!DbgIsRunning())
            return true;
        Sleep(100);
    }
    *err = _strdup("Timed out waiting for debuggee to pause");
    return false;
}

/* hex 编码 */
static void hex_encode(const unsigned char* data, size_t len, char* out)
{
    for(size_t i = 0; i < len; i++)
        sprintf(out + i * 2, "%02X", data[i]);
}

/* hex 解码（支持 空格 分隔），返回解码长度，-1 表示非法输入 */
static int hex_decode(const char* hex, unsigned char* out, size_t maxlen)
{
    int n = 0;
    const char* p = hex;
    while(*p)
    {
        while(*p == ' ' || *p == '\t' || *p == ',')
            p++;
        if(!*p)
            break;
        if((size_t)n >= maxlen)
            return -1;
        int hi = -1, lo = -1;
        if(*p >= '0' && *p <= '9') hi = *p - '0';
        else if(*p >= 'a' && *p <= 'f') hi = *p - 'a' + 10;
        else if(*p >= 'A' && *p <= 'F') hi = *p - 'A' + 10;
        if(hi >= 0 && p[1])
        {
            if(p[1] >= '0' && p[1] <= '9') lo = p[1] - '0';
            else if(p[1] >= 'a' && p[1] <= 'f') lo = p[1] - 'a' + 10;
            else if(p[1] >= 'A' && p[1] <= 'F') lo = p[1] - 'A' + 10;
        }
        if(hi < 0 || lo < 0)
            return -1;
        out[n++] = (unsigned char)((hi << 4) | lo);
        p += 2;
    }
    return n;
}

/* ================= 工具实现 ================= */

/* 获取调试器状态 */
static char* tool_get_state(McpServer* server, json_t* args, bool* is_error)
{
    (void)server;
    (void)args;
    json_t* obj = json_object();

    if(!DbgIsDebugging())
    {
        json_object_set_new(obj, "state", json_string("no_debug"));
        json_object_set_new(obj, "debugging", json_false());
        return tool_ok(obj);
    }

    bool running = DbgIsRunning();
    json_object_set_new(obj, "debugging", json_true());
    json_object_set_new(obj, "state", json_string(running ? "running" : "paused"));
    json_object_set_new(obj, "process_id", json_integer(DbgGetProcessId()));
    json_object_set_new(obj, "thread_id", json_integer(DbgGetThreadId()));

    if(!running)
    {
        REGDUMP_AVX512 rd;
        memset(&rd, 0, sizeof(rd));
        if(DbgGetRegDumpEx(&rd, sizeof(rd)))
        {
            json_object_set_new(obj, "cip", mcp_json_hex(rd.regcontext.cip));
            json_object_set_new(obj, "csp", mcp_json_hex(rd.regcontext.csp));
        }
        char mod[MAX_MODULE_SIZE] = "";
        if(DbgGetModuleAt(rd.regcontext.cip, mod))
            json_object_set_new(obj, "module", json_string(mod));
    }
    return tool_ok(obj);
}

/* 执行简单的调试控制命令 */
static char* tool_run(McpServer* server, json_t* args, bool* is_error)
{
    (void)server;
    if(!DbgIsDebugging())
    {
        *is_error = true;
        return tool_error("No process is being debugged");
    }

    duint addr = 0;
    bool has_addr = parse_number(server, args, "address", &addr);

    char cmd[128];
    if(has_addr)
    {
        char hex[24];
        mcp_format_hex_raw(hex, sizeof(hex), (uintptr_t)addr);
        _snprintf(cmd, sizeof(cmd), "run %s", hex);
    }
    else
        _snprintf(cmd, sizeof(cmd), "run");

    bool ok = DbgCmdExec(cmd);
    if(!ok)
    {
        *is_error = true;
        return tool_error("Failed to submit 'run' command");
    }

    json_t* obj = json_object();
    json_object_set_new(obj, "success", json_true());
    json_object_set_new(obj, "message", json_string("Debuggee resumed (run)"));
    if(has_addr)
        json_object_set_new(obj, "run_to", mcp_json_hex(addr));
    return tool_ok(obj);
}

static char* tool_pause(McpServer* server, json_t* args, bool* is_error)
{
    (void)server;
    (void)args;
    if(!DbgIsDebugging())
    {
        *is_error = true;
        return tool_error("No process is being debugged");
    }
    bool ok = DbgCmdExec("pause");
    if(!ok)
    {
        *is_error = true;
        return tool_error("Failed to submit 'pause' command");
    }
    json_t* obj = json_object();
    json_object_set_new(obj, "success", json_true());
    json_object_set_new(obj, "message", json_string("Pause requested"));
    return tool_ok(obj);
}

static char* tool_restart(McpServer* server, json_t* args, bool* is_error)
{
    (void)server;
    (void)args;
    if(!DbgIsDebugging())
    {
        *is_error = true;
        return tool_error("No process is being debugged");
    }
    bool ok = DbgCmdExec("restart");
    if(!ok)
    {
        *is_error = true;
        return tool_error("Failed to submit 'restart' command");
    }
    json_t* obj = json_object();
    json_object_set_new(obj, "success", json_true());
    json_object_set_new(obj, "message", json_string("Restart requested"));
    return tool_ok(obj);
}

static char* tool_stop(McpServer* server, json_t* args, bool* is_error)
{
    (void)server;
    (void)args;
    if(!DbgIsDebugging())
    {
        *is_error = true;
        return tool_error("No process is being debugged");
    }
    bool ok = DbgCmdExec("stop");
    if(!ok)
    {
        *is_error = true;
        return tool_error("Failed to submit 'stop' command");
    }
    json_t* obj = json_object();
    json_object_set_new(obj, "success", json_true());
    json_object_set_new(obj, "message", json_string("Stop requested"));
    return tool_ok(obj);
}

/* 单步执行（into/over/out） */
static char* tool_step(McpServer* server, json_t* args, bool* is_error, const char* cmd)
{
    (void)server;
    char* err = NULL;
    if(!ensure_paused(&err))
    {
        *is_error = true;
        return err;
    }
    bool ok = DbgCmdExecDirect(cmd);
    if(!ok)
    {
        *is_error = true;
        return tool_error("Failed to execute step command '%s'", cmd);
    }
    json_t* obj = json_object();
    json_object_set_new(obj, "success", json_true());
    json_object_set_new(obj, "message", json_string("Step executed"));
    return tool_ok(obj);
}

static char* tool_step_into(McpServer* server, json_t* args, bool* is_error)
{
    return tool_step(server, args, is_error, "sti");
}

static char* tool_step_over(McpServer* server, json_t* args, bool* is_error)
{
    return tool_step(server, args, is_error, "sto");
}

static char* tool_step_out(McpServer* server, json_t* args, bool* is_error)
{
    return tool_step(server, args, is_error, "sio");
}

/* 设置断点 */
static char* tool_set_breakpoint(McpServer* server, json_t* args, bool* is_error)
{
    (void)server;
    char* err = NULL;
    if(!ensure_paused(&err))
    {
        *is_error = true;
        return err;
    }

    const char* type = get_string(args, "type");
    const char* name = get_string(args, "name");
    duint addr = 0;
    bool has_addr = parse_number(server, args, "address", &addr);

    char cmd[512];
    if(type && strcmp(type, "dll") == 0)
    {
        if(!name)
        {
            *is_error = true;
            return tool_fail("Parameter 'name' (module name) is required for dll breakpoint");
        }
        _snprintf(cmd, sizeof(cmd), "bpd %s", name);
    }
    else if(type && strcmp(type, "exception") == 0)
    {
        if(!has_addr)
        {
            *is_error = true;
            return tool_fail("Parameter 'address' (exception code) is required for exception breakpoint");
        }
        char hex[24];
        mcp_format_hex_raw(hex, sizeof(hex), (uintptr_t)addr);
        _snprintf(cmd, sizeof(cmd), "bpe %s", hex);
    }
    else
    {
        if(!has_addr)
        {
            *is_error = true;
            return tool_fail("Parameter 'address' is required");
        }
        char hex[24];
        mcp_format_hex_raw(hex, sizeof(hex), (uintptr_t)addr);
        if(type && strcmp(type, "hardware") == 0)
            _snprintf(cmd, sizeof(cmd), "bph %s", hex);
        else if(type && strcmp(type, "memory") == 0)
            _snprintf(cmd, sizeof(cmd), "bpm %s", hex);
        else
            _snprintf(cmd, sizeof(cmd), "bp %s", hex);
    }

    if(!DbgCmdExecDirect(cmd))
    {
        *is_error = true;
        return tool_fail("Failed to set breakpoint (command rejected)");
    }

    json_t* obj = json_object();
    json_object_set_new(obj, "success", json_true());
    json_object_set_new(obj, "command", json_string(cmd));
    if(has_addr)
    {
        json_object_set_new(obj, "address", mcp_json_hex(addr));
        BPXTYPE bptype = DbgGetBpxTypeAt(addr);
        json_object_set_new(obj, "active_types", json_integer(bptype));
    }
    return tool_ok(obj);
}

/* 删除断点 */
static char* tool_delete_breakpoint(McpServer* server, json_t* args, bool* is_error)
{
    (void)server;
    char* err = NULL;
    if(!ensure_paused(&err))
    {
        *is_error = true;
        return err;
    }

    const char* type = get_string(args, "type");
    const char* name = get_string(args, "name");
    duint addr = 0;
    bool has_addr = parse_number(server, args, "address", &addr);

    char cmd[512];
    if(type && strcmp(type, "dll") == 0)
    {
        if(!name)
        {
            *is_error = true;
            return tool_fail("Parameter 'name' (module name) is required for dll breakpoint");
        }
        _snprintf(cmd, sizeof(cmd), "bpdc %s", name);
    }
    else if(type && strcmp(type, "exception") == 0)
    {
        if(!has_addr)
        {
            *is_error = true;
            return tool_fail("Parameter 'address' (exception code) is required for exception breakpoint");
        }
        char hex[24];
        mcp_format_hex_raw(hex, sizeof(hex), (uintptr_t)addr);
        _snprintf(cmd, sizeof(cmd), "bpec %s", hex);
    }
    else
    {
        if(!has_addr)
        {
            *is_error = true;
            return tool_fail("Parameter 'address' is required");
        }
        char hex[24];
        mcp_format_hex_raw(hex, sizeof(hex), (uintptr_t)addr);
        if(type && strcmp(type, "hardware") == 0)
            _snprintf(cmd, sizeof(cmd), "bphwc %s", hex);
        else if(type && strcmp(type, "memory") == 0)
            _snprintf(cmd, sizeof(cmd), "bpmc %s", hex);
        else
            _snprintf(cmd, sizeof(cmd), "bc %s", hex);
    }

    if(!DbgCmdExecDirect(cmd))
    {
        *is_error = true;
        return tool_fail("Failed to delete breakpoint (command rejected)");
    }

    json_t* obj = json_object();
    json_object_set_new(obj, "success", json_true());
    json_object_set_new(obj, "command", json_string(cmd));
    return tool_ok(obj);
}

/* 列出断点 */
static char* tool_list_breakpoints(McpServer* server, json_t* args, bool* is_error)
{
    (void)server;
    (void)args;
    json_t* obj = json_object();
    json_t* arr = json_array();
    static const BPXTYPE types[] = { bp_normal, bp_hardware, bp_memory, bp_dll, bp_exception };
    static const char* type_names[] = { "normal", "hardware", "memory", "dll", "exception" };

    for(int t = 0; t < 5; t++)
    {
        BPMAP map;
        memset(&map, 0, sizeof(map));
        int count = DbgGetBpList(types[t], &map);
        for(int i = 0; i < count && i < map.count; i++)
        {
            BRIDGEBP* bp = &map.bp[i];
            json_t* item = json_object();
            json_object_set_new(item, "type", json_string(type_names[t]));
            json_object_set_new(item, "address", mcp_json_hex(bp->addr));
            json_object_set_new(item, "enabled", json_boolean(bp->enabled));
            json_object_set_new(item, "active", json_boolean(bp->active));
            json_object_set_new(item, "singleshoot", json_boolean(bp->singleshoot));
            json_object_set_new(item, "hit_count", json_integer(bp->hitCount));
            if(bp->name[0])
                json_object_set_new(item, "name", json_string(bp->name));
            if(bp->mod[0])
                json_object_set_new(item, "module", json_string(bp->mod));
            if(bp->breakCondition[0])
                json_object_set_new(item, "condition", json_string(bp->breakCondition));
            if(bp->commandText[0])
                json_object_set_new(item, "command", json_string(bp->commandText));
            if(bp->logText[0])
                json_object_set_new(item, "log", json_string(bp->logText));
            json_array_append_new(arr, item);
        }
        if(map.bp)
            BridgeFree(map.bp);
    }

    json_object_set_new(obj, "count", json_integer((json_int_t)json_array_size(arr)));
    json_object_set_new(obj, "breakpoints", arr);
    return tool_ok(obj);
}

/* 读内存 */
static char* tool_read_memory(McpServer* server, json_t* args, bool* is_error)
{
    (void)server;
    duint addr = 0;
    if(!parse_number(server, args, "address", &addr))
    {
        *is_error = true;
        return tool_fail("Parameter 'address' is required");
    }

    duint size = get_int(args, "size", 64);
    if(size == 0)
        size = 64;
    if(size > 0x1000)
    {
        *is_error = true;
        return tool_fail("Parameter 'size' must be <= 0x1000 (4096)");
    }

    unsigned char* buf = (unsigned char*)malloc((size_t)size);
    if(!buf)
    {
        *is_error = true;
        return tool_fail("Out of memory");
    }

    duint read = 0;
    bool ok = false;
    if(DbgIsDebugging())
    {
        /* 尽量读满请求长度 */
        read = size;
        if(!DbgMemRead(addr, buf, size))
        {
            /* 部分读取：探测可用页 */
            duint base = DbgMemFindBaseAddr(addr, NULL);
            duint pagesize = DbgMemGetPageSize(base ? base : addr);
            if(pagesize > 0 && pagesize < size)
            {
                read = pagesize;
                ok = DbgMemRead(addr, buf, read);
            }
        }
        else
        {
            ok = true;
        }
    }

    if(!ok)
    {
        free(buf);
        *is_error = true;
        {
            char hex[24];
            mcp_format_hex(hex, sizeof(hex), (uintptr_t)addr);
            return tool_fail("Failed to read memory at %s", hex);
        }
    }

    /* 构建可读的 ascii 视图 */
    char* hex = (char*)malloc((size_t)read * 2 + 1);
    char* ascii = (char*)malloc((size_t)read + 1);
    if(!hex || !ascii)
    {
        free(buf);
        free(hex);
        free(ascii);
        *is_error = true;
        return tool_fail("Out of memory");
    }
    hex_encode(buf, (size_t)read, hex);
    for(duint i = 0; i < read; i++)
    {
        unsigned char c = buf[i];
        ascii[i] = (c >= 0x20 && c < 0x7f) ? (char)c : '.';
    }
    ascii[read] = '\0';

    json_t* obj = json_object();
    json_object_set_new(obj, "address", mcp_json_hex(addr));
    json_object_set_new(obj, "size", json_integer(read));
    json_object_set_new(obj, "hex", json_string(hex));
    json_object_set_new(obj, "ascii", json_string(ascii));
    /* 尝试显示字符串/指针辅助信息 */
    if(read >= 4)
    {
        duint ptr = 0;
        memcpy(&ptr, buf, sizeof(duint) < (size_t)read ? sizeof(duint) : (size_t)read);
        if(DbgMemIsValidReadPtr(ptr))
            json_object_set_new(obj, "first_ptr", mcp_json_hex(ptr));
    }

    free(buf);
    free(hex);
    free(ascii);
    return tool_ok(obj);
}

/* 写内存 */
static char* tool_write_memory(McpServer* server, json_t* args, bool* is_error)
{
    (void)server;
    duint addr = 0;
    if(!parse_number(server, args, "address", &addr))
    {
        *is_error = true;
        return tool_fail("Parameter 'address' is required");
    }

    unsigned char buf[0x1000];
    int len = -1;

    const char* hex = get_string(args, "hex");
    if(hex)
        len = hex_decode(hex, buf, sizeof(buf));

    if(len < 0)
    {
        /* 尝试 bytes 数组 */
        json_t* bytes = args ? json_object_get(args, "bytes") : NULL;
        if(json_is_array(bytes))
        {
            len = 0;
            size_t n = json_array_size(bytes);
            for(size_t i = 0; i < n && (size_t)len < sizeof(buf); i++)
            {
                json_t* b = json_array_get(bytes, i);
                if(!json_is_integer(b))
                {
                    len = -1;
                    break;
                }
                buf[len++] = (unsigned char)json_integer_value(b);
            }
        }
    }

    if(len <= 0)
    {
        *is_error = true;
        return tool_fail("Parameter 'hex' (hex string) or 'bytes' (byte array) is required");
    }

    if(!DbgMemWrite(addr, buf, (duint)len))
    {
        *is_error = true;
        char hex[24];
        mcp_format_hex(hex, sizeof(hex), (uintptr_t)addr);
        return tool_fail("Failed to write memory at %s", hex);
    }

    json_t* obj = json_object();
    json_object_set_new(obj, "success", json_true());
    json_object_set_new(obj, "address", mcp_json_hex(addr));
    json_object_set_new(obj, "written", json_integer(len));
    return tool_ok(obj);
}

/* 读寄存器 */
static char* tool_read_registers(McpServer* server, json_t* args, bool* is_error)
{
    (void)server;
    (void)args;
    char* err = NULL;
    if(!ensure_paused(&err))
    {
        *is_error = true;
        return err;
    }

    REGDUMP_AVX512 rd;
    memset(&rd, 0, sizeof(rd));
    if(!DbgGetRegDumpEx(&rd, sizeof(rd)))
    {
        *is_error = true;
        return tool_fail("Failed to read registers");
    }

    json_t* obj = json_object();
    REGISTERCONTEXT_AVX512* rc = &rd.regcontext;
    json_object_set_new(obj, "cax", mcp_json_hex(rc->cax));
    json_object_set_new(obj, "ccx", mcp_json_hex(rc->ccx));
    json_object_set_new(obj, "cdx", mcp_json_hex(rc->cdx));
    json_object_set_new(obj, "cbx", mcp_json_hex(rc->cbx));
    json_object_set_new(obj, "csp", mcp_json_hex(rc->csp));
    json_object_set_new(obj, "cbp", mcp_json_hex(rc->cbp));
    json_object_set_new(obj, "csi", mcp_json_hex(rc->csi));
    json_object_set_new(obj, "cdi", mcp_json_hex(rc->cdi));
    json_object_set_new(obj, "cip", mcp_json_hex(rc->cip));
    json_object_set_new(obj, "eflags", mcp_json_hex(rc->eflags));
    json_object_set_new(obj, "dr0", mcp_json_hex(rc->dr0));
    json_object_set_new(obj, "dr1", mcp_json_hex(rc->dr1));
    json_object_set_new(obj, "dr2", mcp_json_hex(rc->dr2));
    json_object_set_new(obj, "dr3", mcp_json_hex(rc->dr3));
    json_object_set_new(obj, "dr6", mcp_json_hex(rc->dr6));
    json_object_set_new(obj, "dr7", mcp_json_hex(rc->dr7));
    json_object_set_new(obj, "segment_cs", json_integer(rc->cs));
    json_object_set_new(obj, "segment_ss", json_integer(rc->ss));
    json_object_set_new(obj, "segment_ds", json_integer(rc->ds));
    json_object_set_new(obj, "segment_es", json_integer(rc->es));
    json_object_set_new(obj, "segment_fs", json_integer(rc->fs));
    json_object_set_new(obj, "segment_gs", json_integer(rc->gs));
#ifdef _WIN64
    json_object_set_new(obj, "r8", mcp_json_hex(rc->r8));
    json_object_set_new(obj, "r9", mcp_json_hex(rc->r9));
    json_object_set_new(obj, "r10", mcp_json_hex(rc->r10));
    json_object_set_new(obj, "r11", mcp_json_hex(rc->r11));
    json_object_set_new(obj, "r12", mcp_json_hex(rc->r12));
    json_object_set_new(obj, "r13", mcp_json_hex(rc->r13));
    json_object_set_new(obj, "r14", mcp_json_hex(rc->r14));
    json_object_set_new(obj, "r15", mcp_json_hex(rc->r15));
#endif
    return tool_ok(obj);
}

/* 写寄存器 */
static char* tool_write_register(McpServer* server, json_t* args, bool* is_error)
{
    (void)server;
    char* err = NULL;
    if(!ensure_paused(&err))
    {
        *is_error = true;
        return err;
    }

    const char* name = get_string(args, "name");
    const char* value = get_string(args, "value");
    if(!name || !name[0] || !value || !value[0])
    {
        *is_error = true;
        return tool_fail("Parameters 'name' and 'value' are required");
    }

    duint v = DbgValFromString(value);
    if(!DbgValSetScalar(name, v))
    {
        *is_error = true;
        return tool_fail("Failed to set register '%s' (invalid name or not writable)", name);
    }

    json_t* obj = json_object();
    json_object_set_new(obj, "success", json_true());
    json_object_set_new(obj, "name", json_string(name));
    json_object_set_new(obj, "value", mcp_json_hex(v));
    return tool_ok(obj);
}

/* 反汇编 */
static char* tool_disassemble(McpServer* server, json_t* args, bool* is_error)
{
    (void)server;
    duint addr = 0;
    if(!parse_number(server, args, "address", &addr))
    {
        /* 默认从 cip 开始 */
        if(DbgIsDebugging() && !DbgIsRunning())
        {
            REGDUMP_AVX512 rd;
            memset(&rd, 0, sizeof(rd));
            if(DbgGetRegDumpEx(&rd, sizeof(rd)))
                addr = rd.regcontext.cip;
        }
        if(!addr)
        {
            *is_error = true;
            return tool_fail("Parameter 'address' is required (or pause the debuggee to use cip)");
        }
    }

    duint count = get_int(args, "count", 8);
    if(count < 1)
        count = 1;
    if(count > 64)
        count = 64;

    json_t* obj = json_object();
    json_t* arr = json_array();

    for(duint i = 0; i < count; i++)
    {
        BASIC_INSTRUCTION_INFO bi;
        memset(&bi, 0, sizeof(bi));
        DbgDisasmFastAt(addr, &bi);

        json_t* item = json_object();
        json_object_set_new(item, "address", mcp_json_hex(addr));
        json_object_set_new(item, "size", json_integer(bi.size));
        if(bi.instruction[0])
            json_object_set_new(item, "instruction", json_string(bi.instruction));
        if(bi.branch)
            json_object_set_new(item, "branch", json_boolean(true));
        if(bi.call)
            json_object_set_new(item, "call", json_boolean(true));
        if(bi.branch || bi.call)
            json_object_set_new(item, "target", mcp_json_hex(bi.addr));

        /* 指令字节 */
        unsigned char bytes[16];
        if(bi.size > 0 && bi.size <= 16 && DbgMemRead(addr, bytes, (duint)bi.size))
        {
            char hex[33];
            hex_encode(bytes, (size_t)bi.size, hex);
            json_object_set_new(item, "bytes", json_string(hex));
        }

        /* 标签/注释 */
        char label[256] = "";
        char comment[MAX_COMMENT_SIZE] = "";
        if(DbgGetLabelAt(addr, SEG_DEFAULT, label))
            json_object_set_new(item, "label", json_string(label));
        if(DbgGetCommentAt(addr, comment))
            json_object_set_new(item, "comment", json_string(comment));

        json_array_append_new(arr, item);
        addr += bi.size > 0 ? (duint)bi.size : 1;
    }

    json_object_set_new(obj, "count", json_integer(count));
    json_object_set_new(obj, "instructions", arr);
    return tool_ok(obj);
}

/* 汇编 */
static char* tool_assemble(McpServer* server, json_t* args, bool* is_error)
{
    (void)server;
    char* err = NULL;
    if(!ensure_paused(&err))
    {
        *is_error = true;
        return err;
    }

    duint addr = 0;
    if(!parse_number(server, args, "address", &addr))
    {
        *is_error = true;
        return tool_fail("Parameter 'address' is required");
    }
    const char* instruction = get_string(args, "instruction");
    if(!instruction || !instruction[0])
    {
        *is_error = true;
        return tool_fail("Parameter 'instruction' is required");
    }

    unsigned char before[16];
    memset(before, 0, sizeof(before));
    DbgMemRead(addr, before, sizeof(before));

    if(!DbgAssembleAt(addr, instruction))
    {
        *is_error = true;
        char hex[24];
        mcp_format_hex(hex, sizeof(hex), (uintptr_t)addr);
        return tool_fail("Failed to assemble '%s' at %s", instruction, hex);
    }

    unsigned char after[16];
    memset(after, 0, sizeof(after));
    DbgMemRead(addr, after, sizeof(after));

    /* 计算写入的字节数（对比前后） */
    int written = 0;
    for(int i = 0; i < 16; i++)
    {
        if(before[i] != after[i])
            written = i + 1;
    }

    json_t* obj = json_object();
    json_object_set_new(obj, "success", json_true());
    json_object_set_new(obj, "address", mcp_json_hex(addr));
    json_object_set_new(obj, "instruction", json_string(instruction));
    if(written > 0)
    {
        char hex[33];
        hex_encode(after, (size_t)written, hex);
        json_object_set_new(obj, "bytes", json_string(hex));
        json_object_set_new(obj, "size", json_integer(written));
    }
    return tool_ok(obj);
}

/* 表达式求值 */
static char* tool_eval(McpServer* server, json_t* args, bool* is_error)
{
    (void)server;
    const char* expression = get_string(args, "expression");
    if(!expression || !expression[0])
    {
        *is_error = true;
        return tool_fail("Parameter 'expression' is required");
    }

    bool ok = false;
    duint value = DbgEval(expression, &ok);
    if(!ok)
    {
        /* 非表达式（纯数字）时直接解析 */
        char* end = NULL;
        unsigned long long n = _strtoui64(expression, &end, 0);
        if(end != expression && *end == '\0')
        {
            value = (duint)n;
            ok = true;
        }
    }
    if(!ok)
    {
        *is_error = true;
        return tool_fail("Failed to evaluate expression: %s", expression);
    }

    json_t* obj = json_object();
    json_object_set_new(obj, "expression", json_string(expression));
    json_object_set_new(obj, "value", mcp_json_hex(value));
    json_object_set_new(obj, "decimal", json_integer(value));

    char label[256] = "";
    if(DbgGetLabelAt(value, SEG_DEFAULT, label))
        json_object_set_new(obj, "label", json_string(label));
    return tool_ok(obj);
}

/* 执行任意 x64dbg 命令 */
static char* tool_command(McpServer* server, json_t* args, bool* is_error)
{
    (void)server;
    const char* command = get_string(args, "command");
    if(!command || !command[0])
    {
        *is_error = true;
        return tool_fail("Parameter 'command' is required");
    }

    bool sync = get_bool(args, "sync", true);
    bool ok = sync ? DbgCmdExecDirect(command) : DbgCmdExec(command);

    json_t* obj = json_object();
    json_object_set_new(obj, "command", json_string(command));
    json_object_set_new(obj, "success", json_boolean(ok));
    json_object_set_new(obj, "sync", json_boolean(sync));
    if(!ok)
        json_object_set_new(obj, "error", json_string("Command failed or not accepted"));
    return tool_ok(obj);
}

/* 模块列表 */
static char* tool_get_modules(McpServer* server, json_t* args, bool* is_error)
{
    (void)server;
    (void)args;
    if(!DbgIsDebugging())
    {
        *is_error = true;
        return tool_fail("No process is being debugged");
    }

    MEMMAP mm;
    memset(&mm, 0, sizeof(mm));
    if(!DbgMemMap(&mm) || mm.count <= 0)
    {
        *is_error = true;
        return tool_fail("Failed to query memory map");
    }

    /* 按名称聚合 MEM_IMAGE 页为模块（x64dbg 中节页的 info 为 " 节名"，
     * 以空格开头，并入其所属模块的地址范围） */
    typedef struct
    {
        char name[MAX_MODULE_SIZE];
        duint base;
        duint end;
    } ModEntry;
    ModEntry mods[512];
    int nmods = 0;

    for(int i = 0; i < mm.count && nmods < 512; i++)
    {
        MEMPAGE* p = &mm.page[i];
        if(p->mbi.Type == MEM_IMAGE && p->info[0])
        {
            duint page_base = (duint)p->mbi.BaseAddress;
            duint page_end = page_base + (duint)p->mbi.RegionSize;
            bool section_page = (p->info[0] == ' ');
            if(nmods > 0 && mods[nmods - 1].end == page_base &&
               (strcmp(mods[nmods - 1].name, p->info) == 0 ||
                section_page))
            {
                mods[nmods - 1].end = page_end;
            }
            else if(!section_page)
            {
                strncpy(mods[nmods].name, p->info, MAX_MODULE_SIZE - 1);
                mods[nmods].name[MAX_MODULE_SIZE - 1] = '\0';
                mods[nmods].base = page_base;
                mods[nmods].end = page_end;
                nmods++;
            }
        }
    }
    if(mm.page)
        BridgeFree(mm.page);

    json_t* obj = json_object();
    json_t* arr = json_array();
    for(int i = 0; i < nmods; i++)
    {
        json_t* item = json_object();
        json_object_set_new(item, "name", json_string(mods[i].name));
        json_object_set_new(item, "base", mcp_json_hex(mods[i].base));
        json_object_set_new(item, "size", mcp_json_hex(mods[i].end - mods[i].base));
        json_object_set_new(item, "end", mcp_json_hex(mods[i].end));
        json_array_append_new(arr, item);
    }
    json_object_set_new(obj, "count", json_integer(nmods));
    json_object_set_new(obj, "modules", arr);
    return tool_ok(obj);
}

/* 线程列表 */
static char* tool_get_threads(McpServer* server, json_t* args, bool* is_error)
{
    (void)server;
    (void)args;
    if(!DbgIsDebugging())
    {
        *is_error = true;
        return tool_fail("No process is being debugged");
    }

    THREADLIST tl;
    memset(&tl, 0, sizeof(tl));
    DbgGetThreadList(&tl);

    json_t* obj = json_object();
    json_t* arr = json_array();
    for(int i = 0; i < tl.count; i++)
    {
        THREADALLINFO* t = &tl.list[i];
        json_t* item = json_object();
        json_object_set_new(item, "index", json_integer(i));
        json_object_set_new(item, "thread_id", json_integer(t->BasicInfo.ThreadId));
        json_object_set_new(item, "cip", mcp_json_hex(t->ThreadCip));
        json_object_set_new(item, "suspend_count", json_integer(t->SuspendCount));
        json_object_set_new(item, "priority", json_integer(t->Priority));
        if(t->BasicInfo.threadName[0])
            json_object_set_new(item, "name", json_string(t->BasicInfo.threadName));
        json_array_append_new(arr, item);
    }
    if(tl.list)
        BridgeFree(tl.list);

    json_object_set_new(obj, "count", json_integer((json_int_t)json_array_size(arr)));
    json_object_set_new(obj, "threads", arr);
    return tool_ok(obj);
}

/* 调用栈 */
static char* tool_get_callstack(McpServer* server, json_t* args, bool* is_error)
{
    (void)server;
    (void)args;
    char* err = NULL;
    if(!ensure_paused(&err))
    {
        *is_error = true;
        return err;
    }

    const DBGFUNCTIONS* fns = DbgFunctions();
    if(!fns || !fns->GetCallStackEx)
    {
        *is_error = true;
        return tool_fail("Call stack API unavailable");
    }

    DBGCALLSTACK cs;
    memset(&cs, 0, sizeof(cs));
    fns->GetCallStackEx(&cs, false);

    json_t* obj = json_object();
    json_t* arr = json_array();
    for(int i = 0; i < cs.total; i++)
    {
        DBGCALLSTACKENTRY* e = &cs.entries[i];
        json_t* item = json_object();
        json_object_set_new(item, "index", json_integer(i));
        json_object_set_new(item, "address", mcp_json_hex(e->addr));
        json_object_set_new(item, "from", mcp_json_hex(e->from));
        json_object_set_new(item, "to", mcp_json_hex(e->to));
        if(e->comment[0])
            json_object_set_new(item, "comment", json_string(e->comment));
        json_array_append_new(arr, item);
    }
    if(cs.entries)
        BridgeFree(cs.entries);

    json_object_set_new(obj, "count", json_integer((json_int_t)json_array_size(arr)));
    json_object_set_new(obj, "callstack", arr);
    return tool_ok(obj);
}

/* ================= 搜索范围辅助 ================= */

/*
 * 解析搜索范围：优先 module（内存映射中聚合的映像范围），
 * 其次 address(+max_size)，缺省时取 cip 所在页。
 * 成功返回 true 并填充 *start 与 *end。
 */
static bool resolve_search_range(McpServer* server, json_t* args,
                                 duint* start, duint* end, char** err)
{
    const char* module = get_string(args, "module");

    if(module && module[0])
    {
        MEMMAP mm;
        memset(&mm, 0, sizeof(mm));
        if(!DbgMemMap(&mm))
        {
            *err = _strdup("Failed to query memory map");
            return false;
        }
        duint base = 0, last_end = 0;
        bool found = false;
        for(int i = 0; i < mm.count; i++)
        {
            MEMPAGE* p = &mm.page[i];
            if(p->mbi.Type == MEM_IMAGE)
            {
                duint pb = (duint)p->mbi.BaseAddress;
                duint pe = pb + (duint)p->mbi.RegionSize;
                if(strcmp(p->info, module) == 0)
                {
                    if(!found)
                    {
                        base = pb;
                        last_end = pe;
                        found = true;
                    }
                    else if(pb == last_end)
                        last_end = pe; /* 相邻页合并 */
                }
                else if(found && p->info[0] == ' ' && pb == last_end)
                {
                    last_end = pe; /* 节页（" 节名"）并入模块范围 */
                }
            }
        }
        if(mm.page)
            BridgeFree(mm.page);
        if(!found)
        {
            *err = (char*)malloc(strlen(module) + 64);
            if(*err)
                sprintf(*err, "Module '%s' not found in memory map", module);
            return false;
        }
        *start = base;
        *end = last_end;
    }
    else
    {
        duint addr = 0;
        if(!parse_number(server, args, "address", &addr))
        {
            /* 缺省：cip 所在页 */
            if(!DbgIsRunning())
            {
                REGDUMP_AVX512 rd;
                memset(&rd, 0, sizeof(rd));
                if(DbgGetRegDumpEx(&rd, sizeof(rd)))
                    addr = rd.regcontext.cip;
            }
            if(!addr)
            {
                *err = _strdup("Parameter 'module' or 'address' is required");
                return false;
            }
        }
        duint base = DbgMemFindBaseAddr(addr, NULL);
        duint size = DbgMemGetPageSize(base ? base : addr);
        *start = base ? base : addr;
        *end = *start + (size ? size : 0x1000);
    }

    duint max_scan = get_int(args, "max_size", 0);
    if(max_scan > 0 && *start + max_scan < *end)
        *end = *start + max_scan;
    return true;
}

/* 记录一条搜索到的字符串（data 为收集缓冲，collected 为缓冲内字节数，
 * total 为字符串总长，可能大于缓冲容量） */
static void find_strings_record(json_t* arr, duint addr, const char* data,
                                duint collected, duint total, bool utf16)
{
    json_t* item = json_object();
    json_object_set_new(item, "address", mcp_json_hex(addr));
    json_object_set_new(item, "length", json_integer(total));

    if(utf16)
    {
        /* data 为 UTF-16LE 字节对（高字节恒为 0） */
        wchar_t* w = (wchar_t*)malloc((size_t)collected + 2);
        if(w)
        {
            for(duint i = 0; i + 1 < collected; i += 2)
                w[i / 2] = (wchar_t)((unsigned char)data[i] |
                                     ((wchar_t)(unsigned char)data[i + 1] << 8));
            w[collected / 2] = L'\0';
            int need = WideCharToMultiByte(CP_UTF8, 0, w, -1, NULL, 0, NULL, NULL);
            if(need > 1)
            {
                char* utf8 = (char*)malloc((size_t)need);
                if(utf8)
                {
                    WideCharToMultiByte(CP_UTF8, 0, w, -1, utf8, need, NULL, NULL);
                    json_object_set_new(item, "string", json_string(utf8));
                    free(utf8);
                }
            }
            free(w);
        }
    }
    else
    {
        /* 数据只含可打印 ASCII，天然是合法 UTF-8 */
        char* s = (char*)malloc((size_t)collected + 1);
        if(s)
        {
            memcpy(s, data, collected);
            s[collected] = '\0';
            json_object_set_new(item, "string", json_string(s));
            free(s);
        }
    }

    if(total > collected)
        json_object_set_new(item, "truncated", json_boolean(true));
    json_array_append_new(arr, item);
}

/* ================= 工具实现（第二批） ================= */

/* 读 NUL 结尾字符串（ascii / utf16） */
static char* tool_read_string(McpServer* server, json_t* args, bool* is_error)
{
    (void)server;
    if(!DbgIsDebugging())
    {
        *is_error = true;
        return tool_fail("No process is being debugged");
    }

    duint addr = 0;
    if(!parse_number(server, args, "address", &addr))
    {
        *is_error = true;
        return tool_fail("Parameter 'address' is required");
    }

    duint max_len = get_int(args, "max_length", 256);
    if(max_len < 1)
        max_len = 1;
    if(max_len > 0x10000)
        max_len = 0x10000;

    const char* enc = get_string(args, "encoding");
    bool utf16 = enc && strcmp(enc, "utf16") == 0;

    unsigned char raw[0x10000];
    duint len = 0;
    bool terminated = false;

    while(len < max_len)
    {
        unsigned char b;
        if(!DbgMemRead(addr + len, &b, 1))
            break;
        if(!utf16)
        {
            if(b == 0)
            {
                terminated = true;
                break;
            }
            raw[len++] = b;
        }
        else
        {
            if(len + 1 >= max_len)
                break;
            unsigned char b2;
            if(!DbgMemRead(addr + len + 1, &b2, 1))
                break;
            if(b == 0 && b2 == 0)
            {
                terminated = true;
                break;
            }
            raw[len++] = b;
            raw[len++] = b2;
        }
    }

    json_t* obj = json_object();
    json_object_set_new(obj, "address", mcp_json_hex(addr));
    json_object_set_new(obj, "encoding", json_string(utf16 ? "utf16" : "ascii"));
    json_object_set_new(obj, "byte_length", json_integer(len));
    json_object_set_new(obj, "terminated", json_boolean(terminated));

    if(len > 0)
    {
        if(utf16)
        {
            wchar_t* w = (wchar_t*)malloc((size_t)len + 2);
            if(w)
            {
                for(duint i = 0; i + 1 < len; i += 2)
                    w[i / 2] = (wchar_t)(raw[i] | ((wchar_t)raw[i + 1] << 8));
                w[len / 2] = L'\0';
                int need = WideCharToMultiByte(CP_UTF8, 0, w, -1, NULL, 0, NULL, NULL);
                if(need > 1)
                {
                    char* utf8 = (char*)malloc((size_t)need);
                    if(utf8)
                    {
                        WideCharToMultiByte(CP_UTF8, 0, w, -1, utf8, need, NULL, NULL);
                        json_object_set_new(obj, "string", json_string(utf8));
                        free(utf8);
                    }
                }
                free(w);
            }
        }
        else
        {
            char* s = (char*)malloc((size_t)len + 1);
            if(s)
            {
                for(duint i = 0; i < len; i++)
                    s[i] = (raw[i] >= 0x20 && raw[i] < 0x7f) ? (char)raw[i] : '.';
                s[len] = '\0';
                json_object_set_new(obj, "string", json_string(s));
                free(s);
            }
        }
    }
    else
    {
        json_object_set_new(obj, "string", json_string(""));
    }
    return tool_ok(obj);
}

/* 搜索模块/范围内的字符串（ASCII 或 UTF-16LE，跨页连续） */
static char* tool_find_strings(McpServer* server, json_t* args, bool* is_error)
{
    (void)server;
    if(!DbgIsDebugging())
    {
        *is_error = true;
        return tool_fail("No process is being debugged");
    }

    duint start = 0, end = 0;
    char* err = NULL;
    if(!resolve_search_range(server, args, &start, &end, &err))
    {
        *is_error = true;
        return err ? err : tool_fail("Failed to resolve search range");
    }

    duint min_len = get_int(args, "min_length", 4);
    if(min_len < 2)
        min_len = 2;
    if(min_len > 0x100)
        min_len = 0x100;

    duint max_results = get_int(args, "max_results", 100);
    if(max_results < 1)
        max_results = 1;
    if(max_results > 500)
        max_results = 500;

    duint max_str = get_int(args, "max_string", 256);
    if(max_str < 16)
        max_str = 16;
    if(max_str > 0x1000)
        max_str = 0x1000;

    const char* enc = get_string(args, "encoding");
    bool utf16 = enc && strcmp(enc, "utf16") == 0;

    json_t* obj = json_object();
    json_t* arr = json_array();

    /* 跨页延续状态 */
    duint run_start = 0;      /* 当前字符串绝对起始地址 */
    duint run_len = 0;        /* 当前字符串总长 */
    duint run_buf_len = 0;    /* run_buf 内字节数（不超过 max_str） */
    char run_buf[0x1000 + 1]; /* 缓冲（utf16 时为字节对） */
    unsigned char prev_lo = 0;
    bool have_prev = false;   /* utf16：上一页以奇数字节结尾 */

    duint pos = start;
    while(pos < end && json_array_size(arr) < max_results)
    {
        duint page_end = pos + 0x1000;
        if(page_end > end)
            page_end = end;
        duint want = page_end - pos;
        unsigned char buf[0x1000];

        if(!DbgMemRead(pos, buf, want))
        {
            /* 不可读页：结束当前字符串，跳过本页 */
            if(run_len >= min_len)
                find_strings_record(arr, run_start, run_buf, run_buf_len,
                                    run_len, utf16);
            run_len = 0;
            run_buf_len = 0;
            have_prev = false;
            pos = page_end;
            continue;
        }

        if(utf16)
        {
            duint i = 0;
            if(have_prev && want > 0)
            {
                /* 与上一页尾字节配对 */
                unsigned char b2 = buf[0];
                bool printable = (prev_lo >= 0x20 && prev_lo < 0x7f && b2 == 0);
                if(printable)
                {
                    if(run_len == 0)
                        run_start = pos - 1;
                    run_len += 2;
                    if(run_buf_len + 2 <= max_str)
                    {
                        run_buf[run_buf_len++] = (char)prev_lo;
                        run_buf[run_buf_len++] = '\0';
                    }
                }
                else if(run_len >= min_len)
                {
                    find_strings_record(arr, run_start, run_buf, run_buf_len,
                                        run_len, true);
                }
                run_len = 0;
                run_buf_len = 0;
                have_prev = false;
                i = 1;
            }
            for(; i + 1 < want && json_array_size(arr) < max_results; i += 2)
            {
                unsigned char lo = buf[i], hi = buf[i + 1];
                bool printable = (lo >= 0x20 && lo < 0x7f && hi == 0);
                if(printable)
                {
                    if(run_len == 0)
                        run_start = pos + i;
                    run_len += 2;
                    if(run_buf_len + 2 <= max_str)
                    {
                        run_buf[run_buf_len++] = (char)lo;
                        run_buf[run_buf_len++] = '\0';
                    }
                }
                else
                {
                    if(run_len >= min_len)
                        find_strings_record(arr, run_start, run_buf, run_buf_len,
                                            run_len, true);
                    run_len = 0;
                    run_buf_len = 0;
                }
            }
            if(i == want - 1)
            {
                have_prev = true;
                prev_lo = buf[i];
            }
        }
        else
        {
            for(duint i = 0; i < want && json_array_size(arr) < max_results; i++)
            {
                unsigned char b = buf[i];
                bool printable = (b >= 0x20 && b < 0x7f);
                if(printable)
                {
                    if(run_len == 0)
                        run_start = pos + i;
                    run_len++;
                    if(run_buf_len < max_str)
                        run_buf[run_buf_len++] = (char)b;
                }
                else
                {
                    if(run_len >= min_len)
                        find_strings_record(arr, run_start, run_buf, run_buf_len,
                                            run_len, false);
                    run_len = 0;
                    run_buf_len = 0;
                }
            }
        }
        pos = page_end;
    }

    /* 范围结束：关闭未闭合的字符串 */
    if(run_len >= min_len && json_array_size(arr) < max_results)
        find_strings_record(arr, run_start, run_buf, run_buf_len, run_len, utf16);

    json_object_set_new(obj, "scanned_from", mcp_json_hex(start));
    json_object_set_new(obj, "scanned_to", mcp_json_hex(end));
    json_object_set_new(obj, "count", json_integer((json_int_t)json_array_size(arr)));
    json_object_set_new(obj, "strings", arr);
    return tool_ok(obj);
}

/* 通配符字节模式搜索（? 表示任意字节），跨页匹配 */
static char* tool_find_pattern(McpServer* server, json_t* args, bool* is_error)
{
    (void)server;
    if(!DbgIsDebugging())
    {
        *is_error = true;
        return tool_fail("No process is being debugged");
    }

    const char* pattern = get_string(args, "pattern");
    if(!pattern || !pattern[0])
    {
        *is_error = true;
        return tool_fail("Parameter 'pattern' is required (e.g. \"E8 ?? ?? ?? ?? 90\")");
    }

    /* 解析模式：pat[] + mask[]（mask=0 表示通配） */
    unsigned char pat[64];
    unsigned char mask[64];
    int patlen = 0;
    {
        const char* p = pattern;
        while(*p)
        {
            while(*p == ' ' || *p == '\t' || *p == ',')
                p++;
            if(!*p)
                break;
            if(patlen >= 64)
            {
                *is_error = true;
                return tool_fail("Pattern too long (max 64 bytes)");
            }
            if(*p == '?')
            {
                pat[patlen] = 0;
                mask[patlen] = 0;
                p++;
                if(*p == '?')
                    p++;
                patlen++;
                continue;
            }
            int hi = -1, lo = -1;
            if(*p >= '0' && *p <= '9')
                hi = *p - '0';
            else if(*p >= 'a' && *p <= 'f')
                hi = *p - 'a' + 10;
            else if(*p >= 'A' && *p <= 'F')
                hi = *p - 'A' + 10;
            if(hi >= 0 && p[1])
            {
                if(p[1] >= '0' && p[1] <= '9')
                    lo = p[1] - '0';
                else if(p[1] >= 'a' && p[1] <= 'f')
                    lo = p[1] - 'a' + 10;
                else if(p[1] >= 'A' && p[1] <= 'F')
                    lo = p[1] - 'A' + 10;
            }
            if(hi < 0 || lo < 0)
            {
                *is_error = true;
                return tool_fail("Invalid pattern syntax near '%s'", p);
            }
            pat[patlen] = (unsigned char)((hi << 4) | lo);
            mask[patlen] = 1;
            patlen++;
            p += 2;
        }
    }
    if(patlen == 0)
    {
        *is_error = true;
        return tool_fail("Empty pattern");
    }

    duint start = 0, end = 0;
    char* err = NULL;
    if(!resolve_search_range(server, args, &start, &end, &err))
    {
        *is_error = true;
        return err ? err : tool_fail("Failed to resolve search range");
    }

    duint max_results = get_int(args, "max_results", 10);
    if(max_results < 1)
        max_results = 1;
    if(max_results > 100)
        max_results = 100;

    json_t* obj = json_object();
    json_t* arr = json_array();

    unsigned char carry[64];
    size_t carry_len = 0;
    duint pos = start;
    duint found = 0;

    while(pos < end && found < max_results)
    {
        duint page_end = pos + 0x1000;
        if(page_end > end)
            page_end = end;
        duint want = page_end - pos;
        unsigned char buf[0x1000];

        if(!DbgMemRead(pos, buf, want))
        {
            carry_len = 0;
            pos = page_end;
            continue;
        }

        size_t clen = carry_len + want;
        unsigned char* comb = (unsigned char*)malloc(clen ? clen : 1);
        if(!comb)
            break;
        if(carry_len)
            memcpy(comb, carry, carry_len);
        memcpy(comb + carry_len, buf, want);

        /* 接受规则：匹配完全落在 combined 内，且不全在 carry 区
         * （全在 carry 区内的匹配已在上页统计过） */
        for(size_t o = 0; o + (size_t)patlen <= clen; o++)
        {
            if(o + (size_t)patlen <= carry_len)
                continue;
            bool m = true;
            for(int k = 0; k < patlen; k++)
            {
                if(mask[k] && comb[o + k] != pat[k])
                {
                    m = false;
                    break;
                }
            }
            if(m)
            {
                json_t* item = json_object();
                json_object_set_new(item, "address",
                                    mcp_json_hex(pos - (duint)carry_len + (duint)o));
                json_array_append_new(arr, item);
                found++;
                if(found >= max_results)
                    break;
            }
        }
        free(comb);

        /* 新 carry = 本页最后 patlen-1 字节（跨页匹配用） */
        size_t cl = (size_t)patlen - 1;
        if(cl > want)
            cl = want;
        if(cl > 0)
            memcpy(carry, buf + want - cl, cl);
        carry_len = cl;

        pos = page_end;
    }

    json_object_set_new(obj, "pattern", json_string(pattern));
    json_object_set_new(obj, "length", json_integer(patlen));
    json_object_set_new(obj, "scanned_from", mcp_json_hex(start));
    json_object_set_new(obj, "scanned_to", mcp_json_hex(end));
    json_object_set_new(obj, "count", json_integer((json_int_t)json_array_size(arr)));
    json_object_set_new(obj, "matches", arr);
    return tool_ok(obj);
}

/* 内存映射（页面级区域） */
static char* tool_get_memory_map(McpServer* server, json_t* args, bool* is_error)
{
    (void)server;
    (void)args;
    if(!DbgIsDebugging())
    {
        *is_error = true;
        return tool_fail("No process is being debugged");
    }

    MEMMAP mm;
    memset(&mm, 0, sizeof(mm));
    if(!DbgMemMap(&mm) || mm.count <= 0)
    {
        *is_error = true;
        return tool_fail("Failed to query memory map");
    }

    json_t* obj = json_object();
    json_t* arr = json_array();
    for(int i = 0; i < mm.count; i++)
    {
        MEMPAGE* p = &mm.page[i];
        json_t* item = json_object();
        duint base = (duint)p->mbi.BaseAddress;
        duint size = (duint)p->mbi.RegionSize;

        json_object_set_new(item, "base", mcp_json_hex(base));
        json_object_set_new(item, "size", mcp_json_hex(size));
        json_object_set_new(item, "end", mcp_json_hex(base + size));

        json_object_set_new(item, "state",
            json_string(p->mbi.State == MEM_COMMIT ? "committed"
                       : (p->mbi.State == MEM_RESERVE ? "reserved" : "free")));
        json_object_set_new(item, "type",
            json_string(p->mbi.Type == MEM_IMAGE ? "image"
                       : (p->mbi.Type == MEM_MAPPED ? "mapped" : "private")));

        const char* pname = "---";
        switch(p->mbi.Protect & 0xFF)
        {
        case PAGE_EXECUTE:        pname = "--x"; break;
        case PAGE_EXECUTE_READ:   pname = "r-x"; break;
        case PAGE_EXECUTE_READWRITE:
        case PAGE_EXECUTE_WRITECOPY: pname = "rwx"; break;
        case PAGE_READONLY:       pname = "r--"; break;
        case PAGE_READWRITE:
        case PAGE_WRITECOPY:      pname = "rw-"; break;
        case PAGE_NOACCESS:       pname = "---"; break;
        default:                  pname = "???"; break;
        }
        json_object_set_new(item, "protect", json_string(pname));

        if(p->info[0])
            json_object_set_new(item, "info", json_string(p->info));
        json_array_append_new(arr, item);
    }
    if(mm.page)
        BridgeFree(mm.page);

    json_object_set_new(obj, "count", json_integer((json_int_t)json_array_size(arr)));
    json_object_set_new(obj, "regions", arr);
    return tool_ok(obj);
}

/* 设置/清除注释（text 为空串时清除） */
static char* tool_set_comment(McpServer* server, json_t* args, bool* is_error)
{
    (void)server;
    char* err = NULL;
    if(!ensure_paused(&err))
    {
        *is_error = true;
        return err;
    }

    duint addr = 0;
    if(!parse_number(server, args, "address", &addr))
    {
        *is_error = true;
        return tool_fail("Parameter 'address' is required");
    }
    const char* text = get_string(args, "text");
    if(!text)
        text = "";

    if(!DbgSetCommentAt(addr, text))
    {
        *is_error = true;
        return tool_fail("Failed to set comment (debuggee must be paused)");
    }

    json_t* obj = json_object();
    json_object_set_new(obj, "success", json_true());
    json_object_set_new(obj, "address", mcp_json_hex(addr));
    json_object_set_new(obj, "text", json_string(text));
    return tool_ok(obj);
}

/* 设置/清除标签（text 为空串时清除） */
static char* tool_set_label(McpServer* server, json_t* args, bool* is_error)
{
    (void)server;
    char* err = NULL;
    if(!ensure_paused(&err))
    {
        *is_error = true;
        return err;
    }

    duint addr = 0;
    if(!parse_number(server, args, "address", &addr))
    {
        *is_error = true;
        return tool_fail("Parameter 'address' is required");
    }
    const char* text = get_string(args, "text");
    if(!text)
        text = "";

    if(!DbgSetLabelAt(addr, text))
    {
        *is_error = true;
        return tool_fail("Failed to set label (debuggee must be paused)");
    }

    json_t* obj = json_object();
    json_object_set_new(obj, "success", json_true());
    json_object_set_new(obj, "address", mcp_json_hex(addr));
    json_object_set_new(obj, "text", json_string(text));
    return tool_ok(obj);
}

/* 打开文件开始调试（init，不自动运行） */
static char* tool_open_file(McpServer* server, json_t* args, bool* is_error)
{
    (void)server;
    const char* path = get_string(args, "path");
    if(!path || !path[0])
    {
        *is_error = true;
        return tool_fail("Parameter 'path' is required");
    }
    if(GetFileAttributesA(path) == INVALID_FILE_ATTRIBUTES)
    {
        *is_error = true;
        return tool_fail("File not found: %s", path);
    }
    if(DbgIsDebugging())
    {
        *is_error = true;
        return tool_fail("A process is already being debugged; stop it first");
    }

    char cmd[MAX_PATH + 16];
    _snprintf(cmd, sizeof(cmd), "init \"%s\"", path);
    if(!DbgCmdExecDirect(cmd))
    {
        *is_error = true;
        return tool_fail("Failed to open file");
    }

    json_t* obj = json_object();
    json_object_set_new(obj, "success", json_true());
    json_object_set_new(obj, "path", json_string(path));
    json_object_set_new(obj, "message",
                        json_string("File loaded; use x64dbg_run to start execution"));
    return tool_ok(obj);
}

/* 附加到运行中的进程 */
static char* tool_attach(McpServer* server, json_t* args, bool* is_error)
{
    (void)server;
    duint pid = 0;
    if(!parse_number(server, args, "pid", &pid) || pid == 0)
    {
        *is_error = true;
        return tool_fail("Parameter 'pid' is required");
    }
    if(DbgIsDebugging())
    {
        *is_error = true;
        return tool_fail("A process is already being debugged; stop it first");
    }

    char cmd[64];
    if(sizeof(duint) == 8)
        _snprintf(cmd, sizeof(cmd), "attach %llu", (unsigned long long)pid);
    else
        _snprintf(cmd, sizeof(cmd), "attach %lu", (unsigned long)pid);

    if(!DbgCmdExecDirect(cmd))
    {
        *is_error = true;
        return tool_fail("Failed to attach to pid %Iu", (size_t)pid);
    }

    json_t* obj = json_object();
    json_object_set_new(obj, "success", json_true());
    json_object_set_new(obj, "pid", json_integer(pid));
    json_object_set_new(obj, "message", json_string("Attach requested"));
    return tool_ok(obj);
}

/* ================= 工具注册 ================= */

void tools_register_all(McpServer* server);

void tools_register_all(McpServer* server)
{
    static const McpTool tools[] = {
        {
            "x64dbg_get_state",
            "获取 x64dbg 调试器当前状态（是否在调试、运行/暂停、PID/TID、CIP）。",
            "{\"type\":\"object\",\"properties\":{}}",
            tool_get_state,
        },
        {
            "x64dbg_run",
            "继续运行被调试程序（可选指定运行到某地址后暂停）。",
            "{\"type\":\"object\",\"properties\":{\"address\":{\"type\":\"string\",\"description\":\"可选：表达式或地址，运行到此地址后暂停（run-to-cursor）\"}},\"additionalProperties\":false}",
            tool_run,
        },
        {
            "x64dbg_pause",
            "暂停正在运行的程序。",
            "{\"type\":\"object\",\"properties\":{}}",
            tool_pause,
        },
        {
            "x64dbg_restart",
            "重启当前被调试程序。",
            "{\"type\":\"object\",\"properties\":{}}",
            tool_restart,
        },
        {
            "x64dbg_stop",
            "停止调试（终止调试会话）。",
            "{\"type\":\"object\",\"properties\":{}}",
            tool_stop,
        },
        {
            "x64dbg_step_into",
            "单步步入（F7）：执行一条指令，进入 call。",
            "{\"type\":\"object\",\"properties\":{}}",
            tool_step_into,
        },
        {
            "x64dbg_step_over",
            "单步步过（F8）：执行一条指令，跳过 call。",
            "{\"type\":\"object\",\"properties\":{}}",
            tool_step_over,
        },
        {
            "x64dbg_step_out",
            "步出（Ctrl+F8）：执行直到当前函数返回。",
            "{\"type\":\"object\",\"properties\":{}}",
            tool_step_out,
        },
        {
            "x64dbg_set_breakpoint",
            "设置断点。type 可选 normal(默认)/hardware/memory/dll/exception；dll 用 name 传模块名，exception 用 address 传异常码。",
            "{\"type\":\"object\",\"properties\":{\"address\":{\"type\":\"string\",\"description\":\"表达式或地址，如 eip、0x401000、模块名+0x10\"},\"type\":{\"type\":\"string\",\"enum\":[\"normal\",\"hardware\",\"memory\",\"dll\",\"exception\"]},\"name\":{\"type\":\"string\",\"description\":\"dll 断点的模块名\"}},\"additionalProperties\":false}",
            tool_set_breakpoint,
        },
        {
            "x64dbg_delete_breakpoint",
            "删除断点。参数同 set_breakpoint。",
            "{\"type\":\"object\",\"properties\":{\"address\":{\"type\":\"string\",\"description\":\"表达式或地址\"},\"type\":{\"type\":\"string\",\"enum\":[\"normal\",\"hardware\",\"memory\",\"dll\",\"exception\"]},\"name\":{\"type\":\"string\",\"description\":\"dll 断点的模块名\"}},\"additionalProperties\":false}",
            tool_delete_breakpoint,
        },
        {
            "x64dbg_list_breakpoints",
            "列出所有断点（普通/硬件/内存/DLL/异常）。",
            "{\"type\":\"object\",\"properties\":{}}",
            tool_list_breakpoints,
        },
        {
            "x64dbg_read_memory",
            "读取被调试进程内存，返回 hex 和 ascii 视图。地址支持表达式。",
            "{\"type\":\"object\",\"properties\":{\"address\":{\"type\":\"string\",\"description\":\"表达式或地址，如 0x401000、[eip]\"},\"size\":{\"type\":\"integer\",\"description\":\"读取字节数，默认 64，最大 4096\"}},\"required\":[\"address\"],\"additionalProperties\":false}",
            tool_read_memory,
        },
        {
            "x64dbg_write_memory",
            "写入被调试进程内存。hex 为十六进制字符串（如 909090），或 bytes 为字节数组。",
            "{\"type\":\"object\",\"properties\":{\"address\":{\"type\":\"string\",\"description\":\"表达式或地址\"},\"hex\":{\"type\":\"string\",\"description\":\"十六进制字节串，如 '909090'\"},\"bytes\":{\"type\":\"array\",\"items\":{\"type\":\"integer\"},\"description\":\"字节数组，如 [0x90,0x90]\"}},\"required\":[\"address\"],\"additionalProperties\":false}",
            tool_write_memory,
        },
        {
            "x64dbg_read_registers",
            "读取全部通用寄存器（cax/ccx/cdx/cbx/csp/cbp/csi/cdi/cip/eflags/dr0-dr7，x64 含 r8-r15）。需暂停。",
            "{\"type\":\"object\",\"properties\":{}}",
            tool_read_registers,
        },
        {
            "x64dbg_write_register",
            "写入寄存器值（如 cax、eip、eflags）。value 可为数值或表达式。需暂停。",
            "{\"type\":\"object\",\"properties\":{\"name\":{\"type\":\"string\",\"description\":\"寄存器名，如 cax/cbx/eip/eflags/r8\"},\"value\":{\"type\":\"string\",\"description\":\"值或表达式，如 0x401000\"}},\"required\":[\"name\",\"value\"],\"additionalProperties\":false}",
            tool_write_register,
        },
        {
            "x64dbg_disassemble",
            "反汇编指定地址（默认从 cip 开始，需暂停），返回指令文本、长度、字节、跳转目标。",
            "{\"type\":\"object\",\"properties\":{\"address\":{\"type\":\"string\",\"description\":\"表达式或地址；缺省时从 cip 开始\"},\"count\":{\"type\":\"integer\",\"description\":\"反汇编条数，默认 8，最大 64\"}},\"additionalProperties\":false}",
            tool_disassemble,
        },
        {
            "x64dbg_assemble",
            "在指定地址写入汇编指令（如 nop、jmp eax）。需暂停。",
            "{\"type\":\"object\",\"properties\":{\"address\":{\"type\":\"string\",\"description\":\"表达式或地址\"},\"instruction\":{\"type\":\"string\",\"description\":\"汇编指令，如 'nop'、'jmp eax'\"}},\"required\":[\"address\",\"instruction\"],\"additionalProperties\":false}",
            tool_assemble,
        },
        {
            "x64dbg_eval",
            "求值 x64dbg 表达式（寄存器、内存、标签、API 名、算术），返回十六进制/十进制值。",
            "{\"type\":\"object\",\"properties\":{\"expression\":{\"type\":\"string\",\"description\":\"表达式，如 eip、[esp+4]、kernel32.CreateFileA、1+2\"}},\"required\":[\"expression\"],\"additionalProperties\":false}",
            tool_eval,
        },
        {
            "x64dbg_command",
            "执行任意 x64dbg 命令（如 dump、disasm、memset、find 等）。sync=false 时异步执行。",
            "{\"type\":\"object\",\"properties\":{\"command\":{\"type\":\"string\",\"description\":\"x64dbg 命令，如 'dump 0x401000'\"},\"sync\":{\"type\":\"boolean\",\"description\":\"是否同步等待执行完成，默认 true\"}},\"required\":[\"command\"],\"additionalProperties\":false}",
            tool_command,
        },
        {
            "x64dbg_get_modules",
            "列出被调试进程加载的模块（名称、基址、大小）。",
            "{\"type\":\"object\",\"properties\":{}}",
            tool_get_modules,
        },
        {
            "x64dbg_get_threads",
            "列出被调试进程的线程（TID、CIP、挂起计数）。",
            "{\"type\":\"object\",\"properties\":{}}",
            tool_get_threads,
        },
        {
            "x64dbg_get_callstack",
            "获取当前调用栈。需暂停。",
            "{\"type\":\"object\",\"properties\":{}}",
            tool_get_callstack,
        },
        {
            "x64dbg_read_string",
            "从地址读取 NUL 结尾字符串（ascii 或 utf16），返回文本与字节长度。",
            "{\"type\":\"object\",\"properties\":{\"address\":{\"type\":\"string\",\"description\":\"表达式或地址\"},\"encoding\":{\"type\":\"string\",\"enum\":[\"ascii\",\"utf16\"],\"description\":\"编码，默认 ascii\"},\"max_length\":{\"type\":\"integer\",\"description\":\"最大字节数，默认 256，最大 65536\"}},\"required\":[\"address\"],\"additionalProperties\":false}",
            tool_read_string,
        },
        {
            "x64dbg_find_strings",
            "在模块或地址范围内搜索字符串（ASCII/UTF-16LE），返回地址与文本。",
            "{\"type\":\"object\",\"properties\":{\"module\":{\"type\":\"string\",\"description\":\"模块名，如 kernel32.dll\"},\"address\":{\"type\":\"string\",\"description\":\"起始地址（与 module 二选一；缺省用 cip 所在页）\"},\"encoding\":{\"type\":\"string\",\"enum\":[\"ascii\",\"utf16\"],\"description\":\"默认 ascii\"},\"min_length\":{\"type\":\"integer\",\"description\":\"最短字符串长度，默认 4\"},\"max_results\":{\"type\":\"integer\",\"description\":\"最多返回条数，默认 100，最大 500\"},\"max_string\":{\"type\":\"integer\",\"description\":\"单条字符串最长字节数，默认 256\"},\"max_size\":{\"type\":\"integer\",\"description\":\"最大扫描字节数，默认扫描整个范围\"}},\"additionalProperties\":false}",
            tool_find_strings,
        },
        {
            "x64dbg_find_pattern",
            "在模块或地址范围内搜索字节模式（? 通配任意字节），如 'E8 ?? ?? ?? ?? 90'。",
            "{\"type\":\"object\",\"properties\":{\"pattern\":{\"type\":\"string\",\"description\":\"字节模式，? 为通配符，如 'E8 ?? ?? ?? ?? 90'\"},\"module\":{\"type\":\"string\",\"description\":\"模块名\"},\"address\":{\"type\":\"string\",\"description\":\"起始地址（与 module 二选一；缺省用 cip 所在页）\"},\"max_results\":{\"type\":\"integer\",\"description\":\"最多返回条数，默认 10，最大 100\"},\"max_size\":{\"type\":\"integer\",\"description\":\"最大扫描字节数\"}},\"required\":[\"pattern\"],\"additionalProperties\":false}",
            tool_find_pattern,
        },
        {
            "x64dbg_get_memory_map",
            "列出被调试进程的内存区域（基址、大小、状态、类型、保护属性）。",
            "{\"type\":\"object\",\"properties\":{}}",
            tool_get_memory_map,
        },
        {
            "x64dbg_set_comment",
            "在地址处设置注释（text 为空字符串时清除）。需暂停。",
            "{\"type\":\"object\",\"properties\":{\"address\":{\"type\":\"string\",\"description\":\"表达式或地址\"},\"text\":{\"type\":\"string\",\"description\":\"注释文本；空字符串清除\"}},\"required\":[\"address\",\"text\"],\"additionalProperties\":false}",
            tool_set_comment,
        },
        {
            "x64dbg_set_label",
            "在地址处设置标签（text 为空字符串时清除）。需暂停。",
            "{\"type\":\"object\",\"properties\":{\"address\":{\"type\":\"string\",\"description\":\"表达式或地址\"},\"text\":{\"type\":\"string\",\"description\":\"标签名；空字符串清除\"}},\"required\":[\"address\",\"text\"],\"additionalProperties\":false}",
            tool_set_label,
        },
        {
            "x64dbg_open_file",
            "在 x64dbg 中打开一个可执行文件开始调试（不自动运行，之后可设断点并 x64dbg_run）。",
            "{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\",\"description\":\"可执行文件完整路径\"}},\"required\":[\"path\"],\"additionalProperties\":false}",
            tool_open_file,
        },
        {
            "x64dbg_attach",
            "附加到指定 PID 的进程进行调试。",
            "{\"type\":\"object\",\"properties\":{\"pid\":{\"type\":\"integer\",\"description\":\"目标进程 PID\"}},\"required\":[\"pid\"],\"additionalProperties\":false}",
            tool_attach,
        },
    };

    for(size_t i = 0; i < sizeof(tools) / sizeof(tools[0]); i++)
        mcp_server_register_tool(server, &tools[i]);
}

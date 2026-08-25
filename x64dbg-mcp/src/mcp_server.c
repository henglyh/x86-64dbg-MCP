/*
 * x64dbg-mcp: Model Context Protocol 服务器插件
 *
 * mcp_server.c - HTTP + SSE 传输层与 JSON-RPC 分发
 *
 * 传输层遵循 MCP "HTTP with SSE" 规范：
 *   - GET  /sse                      建立 SSE 事件流，返回 endpoint（session 注册）
 *   - POST /messages?session_id=xxx  提交 JSON-RPC 请求，响应经 SSE 流回发
 *
 * 实现采用纯 Win32 (Winsock) + jansson，无第三方依赖；
 * 所有网络 I/O 均在插件自有线程中完成，不阻塞 x64dbg 的 GUI 线程。
 */
#include "mcp.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

#include <wincrypt.h>

#include <jansson.h>

/* ---------- 调试日志（默认关闭） ----------
 * 设置环境变量 X64DBG_MCP_DEBUG=1 开启；
 * 日志默认写到插件 DLL 所在目录的 mcp_dbg.log（可被 X64DBG_MCP_LOG 覆盖），
 * 单文件超过 1MB 自动轮转（删除后重建）。
 */
static int mcp_debug_enabled(void)
{
    static int cached = -1;
    if(cached < 0)
    {
        char buf[8] = "";
        DWORD n = GetEnvironmentVariableA("X64DBG_MCP_DEBUG", buf, sizeof(buf));
        cached = (n > 0 && n < sizeof(buf) && buf[0] == '1') ? 1 : 0;
    }
    return cached;
}

static void mcp_dbg_log(const char* fmt, ...)
{
    if(!mcp_debug_enabled())
        return;

    static char path[MAX_PATH] = "";
    if(!path[0])
    {
        if(!GetEnvironmentVariableA("X64DBG_MCP_LOG", path, MAX_PATH) || !path[0])
        {
            /* 默认写插件 DLL 所在目录 */
            HMODULE self = NULL;
            if(GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                  GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                  (LPCSTR)(uintptr_t)&mcp_dbg_log, &self) &&
               self && GetModuleFileNameA(self, path, MAX_PATH))
            {
                char* slash = strrchr(path, '\\');
                if(slash)
                    slash[1] = '\0';
                strncat(path, "mcp_dbg.log", sizeof(path) - strlen(path) - 1);
            }
            else
                strcpy(path, "mcp_dbg.log");
        }
    }

    FILE* f = fopen(path, "ab");
    if(!f)
        return;
    fseek(f, 0, SEEK_END);
    if(ftell(f) > 1024 * 1024)
    {
        fclose(f);
        DeleteFileA(path);
        f = fopen(path, "ab");
        if(!f)
            return;
    }
    va_list ap;
    va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fputs("\n", f);
    fclose(f);
}

/* ---------- 工具表 ---------- */
typedef struct
{
    McpTool tool;
    bool registered;
} ToolSlot;

#define MCP_MAX_CONNS 64

typedef struct ConnEntry
{
    SOCKET sock;
    HANDLE thread;
    bool used;
} ConnEntry;

typedef struct SseClient
{
    SOCKET sock;
    char session_id[MCP_SESSION_ID_LEN];
    struct SseClient* next;
} SseClient;

struct McpServer
{
    volatile LONG running;
    int port;

    SOCKET listen_sock;
    HANDLE listen_thread;
    HANDLE stop_event;

    /* 工具表 */
    ToolSlot tools[MCP_MAX_TOOLS];
    int tool_count;
    CRITICAL_SECTION tools_lock;

    /* SSE 客户端列表（按 session_id 查找，用于回发响应） */
    struct SseClient* clients;
    CRITICAL_SECTION clients_lock;

    /* 全部活动连接（socket + 处理线程），用于停机时安全回收 */
    struct ConnEntry entries[MCP_MAX_CONNS];
    CRITICAL_SECTION entries_lock;
};

/* ---------- 内部工具函数 ---------- */

static void* xmalloc(size_t size)
{
    void* p = malloc(size);
    if(!p)
        abort();
    return p;
}

/* 登记连接线程（handle_connection 入口调用） */
static void conn_register(McpServer* server, SOCKET sock)
{
    EnterCriticalSection(&server->entries_lock);
    for(int i = 0; i < MCP_MAX_CONNS; i++)
    {
        if(!server->entries[i].used)
        {
            server->entries[i].sock = sock;
            server->entries[i].thread = OpenThread(THREAD_ALL_ACCESS, FALSE,
                                                   GetCurrentThreadId());
            server->entries[i].used = true;
            break;
        }
    }
    LeaveCriticalSection(&server->entries_lock);
}

/* 注销连接线程（handle_connection 退出前调用） */
static void conn_unregister(McpServer* server, SOCKET sock)
{
    EnterCriticalSection(&server->entries_lock);
    for(int i = 0; i < MCP_MAX_CONNS; i++)
    {
        if(server->entries[i].used && server->entries[i].sock == sock)
        {
            if(server->entries[i].thread)
                CloseHandle(server->entries[i].thread);
            server->entries[i].used = false;
            break;
        }
    }
    LeaveCriticalSection(&server->entries_lock);
}

/* 停机：唤醒并等待全部连接线程退出 */
static void conn_shutdown_all(McpServer* server)
{
    HANDLE handles[MCP_MAX_CONNS];
    int count = 0;

    EnterCriticalSection(&server->entries_lock);
    for(int i = 0; i < MCP_MAX_CONNS; i++)
    {
        if(server->entries[i].used)
        {
            shutdown(server->entries[i].sock, SD_BOTH);
            if(server->entries[i].thread && count < MCP_MAX_CONNS)
                handles[count++] = server->entries[i].thread;
        }
    }
    LeaveCriticalSection(&server->entries_lock);

    if(count > 0)
    {
        for(int i = 0; i < count; i += MAXIMUM_WAIT_OBJECTS)
        {
            DWORD n = (count - i) > MAXIMUM_WAIT_OBJECTS ? MAXIMUM_WAIT_OBJECTS
                                                         : (DWORD)(count - i);
            WaitForMultipleObjects(n, handles + i, TRUE, 5000);
        }
    }
}

/* 生成随机 session id（十六进制） */
static void make_session_id(char* out, size_t outlen)
{
    HCRYPTPROV prov = 0;
    unsigned char buf[16];
    DWORD i;

    if(!CryptAcquireContextW(&prov, NULL, NULL, PROV_RSA_FULL,
                             CRYPT_VERIFYCONTEXT | CRYPT_SILENT))
    {
        /* 退化为时间+计数器 */
        LARGE_INTEGER pc;
        QueryPerformanceCounter(&pc);
        _snprintf(out, outlen, "x64dbg%08lx%08lx", (DWORD)GetTickCount(),
                  (DWORD)pc.LowPart);
        return;
    }
    CryptGenRandom(prov, sizeof(buf), buf);
    CryptReleaseContext(prov, 0);
    for(i = 0; i < sizeof(buf) && (size_t)i * 2 + 1 < outlen; i++)
        sprintf(out + i * 2, "%02x", buf[i]);
    out[outlen - 1] = '\0';
}

/* 查找 session 对应的 SSE 客户端（调用方须持有 clients_lock） */
static SseClient* find_client(McpServer* server, const char* session_id)
{
    SseClient* c;
    for(c = server->clients; c; c = c->next)
    {
        if(strcmp(c->session_id, session_id) == 0)
            return c;
    }
    return NULL;
}

/* 移除并关闭一个客户端（调用方须持有 clients_lock） */
static void remove_client(McpServer* server, SseClient* client)
{
    SseClient** pp = &server->clients;
    while(*pp)
    {
        if(*pp == client)
        {
            *pp = client->next;
            shutdown(client->sock, SD_BOTH);
            closesocket(client->sock);
            free(client);
            return;
        }
        pp = &(*pp)->next;
    }
}

/*
 * 向 SSE 连接发送一个事件。
 * max_retry 控制 WSAEWOULDBLOCK（发送缓冲满）时的重试次数，
 * 避免对端不读取时无限阻塞（通知路径用 0，响应路径用 10）。
 * 失败时返回 false，调用方应断开该连接。
 */
static bool sse_send_event(McpServer* server, SseClient* client,
                           const char* event, const char* data, int max_retry)
{
    char header[64];
    int hlen = sprintf(header, "event: %s\r\ndata: ", event);
    int dlen = (int)strlen(data);
    char* frame = (char*)xmalloc((size_t)hlen + (size_t)dlen + 4);
    char* p = frame;

    memcpy(p, header, (size_t)hlen);
    p += hlen;
    memcpy(p, data, (size_t)dlen);
    p += dlen;
    memcpy(p, "\r\n\r\n", 4);

    /* 非阻塞发送，避免对端不读时阻塞线程 */
    u_long nonblock = 1;
    ioctlsocket(client->sock, FIONBIO, &nonblock);

    int total = 0;
    int sent;
    int retries = 0;
    do
    {
        sent = send(client->sock, frame + total, hlen + dlen + 4 - total, 0);
        if(sent == SOCKET_ERROR)
        {
            int err = WSAGetLastError();
            if(err == WSAEWOULDBLOCK)
            {
                if(retries++ >= max_retry)
                {
                    free(frame);
                    return false;
                }
                Sleep(50);
                continue;
            }
            free(frame);
            return false;
        }
        total += sent;
    }
    while(total < hlen + dlen + 4 && server->running);

    free(frame);
    return total == hlen + dlen + 4;
}

/* 广播一条 JSON-RPC 消息（响应或通知）到所有 SSE 客户端 */
static void sse_broadcast(McpServer* server, const char* json_text, int max_retry)
{
    SseClient* to_drop[64];
    int drop_count = 0;

    EnterCriticalSection(&server->clients_lock);
    SseClient* c = server->clients;
    while(c)
    {
        SseClient* next = c->next;
        if(!sse_send_event(server, c, "message", json_text, max_retry))
        {
            if(drop_count < 64)
                to_drop[drop_count++] = c;
        }
        c = next;
    }
    for(int i = 0; i < drop_count; i++)
        remove_client(server, to_drop[i]);
    LeaveCriticalSection(&server->clients_lock);
}

/* ---------- HTTP 解析 ---------- */

typedef struct
{
    char method[16];
    char path[512];
    char query[512];
    size_t content_length;
    const char* body; /* 指向接收缓冲（不拷贝） */
} HttpRequest;

/*
 * 简单 HTTP 请求头解析。
 * buf/len 为完整请求（含 body）。成功返回 true 并填充 req。
 */
static bool http_parse_request(const char* buf, size_t len, HttpRequest* req)
{
    const char* p = buf;
    const char* end = buf + len;
    const char* line_end;

    /* 请求行 */
    line_end = (const char*)memchr(p, '\n', (size_t)(end - p));
    if(!line_end)
        return false;
    {
        size_t ll = (size_t)(line_end - p);
        char line[1024];
        char* sp1;
        char* sp2;
        if(ll >= sizeof(line))
            return false;
        memcpy(line, p, ll);
        line[ll] = '\0';
        if(line[ll - 1] == '\r')
            line[ll - 1] = '\0';

        sp1 = strchr(line, ' ');
        if(!sp1)
            return false;
        *sp1 = '\0';
        strncpy(req->method, line, sizeof(req->method) - 1);
        req->method[sizeof(req->method) - 1] = '\0';

        sp1++;
        sp2 = strchr(sp1, ' ');
        if(sp2)
            *sp2 = '\0';

        /* 拆分 path 与 query */
        char* q = strchr(sp1, '?');
        if(q)
        {
            *q = '\0';
            strncpy(req->query, q + 1, sizeof(req->query) - 1);
            req->query[sizeof(req->query) - 1] = '\0';
        }
        else
        {
            req->query[0] = '\0';
        }
        strncpy(req->path, sp1, sizeof(req->path) - 1);
        req->path[sizeof(req->path) - 1] = '\0';
    }

    req->content_length = 0;
    p = line_end + 1;
    while(p < end)
    {
        line_end = (const char*)memchr(p, '\n', (size_t)(end - p));
        if(!line_end)
            break;
        size_t ll = (size_t)(line_end - p);
        if(ll == 1 || (ll == 2 && p[0] == '\r'))
        {
            /* 空行：头部结束 */
            p = line_end + 1;
            break;
        }
        if(ll > 2)
        {
            if(_strnicmp(p, "content-length:", 15) == 0)
            {
                req->content_length = (size_t)strtoul(p + 15, NULL, 10);
            }
        }
        p = line_end + 1;
    }

    if(p > end)
        return false;
    req->body = p;
    if(req->content_length > (size_t)(end - p))
        req->content_length = (size_t)(end - p);
    return true;
}

/* 在头部区查找指定头（大小写不敏感），返回头部值起始位置 */
static const char* http_find_header(const char* buf, const char* hdr_end,
                                    const char* name)
{
    const char* p = buf;
    size_t namelen = strlen(name);
    while(p < hdr_end)
    {
        const char* eol = strstr(p, "\r\n");
        if(!eol || eol > hdr_end)
            break;
        if((size_t)(eol - p) > namelen &&
           _strnicmp(p, name, namelen) == 0 &&
           p[namelen] == ':')
            return p + namelen + 1;
        p = eol + 2;
    }
    return NULL;
}

/* 发送 HTTP 响应并关闭连接 */
static void http_send_response(SOCKET sock, int status, const char* status_text,
                               const char* content_type, const char* body,
                               size_t body_len)
{
    char head[512];
    int head_len = sprintf(head,
                           "HTTP/1.1 %d %s\r\n"
                           "Content-Type: %s\r\n"
                           "Content-Length: %zu\r\n"
                           "Connection: close\r\n"
                           "Cache-Control: no-cache\r\n"
                           "Access-Control-Allow-Origin: *\r\n"
                           "Access-Control-Allow-Headers: Content-Type\r\n"
                           "Access-Control-Allow-Methods: GET, POST, OPTIONS\r\n"
                           "\r\n",
                           status, status_text, content_type, body_len);
    send(sock, head, head_len, 0);
    if(body && body_len)
        send(sock, body, (int)body_len, 0);
}

/* ---------- JSON-RPC ---------- */

static void jsonrpc_send_response(McpServer* server, const char* session_id,
                                  const json_t* result, const json_t* error,
                                  const json_t* id)
{
    json_t* resp = json_object();
    json_object_set_new(resp, "jsonrpc", json_string("2.0"));
    if(id && json_is_number(id))
        json_object_set(resp, "id", (json_t*)id);
    else if(id && json_is_string(id))
        json_object_set(resp, "id", (json_t*)id);
    else
        json_object_set_new(resp, "id", json_null());

    if(error)
        json_object_set(resp, "error", (json_t*)error);
    else if(result)
        json_object_set(resp, "result", (json_t*)result);

    char* text = json_dumps(resp, JSON_COMPACT | JSON_ENSURE_ASCII);
    if(!text)
    {
        json_decref(resp);
        return;
    }

    if(session_id && session_id[0])
    {
        EnterCriticalSection(&server->clients_lock);
        SseClient* client = find_client(server, session_id);
        if(client)
        {
            /* 响应路径：允许少量重试 */
            if(!sse_send_event(server, client, "message", text, 10))
                remove_client(server, client);
        }
        LeaveCriticalSection(&server->clients_lock);
    }
    else
    {
        sse_broadcast(server, text, 10);
    }

    free(text);
    json_decref(resp);
}

static void jsonrpc_send_error(McpServer* server, const char* session_id,
                               const json_t* id, int code, const char* message)
{
    json_t* err = json_object();
    json_object_set_new(err, "code", json_integer(code));
    json_object_set_new(err, "message", json_string(message));
    jsonrpc_send_response(server, session_id, NULL, err, id);
    json_decref(err);
}

/* 构造 MCP notifications/message（调试事件通知） */
static void mcp_send_notification(McpServer* server, const char* event_json)
{
    json_t* notif = json_object();
    json_t* params = json_object();
    json_t* data = NULL;
    json_error_t err;

    data = json_loads(event_json, JSON_REJECT_DUPLICATES, &err);
    if(!data)
        data = json_object();

    json_object_set_new(notif, "jsonrpc", json_string("2.0"));
    json_object_set_new(notif, "method", json_string("notifications/message"));
    json_object_set_new(params, "level", json_string("info"));
    json_object_set_new(params, "data", data);
    json_object_set_new(notif, "params", params);

    char* text = json_dumps(notif, JSON_COMPACT | JSON_ENSURE_ASCII);
    if(text)
    {
        /* 通知路径：只尝试一次，避免阻塞 GUI 回调线程 */
        sse_broadcast(server, text, 0);
        free(text);
    }
    json_decref(notif);
}

/* 处理单个 JSON-RPC 请求（method 分发） */
static void handle_jsonrpc_request(McpServer* server, const char* session_id,
                                   const char* body)
{
    json_t* req = NULL;
    json_t* id = NULL;
    json_t* method = NULL;
    json_error_t err;

    req = json_loads(body, JSON_REJECT_DUPLICATES, &err);
    if(!req)
    {
        jsonrpc_send_error(server, session_id, NULL, -32700, "Parse error");
        return;
    }

    if(!json_is_object(req))
    {
        jsonrpc_send_error(server, session_id, NULL, -32600, "Invalid Request");
        json_decref(req);
        return;
    }

    id = json_object_get(req, "id");
    method = json_object_get(req, "method");

    if(!json_is_string(method))
    {
        jsonrpc_send_error(server, session_id, id, -32600, "Invalid Request: missing method");
        json_decref(req);
        return;
    }

    const char* m = json_string_value(method);

    if(strcmp(m, "initialize") == 0)
    {
        json_t* result = json_object();
        json_t* caps = json_object();
        json_t* tools_caps = json_object();
        json_t* info = json_object();

        json_object_set_new(result, "protocolVersion", json_string("2025-03-26"));
        json_object_set_new(tools_caps, "listChanged", json_false());
        json_object_set_new(caps, "tools", tools_caps);
        json_object_set_new(result, "capabilities", caps);
        json_object_set_new(info, "name", json_string("x64dbg-mcp"));
        json_object_set_new(info, "version", json_string("1.0.0"));
        json_object_set_new(result, "serverInfo", info);

        jsonrpc_send_response(server, session_id, result, NULL, id);
        json_decref(result);
    }
    else if(strcmp(m, "notifications/initialized") == 0 ||
            strcmp(m, "notifications/cancelled") == 0 ||
            strcmp(m, "notifications/roots/list_changed") == 0 ||
            strcmp(m, "notifications/tools/list_changed") == 0)
    {
        /* 通知：无响应 */
    }
    else if(strcmp(m, "ping") == 0)
    {
        json_t* result = json_object();
        jsonrpc_send_response(server, session_id, result, NULL, id);
        json_decref(result);
    }
    else if(strcmp(m, "tools/list") == 0)
    {
        json_t* result = json_object();
        json_t* arr = json_array();

        EnterCriticalSection(&server->tools_lock);
        for(int i = 0; i < server->tool_count; i++)
        {
            const McpTool* t = &server->tools[i].tool;
            json_t* item = json_object();
            json_t* schema = NULL;
            json_error_t schema_err;

            json_object_set_new(item, "name", json_string(t->name));
            json_object_set_new(item, "description", json_string(t->description));

            schema = json_loads(t->input_schema_json, JSON_REJECT_DUPLICATES, &schema_err);
            if(!schema)
                schema = json_object();
            json_object_set_new(item, "inputSchema", schema);

            json_array_append_new(arr, item);
        }
        LeaveCriticalSection(&server->tools_lock);

        json_object_set_new(result, "tools", arr);
        jsonrpc_send_response(server, session_id, result, NULL, id);
        json_decref(result);
    }
    else if(strcmp(m, "tools/call") == 0)
    {
        json_t* params = json_object_get(req, "params");
        json_t* name = params ? json_object_get(params, "name") : NULL;
        json_t* args = params ? json_object_get(params, "arguments") : NULL;
        const char* tool_name = json_is_string(name) ? json_string_value(name) : NULL;

        if(!tool_name)
        {
            jsonrpc_send_error(server, session_id, id, -32602, "Invalid params: missing tool name");
            json_decref(req);
            return;
        }

        McpToolHandler handler = NULL;
        EnterCriticalSection(&server->tools_lock);
        for(int i = 0; i < server->tool_count; i++)
        {
            if(strcmp(server->tools[i].tool.name, tool_name) == 0)
            {
                handler = server->tools[i].tool.handler;
                break;
            }
        }
        LeaveCriticalSection(&server->tools_lock);

        if(!handler)
        {
            char msg[256];
            _snprintf(msg, sizeof(msg), "Unknown tool: %s", tool_name);
            jsonrpc_send_error(server, session_id, id, -32602, msg);
            json_decref(req);
            return;
        }

        bool is_error = false;
        if(!json_is_object(args))
            args = NULL;
        char* text = handler(server, args, &is_error);

        json_t* result = json_object();
        json_t* content = json_array();
        json_t* item = json_object();
        json_object_set_new(item, "type", json_string("text"));
        json_object_set_new(item, "text", json_string(text ? text : ""));
        json_array_append_new(content, item);
        json_object_set_new(result, "content", content);
        json_object_set_new(result, "isError", json_boolean(is_error));

        jsonrpc_send_response(server, session_id, result, NULL, id);
        json_decref(result);
        free(text);
    }
    else
    {
        char msg[256];
        _snprintf(msg, sizeof(msg), "Method not found: %s", m);
        jsonrpc_send_error(server, session_id, id, -32601, msg);
    }

    json_decref(req);
}

/* ---------- SSE 连接处理 ---------- */

/* ---------- HTTP 连接处理（每连接一线程） ---------- */

typedef struct
{
    McpServer* server;
    SOCKET sock;
} ConnParam;

static DWORD WINAPI handle_connection(LPVOID param);

static void process_sse_connection(McpServer* server, SOCKET sock)
{
    mcp_dbg_log("[sse] enter sock=%p", (void*)sock);
    /* 注册 session */
    SseClient* client = (SseClient*)xmalloc(sizeof(SseClient));
    client->sock = sock;
    client->next = NULL;
    make_session_id(client->session_id, sizeof(client->session_id));
    mcp_dbg_log("[sse] session=%s", client->session_id);

    EnterCriticalSection(&server->clients_lock);
    client->next = server->clients;
    server->clients = client;
    LeaveCriticalSection(&server->clients_lock);

    /* 发送 endpoint 事件 */
    char endpoint[128];
    _snprintf(endpoint, sizeof(endpoint), "/messages?session_id=%s",
              client->session_id);
    bool ok = sse_send_event(server, client, "endpoint", endpoint, 10);
    mcp_dbg_log("[sse] endpoint sent ok=%d", ok ? 1 : 0);

    /* 保持连接，周期性发送 keep-alive 注释，检测断开 */
    int keepalive = 0;
    while(server->running)
    {
        Sleep(15000);
        keepalive++;
        if(keepalive % 2 == 0)
        {
            /* 发送 ": keepalive" 注释帧；失败视为连接断开 */
            const char* ka = ": keepalive\r\n\r\n";
            u_long nonblock = 1;
            ioctlsocket(sock, FIONBIO, &nonblock);
            int sent = send(sock, ka, (int)strlen(ka), 0);
            if(sent == SOCKET_ERROR)
            {
                int err = WSAGetLastError();
                if(err != WSAEWOULDBLOCK)
                    break;
            }
        }
    }

    EnterCriticalSection(&server->clients_lock);
    remove_client(server, client);
    LeaveCriticalSection(&server->clients_lock);
}

static void process_http_connection(McpServer* server, SOCKET sock)
{
    char buf[65536];
    int total = 0;

    while(server->running)
    {
        /* 接收请求（可能分片到达；SO_RCVTIMEO 控制空闲超时） */
        int n;
        bool got_header = false;
        while(total < (int)sizeof(buf) - 1)
        {
            n = recv(sock, buf + total, (int)sizeof(buf) - 1 - total, 0);
            if(n == 0)
                return; /* 对端关闭 */
            if(n == SOCKET_ERROR)
            {
                int err = WSAGetLastError();
                if(err == WSAETIMEDOUT)
                {
                    /* 空闲超时：无完整请求则关闭，有部分数据则继续等待 */
                    if(total == 0)
                        return;
                    continue;
                }
                return;
            }
            total += n;
            buf[total] = '\0';

            /* 检查头部是否完整 */
            if(!got_header)
            {
                const char* hdr_end = strstr(buf, "\r\n\r\n");
                if(hdr_end)
                {
                    got_header = true;
                    const char* cl = http_find_header(buf, hdr_end, "content-length");
                    if(cl)
                    {
                        size_t body_len = (size_t)strtoul(cl, NULL, 10);
                        /* 需要等待 body 完整 */
                        size_t have = (size_t)(total - (hdr_end + 4 - buf));
                        if(have < body_len)
                        {
                            if(total < (int)sizeof(buf) - 1)
                                continue;
                        }
                    }
                }
            }
            if(got_header)
                break;
        }
        if(!got_header)
            return;

        HttpRequest req;
        if(!http_parse_request(buf, (size_t)total, &req))
        {
            mcp_dbg_log("[http] parse FAILED sock=%p", (void*)sock);
            http_send_response(sock, 400, "Bad Request", "text/plain",
                               "bad request", 11);
            return;
        }
        mcp_dbg_log("[http] %s %s sock=%p", req.method, req.path, (void*)sock);

        /* CORS 预检 */
        if(strcmp(req.method, "OPTIONS") == 0)
        {
            http_send_response(sock, 204, "No Content", "text/plain", NULL, 0);
            return;
        }

        if(strcmp(req.method, "GET") == 0 && strcmp(req.path, "/sse") == 0)
        {
            /* 升级为 SSE 连接：立即发响应头，然后常驻 */
            const char* head =
                "HTTP/1.1 200 OK\r\n"
                "Content-Type: text/event-stream\r\n"
                "Cache-Control: no-cache\r\n"
                "Connection: keep-alive\r\n"
                "Access-Control-Allow-Origin: *\r\n"
                "\r\n";
            int hl = (int)strlen(head);
            int sent = 0;
            while(sent < hl)
            {
                int s = send(sock, head + sent, hl - sent, 0);
                if(s == SOCKET_ERROR)
                {
                    closesocket(sock);
                    return;
                }
                sent += s;
            }
            process_sse_connection(server, sock);
            return;
        }

        if(strcmp(req.method, "POST") == 0 && strcmp(req.path, "/messages") == 0)
        {
            mcp_dbg_log("[http] POST /messages sock=%p", (void*)sock);
            /* 解析 session_id */
            char session_id[MCP_SESSION_ID_LEN] = "";
            {
                const char* sid = strstr(req.query, "session_id=");
                if(sid)
                {
                    sid += strlen("session_id=");
                    const char* amp = strchr(sid, '&');
                    size_t len = amp ? (size_t)(amp - sid) : strlen(sid);
                    if(len >= sizeof(session_id))
                        len = sizeof(session_id) - 1;
                    memcpy(session_id, sid, len);
                    session_id[len] = '\0';
                }
            }

            /* 执行 JSON-RPC（结果经 SSE 回发） */
            handle_jsonrpc_request(server, session_id, req.body);

            /* 202 Accepted，无需 body */
            http_send_response(sock, 202, "Accepted", "text/plain", NULL, 0);
            return;
        }

        if(strcmp(req.method, "GET") == 0 && strcmp(req.path, "/") == 0)
        {
            char page[1024];
            int n = _snprintf(page, sizeof(page),
                "{\"server\":\"x64dbg-mcp\",\"port\":%d,\"clients\":%d,\"endpoint\":\"/sse\"}",
                server->port, mcp_server_get_client_count(server));
            http_send_response(sock, 200, "OK", "application/json", page, (size_t)n);
            return;
        }

        http_send_response(sock, 404, "Not Found", "text/plain", "not found", 9);
        return;
    }
}

static DWORD WINAPI handle_connection(LPVOID param)
{
    ConnParam* cp = (ConnParam*)param;

    /* 3 秒接收超时：空闲连接自动回收，避免线程泄漏 */
    int timeout = 3000;
    setsockopt(cp->sock, SOL_SOCKET, SO_RCVTIMEO, (const char*)&timeout,
               sizeof(timeout));

    mcp_dbg_log("[conn] enter sock=%p", (void*)cp->sock);
    conn_register(cp->server, cp->sock);
    process_http_connection(cp->server, cp->sock);
    conn_unregister(cp->server, cp->sock);
    mcp_dbg_log("[conn] exit sock=%p", (void*)cp->sock);

    shutdown(cp->sock, SD_BOTH);
    closesocket(cp->sock);
    free(cp);
    return 0;
}

/* ---------- 监听线程 ---------- */

static DWORD WINAPI listen_thread(LPVOID param)
{
    McpServer* server = (McpServer*)param;

    while(server->running)
    {
        SOCKET client = accept(server->listen_sock, NULL, NULL);
        if(client == INVALID_SOCKET)
        {
            int err = WSAGetLastError();
            if(!server->running || err == WSAEINTR || err == WSAENOTSOCK)
                break;
            Sleep(50);
            continue;
        }

        ConnParam* cp = (ConnParam*)xmalloc(sizeof(ConnParam));
        cp->server = server;
        cp->sock = client;
        HANDLE h = CreateThread(NULL, 0, handle_connection, cp, 0, NULL);
        if(h)
            CloseHandle(h);
        else
        {
            closesocket(client);
            free(cp);
        }
    }
    return 0;
}

/* ---------- 公共接口 ---------- */

McpServer* mcp_server_start(int port)
{
    WSADATA wsa;
    McpServer* server;
    SOCKET sock;
    struct sockaddr_in addr;

    if(WSAStartup(MAKEWORD(2, 2), &wsa) != 0)
        return NULL;

    server = (McpServer*)xmalloc(sizeof(McpServer));
    memset(server, 0, sizeof(*server));
    server->running = 1;
    server->port = port > 0 ? port : MCP_DEFAULT_PORT;
    server->listen_sock = INVALID_SOCKET;
    server->stop_event = CreateEventW(NULL, TRUE, FALSE, NULL);
    InitializeCriticalSection(&server->tools_lock);
    InitializeCriticalSection(&server->clients_lock);
    InitializeCriticalSection(&server->entries_lock);

    sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if(sock == INVALID_SOCKET)
        goto fail;

    /* SO_EXCLUSIVEADDRUSE：禁止其他进程（如 x32dbg 的插件）劫持同一端口 */
    BOOL excl = TRUE;
    setsockopt(sock, SOL_SOCKET, SO_EXCLUSIVEADDRUSE,
               (const char*)&excl, sizeof(excl));

    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = inet_addr("127.0.0.1"); /* 仅本机回环 */
    addr.sin_port = htons((u_short)server->port);

    if(bind(sock, (struct sockaddr*)&addr, sizeof(addr)) != 0)
    {
        /* 端口被占用时自动回退到相邻空闲端口（向上探测） */
        int tried = 0;
        while(tried < 20)
        {
            server->port++;
            addr.sin_port = htons((u_short)server->port);
            if(bind(sock, (struct sockaddr*)&addr, sizeof(addr)) == 0)
                break;
            tried++;
        }
        if(tried >= 20)
        {
            closesocket(sock);
            goto fail;
        }
    }

    if(listen(sock, 8) != 0)
    {
        closesocket(sock);
        goto fail;
    }

    server->listen_sock = sock;
    server->listen_thread = CreateThread(NULL, 0, listen_thread, server, 0, NULL);
    if(!server->listen_thread)
    {
        closesocket(sock);
        goto fail;
    }

    return server;

fail:
    DeleteCriticalSection(&server->tools_lock);
    DeleteCriticalSection(&server->clients_lock);
    CloseHandle(server->stop_event);
    free(server);
    WSACleanup();
    return NULL;
}

void mcp_server_stop(McpServer* server)
{
    if(!server)
        return;

    InterlockedExchange(&server->running, 0);

    /* 关闭监听 socket 以唤醒 accept */
    if(server->listen_sock != INVALID_SOCKET)
    {
        shutdown(server->listen_sock, SD_BOTH);
        closesocket(server->listen_sock);
        server->listen_sock = INVALID_SOCKET;
    }

    if(server->listen_thread)
    {
        WaitForSingleObject(server->listen_thread, 3000);
        CloseHandle(server->listen_thread);
        server->listen_thread = NULL;
    }

    /* 唤醒并等待全部连接线程退出（此时 server 内存仍有效） */
    conn_shutdown_all(server);

    /* 关闭所有 SSE 客户端（线程已退出，锁安全） */
    EnterCriticalSection(&server->clients_lock);
    while(server->clients)
    {
        SseClient* c = server->clients;
        server->clients = c->next;
        closesocket(c->sock);
        free(c);
    }
    LeaveCriticalSection(&server->clients_lock);

    DeleteCriticalSection(&server->tools_lock);
    DeleteCriticalSection(&server->clients_lock);
    DeleteCriticalSection(&server->entries_lock);
    CloseHandle(server->stop_event);
    free(server);
    WSACleanup();
}

void mcp_server_register_tool(McpServer* server, const McpTool* tool)
{
    if(!server || !tool || !tool->name || !tool->handler)
        return;

    EnterCriticalSection(&server->tools_lock);
    if(server->tool_count < MCP_MAX_TOOLS)
    {
        server->tools[server->tool_count].tool = *tool;
        server->tools[server->tool_count].registered = true;
        server->tool_count++;
    }
    LeaveCriticalSection(&server->tools_lock);
}

int mcp_server_get_port(McpServer* server)
{
    return server ? server->port : -1;
}

int mcp_server_get_client_count(McpServer* server)
{
    int count = 0;
    if(!server)
        return 0;
    EnterCriticalSection(&server->clients_lock);
    SseClient* c;
    for(c = server->clients; c; c = c->next)
        count++;
    LeaveCriticalSection(&server->clients_lock);
    return count;
}

bool mcp_server_is_running(McpServer* server)
{
    return server && server->running && server->listen_sock != INVALID_SOCKET;
}

void mcp_server_notify(McpServer* server, const char* event_json)
{
    if(!server || !server->running || !event_json)
        return;
    mcp_send_notification(server, event_json);
}

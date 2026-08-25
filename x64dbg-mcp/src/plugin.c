/*
 * x64dbg-mcp: Model Context Protocol 服务器插件
 *
 * plugin.c - x64dbg 插件入口
 *
 * 插件加载后在 x64dbg 内启动 MCP 服务器（127.0.0.1:<port>/sse），
 * 提供调试事件推送（暂停/运行/断点等）与插件菜单/命令。
 *
 * 端口通过 x64dbg 配置持久化（BridgeSetting，section [x64dbg-mcp]）。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mcp.h"
#include "_plugins.h"
#include "bridgemain.h"
#include "_dbgfunctions.h"

/* jansson 前向声明（mcp.h 已有），此处仅用于日志辅助 */
#include <jansson.h>

/* tools.c 提供的工具注册入口 */
void tools_register_all(McpServer* server);

/* x64dbg 插件导出宏 */
#define PLUG_EXPORT __declspec(dllexport)

/* ---------- 全局状态 ---------- */

static int g_plugin_handle = -1;
static int g_menu_root = -1;
static int g_menu_status = -1;
static int g_menu_restart = -1;
static McpServer* g_server = NULL;

#define SETTINGS_SECTION "x64dbg-mcp"
#define SETTINGS_PORT "Port"

/* ---------- 配置 ---------- */

static int load_port_from_settings(void)
{
    duint port = MCP_DEFAULT_PORT;
    BridgeSettingGetUint(SETTINGS_SECTION, SETTINGS_PORT, &port);
    if(port < 1024 || port > 65535)
        port = MCP_DEFAULT_PORT;
    return (int)port;
}

static void save_port_to_settings(int port)
{
    BridgeSettingSetUint(SETTINGS_SECTION, SETTINGS_PORT, (duint)port);
    BridgeSettingFlush();
}

/* ---------- 服务器生命周期 ---------- */

static void start_mcp_server(void)
{
    if(g_server)
        return;

    int port = load_port_from_settings();
    g_server = mcp_server_start(port);
    if(!g_server)
    {
        _plugin_logprintf("[x64dbg-mcp] Failed to start MCP server on port %d\n", port);
        return;
    }

    /* 若端口被占用自动回退，保存实际端口 */
    int actual = mcp_server_get_port(g_server);
    if(actual != port)
        save_port_to_settings(actual);

    /* 注册全部工具 */
    tools_register_all(g_server);

    _plugin_logprintf("[x64dbg-mcp] MCP server listening on http://127.0.0.1:%d/sse\n",
                      mcp_server_get_port(g_server));
    _plugin_logputs("[x64dbg-mcp] Connect your MCP client to this SSE endpoint.");
}

static void stop_mcp_server(void)
{
    if(!g_server)
        return;
    mcp_server_stop(g_server);
    g_server = NULL;
    _plugin_logputs("[x64dbg-mcp] MCP server stopped");
}

static void restart_mcp_server(void)
{
    stop_mcp_server();
    start_mcp_server();
}

/* ---------- 调试事件推送 ---------- */

static void notify_event(const char* event, const char* extra_key, duint extra_value)
{
    if(!g_server)
        return;
    json_t* obj = json_object();
    json_object_set_new(obj, "event", json_string(event));
    if(extra_key)
    {
        char buf[64];
        mcp_format_hex(buf, sizeof(buf), (uintptr_t)extra_value);
        json_object_set_new(obj, extra_key, json_string(buf));
    }

    char* text = json_dumps(obj, JSON_COMPACT | JSON_ENSURE_ASCII);
    if(text)
    {
        mcp_server_notify(g_server, text);
        free(text);
    }
    json_decref(obj);
}

static void notify_state(const char* state)
{
    if(!g_server)
        return;
    json_t* obj = json_object();
    json_object_set_new(obj, "event", json_string("debug_state"));
    json_object_set_new(obj, "state", json_string(state));

    /* 附带 CIP 等信息 */
    if(strcmp(state, "paused") == 0)
    {
        REGDUMP_AVX512 rd;
        memset(&rd, 0, sizeof(rd));
        if(DbgGetRegDumpEx(&rd, sizeof(rd)))
        {
            char buf[32];
            mcp_format_hex(buf, sizeof(buf), (uintptr_t)rd.regcontext.cip);
            json_object_set_new(obj, "cip", json_string(buf));
            char mod[MAX_MODULE_SIZE] = "";
            if(DbgGetModuleAt(rd.regcontext.cip, mod))
                json_object_set_new(obj, "module", json_string(mod));
        }
    }

    char* text = json_dumps(obj, JSON_COMPACT | JSON_ENSURE_ASCII);
    if(text)
    {
        mcp_server_notify(g_server, text);
        free(text);
    }
    json_decref(obj);
}

/* 插件回调：GUI 线程执行，只做轻量入队 */
static void cb_debug(CBTYPE cbType, void* callbackInfo)
{
    switch(cbType)
    {
    case CB_INITDEBUG:
        notify_state("debug_start");
        break;
    case CB_STOPDEBUG:
        notify_state("stopped");
        break;
    case CB_PAUSEDEBUG:
        notify_state("paused");
        break;
    case CB_RESUMEDEBUG:
        notify_state("running");
        break;
    case CB_SYSTEMBREAKPOINT:
        notify_state("system_breakpoint");
        break;
    case CB_BREAKPOINT:
    {
        PLUG_CB_BREAKPOINT* bp = (PLUG_CB_BREAKPOINT*)callbackInfo;
        if(bp && bp->breakpoint)
            notify_event("breakpoint", "address", bp->breakpoint->addr);
        break;
    }
    case CB_EXCEPTION:
    {
        PLUG_CB_EXCEPTION* ex = (PLUG_CB_EXCEPTION*)callbackInfo;
        if(ex && ex->Exception)
            notify_event("exception", "code", ex->Exception->ExceptionRecord.ExceptionCode);
        break;
    }
    case CB_EXITPROCESS:
        notify_state("process_exit");
        break;
    default:
        break;
    }
}

/* ---------- 菜单 ---------- */

static void print_status(void)
{
    if(!g_server)
    {
        _plugin_logputs("[x64dbg-mcp] MCP server is not running");
        return;
    }
    _plugin_logprintf("[x64dbg-mcp] Status: RUNNING | endpoint: http://127.0.0.1:%d/sse | clients: %d\n",
                      mcp_server_get_port(g_server),
                      mcp_server_get_client_count(g_server));
}

static void cb_menu(CBTYPE cbType, void* callbackInfo)
{
    (void)cbType;
    PLUG_CB_MENUENTRY* info = (PLUG_CB_MENUENTRY*)callbackInfo;
    if(!info)
        return;

    if(info->hEntry == g_menu_status)
        print_status();
    else if(info->hEntry == g_menu_restart)
        restart_mcp_server();
}

/* ---------- 插件命令 ---------- */

static bool cb_command_mcp(int argc, char** argv)
{
    (void)argc;
    (void)argv;
    print_status();
    return true;
}

/* ---------- 导出接口 ---------- */

PLUG_EXPORT bool pluginit(PLUG_INITSTRUCT* initStruct)
{
    g_plugin_handle = initStruct->pluginHandle;
    initStruct->sdkVersion = PLUG_SDKVERSION;
    initStruct->pluginVersion = 1;
    strncpy(initStruct->pluginName, "x64dbg-mcp", sizeof(initStruct->pluginName) - 1);

    _plugin_registercallback(g_plugin_handle, CB_INITDEBUG, cb_debug);
    _plugin_registercallback(g_plugin_handle, CB_STOPDEBUG, cb_debug);
    _plugin_registercallback(g_plugin_handle, CB_PAUSEDEBUG, cb_debug);
    _plugin_registercallback(g_plugin_handle, CB_RESUMEDEBUG, cb_debug);
    _plugin_registercallback(g_plugin_handle, CB_SYSTEMBREAKPOINT, cb_debug);
    _plugin_registercallback(g_plugin_handle, CB_BREAKPOINT, cb_debug);
    _plugin_registercallback(g_plugin_handle, CB_EXCEPTION, cb_debug);
    _plugin_registercallback(g_plugin_handle, CB_EXITPROCESS, cb_debug);

    _plugin_registercommand(g_plugin_handle, "mcp", cb_command_mcp, false);

    return true;
}

PLUG_EXPORT bool plugsetup(PLUG_SETUPSTRUCT* setupStruct)
{
    /* 插件子菜单：插件 → x64dbg MCP → 状态 / 重启服务器 */
    int hRoot = _plugin_menuadd(setupStruct->hMenu, "&x64dbg MCP");
    if(hRoot)
    {
        g_menu_root = hRoot;
        g_menu_status = _plugin_menuaddentry(hRoot, 1, "Status");
        g_menu_restart = _plugin_menuaddentry(hRoot, 2, "Restart MCP server");
        _plugin_registercallback(g_plugin_handle, CB_MENUENTRY, cb_menu);
    }

    /* 启动 MCP 服务器 */
    start_mcp_server();

    return true;
}

PLUG_EXPORT bool plugstop(void)
{
    if(g_plugin_handle != -1)
    {
        _plugin_unregistercommand(g_plugin_handle, "mcp");
        _plugin_unregistercallback(g_plugin_handle, CB_INITDEBUG);
        _plugin_unregistercallback(g_plugin_handle, CB_STOPDEBUG);
        _plugin_unregistercallback(g_plugin_handle, CB_PAUSEDEBUG);
        _plugin_unregistercallback(g_plugin_handle, CB_RESUMEDEBUG);
        _plugin_unregistercallback(g_plugin_handle, CB_SYSTEMBREAKPOINT);
        _plugin_unregistercallback(g_plugin_handle, CB_BREAKPOINT);
        _plugin_unregistercallback(g_plugin_handle, CB_EXCEPTION);
        _plugin_unregistercallback(g_plugin_handle, CB_EXITPROCESS);
        _plugin_unregistercallback(g_plugin_handle, CB_MENUENTRY);
    }

    stop_mcp_server();
    g_plugin_handle = -1;
    return true;
}

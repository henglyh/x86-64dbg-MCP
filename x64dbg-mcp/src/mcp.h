/*
 * x64dbg-mcp: Model Context Protocol 服务器插件
 *
 * mcp.h - 公共接口定义
 *
 * 该插件在 x64dbg 内启动一个基于 HTTP + SSE 的 MCP 服务器，
 * 将调试器能力（运行/暂停/断点/内存/寄存器/反汇编等）暴露为 MCP tools，
 * 供 Qoder / Claude Desktop 等 MCP 客户端接入。
 */
#ifndef X64DBG_MCP_H
#define X64DBG_MCP_H

#include <winsock2.h>
#include <windows.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

/*
 * 跨架构十六进制格式化。
 * 32 位下 msvcrt 不支持 %llX，此处按架构选用格式，避免栈错位/垃圾输出。
 */
static inline void mcp_format_hex(char* buf, size_t len, uintptr_t value)
{
#if defined(_WIN64)
    _snprintf(buf, len, "0x%llX", (unsigned long long)value);
#else
    _snprintf(buf, len, "0x%08X", (unsigned int)value);
#endif
}

static inline void mcp_format_hex_raw(char* buf, size_t len, uintptr_t value)
{
#if defined(_WIN64)
    _snprintf(buf, len, "%llX", (unsigned long long)value);
#else
    _snprintf(buf, len, "%08X", (unsigned int)value);
#endif
}

/* jansson 前向声明（完整定义见 pluginsdk/jansson/jansson.h） */
typedef struct json_t json_t;

/* 默认监听端口（仅绑定 127.0.0.1） */
#define MCP_DEFAULT_PORT 8765
#define MCP_MAX_TOOLS 32
#define MCP_SESSION_ID_LEN 48

typedef struct McpServer McpServer;

/*
 * MCP 工具处理函数。
 * args       : tools/call 的 arguments 参数（json_t*，工具不应持有）
 * is_error   : 输出参数，置 true 表示工具执行失败
 * 返回值     : malloc 分配的 UTF-8 文本（成功时为结果描述，失败时为错误信息），调用方 free
 */
typedef char* (*McpToolHandler)(McpServer* server, json_t* args, bool* is_error);

typedef struct
{
    const char* name;                  /* 工具名（MCP 规范：小写字母+数字+_-.） */
    const char* description;           /* 工具描述（供 AI 理解用途） */
    const char* input_schema_json;     /* JSON Schema (text)，描述参数 */
    McpToolHandler handler;            /* 实现 */
} McpTool;

/*
 * 启动 MCP 服务器（异步：内部创建监听线程）。
 * port 为 0 时使用默认端口。返回 NULL 表示失败。
 */
McpServer* mcp_server_start(int port);

/* 停止服务器并释放资源（会关闭所有 SSE 连接） */
void mcp_server_stop(McpServer* server);

/* 注册一个工具（须在服务器启动前或启动后均可调用，内部加锁） */
void mcp_server_register_tool(McpServer* server, const McpTool* tool);

/* 查询服务器信息（日志/命令用） */
int mcp_server_get_port(McpServer* server);
int mcp_server_get_client_count(McpServer* server);
bool mcp_server_is_running(McpServer* server);

/*
 * 向所有已连接的 MCP 客户端推送调试事件通知。
 * event_json 为 JSON 对象文本（如 {"event":"debug_state","state":"paused"}），
 * 将包装为 MCP notifications/message 通过 SSE 下发。
 * 可从任意线程调用（GUI 回调线程亦可），内部加锁，不会长时间阻塞。
 */
void mcp_server_notify(McpServer* server, const char* event_json);

#endif /* X64DBG_MCP_H */

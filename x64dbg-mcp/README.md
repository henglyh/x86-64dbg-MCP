# x64dbg-mcp

在 x64dbg / x32dbg 内运行的 **Model Context Protocol (MCP) 服务器插件**。
插件启动后在本机（127.0.0.1）开放一个 HTTP + SSE 端点，把调试器的能力
（运行/暂停/单步、断点、内存、寄存器、反汇编/汇编、表达式求值、模块/线程/调用栈、
字符串与字节模式搜索等）暴露为 MCP tools，供 **Qoder、Claude Desktop** 等
MCP 客户端接入，让 AI 直接驱动调试器。

- 传输：MCP "HTTP with SSE"（`GET /sse` + `POST /messages?session_id=...`）
- 依赖：仅 Win32 (Winsock) + jansson，插件自带线程，不阻塞 x64dbg GUI
- 双架构：同时构建 `x64dbg_mcp.dp64`（x64dbg）与 `x64dbg_mcp.dp32`（x32dbg）

## 目录结构

```
x64dbg-mcp/
├── build.bat          # 构建脚本（MinGW-w64，双架构）
├── install.bat        # 把构建产物安装到 release 的 plugins 目录
├── src/
│   ├── mcp.h          # 公共接口
│   ├── plugin.c       # x64dbg 插件入口（回调注册、菜单、命令、事件推送）
│   ├── mcp_server.c   # HTTP+SSE 传输层与 JSON-RPC 分发
│   └── tools.c        # 30 个 MCP 工具实现
├── test_mcp.py        # 端到端测试脚本（Python 3 标准库）
├── test_flow.py       # 完整调试流程测试（加载/反汇编/断点/运行等）
└── examples/          # MCP 客户端配置示例
```

## 构建

依赖 [WinLibs MinGW-w64](https://winlibs.com/)（须同时包含 x86_64 与 i686 两套工具链），
以及仓库根目录的 `pluginsdk/` 与 `release/`（含 x64/x32 的 DLL）。

```bat
cd x64dbg-mcp
build.bat
```

产物：

- `build/x64dbg_mcp.dp64` — x64dbg 插件
- `build/x64dbg_mcp.dp32` — x32dbg 插件

## 安装

把对应文件复制到 x64dbg 的插件目录后重启 x96dbg：

| 架构 | 复制到 |
| ---- | ------ |
| x64dbg | `release\x64\plugins\x64dbg_mcp.dp64` |
| x32dbg | `release\x32\plugins\x64dbg_mcp.dp32` |

或直接运行：

```bat
install.bat
```

## 使用

1. 启动 `x96dbg.exe`（或 x64dbg/x32dbg），插件自动加载并监听
   `http://127.0.0.1:8765/sse`。
   - 端口保存在 `x64dbg.ini` 的 `[x64dbg-mcp] Port` 中；
     若 8765 被占用会自动向上探测空闲端口并保存实际值。
   - 插件日志窗口会打印实际端点；也可在命令栏输入 `mcp` 查看状态。
2. 在 MCP 客户端中把 `http://127.0.0.1:<port>/sse` 配置为 SSE 服务器
   （见下方示例；若端口不是 8765，请以插件日志显示的为准）。
3. 连接后 `tools/list` 会返回 30 个 `x64dbg_*` 工具。

### 插件菜单与命令

- 菜单：`插件 → x64dbg MCP → Status / Restart MCP server`
- 命令：`mcp`（打印服务器状态）

### 调试事件推送

插件会把调试事件作为 `notifications/message` 通过 SSE 推送给所有已连接客户端：

- `debug_start` / `stopped` / `paused` / `running` / `system_breakpoint` / `process_exit`
- `breakpoint`（带 `address`）、`exception`（带 `code`）

### 调试日志（可选）

默认关闭。需要时设置环境变量后重启 x64dbg：

- `X64DBG_MCP_DEBUG=1` 开启日志，写到插件 DLL 所在目录的 `mcp_dbg.log`
- `X64DBG_MCP_LOG=C:\path\mcp.log` 覆盖日志路径（单文件超 1MB 自动轮转）

## MCP 客户端配置

### Qoder

在 Qoder 的 MCP 配置中添加：

```json
{
  "mcpServers": {
    "x64dbg": {
      "type": "sse",
      "url": "http://127.0.0.1:8765/sse"
    }
  }
}
```

### Claude Desktop

编辑 `claude_desktop_config.json`（新版 Claude Desktop 支持 SSE 传输）：

```json
{
  "mcpServers": {
    "x64dbg": {
      "type": "sse",
      "url": "http://127.0.0.1:8765/sse"
    }
  }
}
```

> 若客户端只支持 stdio，可用任意 MCP SSE 代理（如 `mcp-remote`）桥接：
> `mcp-remote http://127.0.0.1:8765/sse`

完整示例见 `examples/` 目录。

## 工具一览（30 个）

| 分类 | 工具 |
| ---- | ---- |
| 状态/控制 | `x64dbg_get_state` `x64dbg_run` `x64dbg_pause` `x64dbg_restart` `x64dbg_stop` `x64dbg_step_into` `x64dbg_step_over` `x64dbg_step_out` `x64dbg_open_file` `x64dbg_attach` |
| 断点 | `x64dbg_set_breakpoint` `x64dbg_delete_breakpoint` `x64dbg_list_breakpoints` |
| 内存 | `x64dbg_read_memory` `x64dbg_write_memory` `x64dbg_read_string` `x64dbg_find_strings` `x64dbg_find_pattern` `x64dbg_get_memory_map` |
| 寄存器 | `x64dbg_read_registers` `x64dbg_write_register` |
| 反汇编/汇编 | `x64dbg_disassemble` `x64dbg_assemble` |
| 表达式/命令 | `x64dbg_eval` `x64dbg_command` |
| 模块/线程/栈 | `x64dbg_get_modules` `x64dbg_get_threads` `x64dbg_get_callstack` |
| 标注 | `x64dbg_set_comment` `x64dbg_set_label` |

## 测试

先启动 x96dbg（插件自动监听），然后：

```bat
python test_mcp.py                       :: 连接 + 握手 + 工具列表 + get_state
python test_mcp.py --tool x64dbg_eval --args "{\"expression\":\"eip\"}"
python test_mcp.py --notify-wait 10      :: 额外等待 10s 接收调试事件推送
python test_flow.py                      :: 完整调试流程：加载程序->反汇编/内存/搜索
                                         :: ->断点->运行/暂停->寄存器->清理
```

> 若连接失败且报 `ConnectionResetError`：测试脚本已显式绕过系统代理直连
> 127.0.0.1；Qoder/Claude 等客户端自身的代理设置仍可能拦截本机端口，
> 请把 `127.0.0.1` 加入代理例外。

## 安全说明

- 服务器**仅绑定 127.0.0.1**，且**没有鉴权**——任何能访问该端口的本机进程
  都能完全控制调试器。
- 请勿通过端口转发/代理把该端口暴露到局域网或公网。
- 工具参数中的 `x64dbg_command` 可执行任意调试器命令，等同本机管理员调试权限。

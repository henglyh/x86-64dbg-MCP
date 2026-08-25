# 更新与修复记录 (UPDATES & FIXES)

记录 x86-64dbg-MCP 从开发至今的**新增功能与全部问题修复**。
每个问题均按「现象 → 根因 → 修复」说明，便于复现与回查。

---

## 版本历史

| 版本 | 说明 |
| ---- | ---- |
| v1.0 | 初始版本：MCP HTTP+SSE 服务器、22 个调试工具、插件入口、事件推送 |
| v1.1 | 功能增强：新增 8 个工具（共 30 个）、模块聚合修复、日志重构、构建链修复 |
| v1.2 | **x32dbg（x86）完整适配与稳定性修复**：修复 32 位崩溃与加载问题，双架构验证通过 |

---

## v1.2 — x32dbg 完整适配与稳定性修复（当前版本）

### 1. 32 位下插件崩溃：跨 CRT 堆损坏（0xC0000374）⚠️ 最重要

**现象**

- x32dbg 中插件可以加载，SSE 握手成功（`GET /sse` 正常返回 endpoint 事件），
  但一旦客户端发送 `POST /messages`（如 `initialize`），**整个 x32dbg 进程崩溃**，
  连接被重置（`ConnectionResetError: WinError 10054`）。
- Windows 事件日志（应用程序 → Application Error）记录异常码
  `0xC0000374`（`STATUS_HEAP_CORRUPTION`，堆损坏），崩溃模块 `ntdll.dll`。
- 64 位（x64dbg）下**无此问题**，因此只在 x86 上暴露。

**根因**

- 插件使用 `json_dumps()` 获取 JSON 文本后，用 MinGW CRT 的 `free()` 释放。
- 而 `json_dumps` 分配的内存来自 **jansson.dll**——该 DLL 由 MSVC 编译，
  使用 MSVC CRT（UCRT）的堆。
- **32 位**下 MinGW（MSYS2）默认链接 `msvcrt.dll`，与 MSVC CRT 是两个不同的
  堆：跨堆 free 触发堆损坏，随后任意路径崩溃。
  （64 位恰好 MinGW 与 jansson.dll 同为 UCRT，堆一致，问题被掩盖。）
- 已用独立测试程序实锤：`json_dumps` 成功返回 → 紧接着 `free()` 即
  `0xC0000005`（访问违例）。

**修复**

- **将 jansson 2.14 源码静态编译进插件**（`third_party/jansson-2.14`），
  插件不再加载 `jansson.dll`，分配与释放都在插件自身的 CRT 内完成。
- 构建后验证：`.dp32` / `.dp64` 的导入表中已**不存在 jansson.dll**。

### 2. 静态链接 jansson 后报 `undefined reference to __imp_json_*`

**现象**：改用源码静态编译后，链接阶段报大量
`undefined reference to \`__imp_json_string'` 等错误。

**根因**：x64dbg 的 `pluginsdk\jansson\jansson.h` 对所有函数声明了
**无条件的** `__declspec(dllimport)`，且 `_dbgfunctions.h` 通过相对路径
`jansson/jansson.h` 在头文件层面就引入了它，导致插件目标文件全部以
dllimport 形式引用符号。

**修复**：编译插件时增加参数 `-Ddllimport=`，使该声明退化为普通函数声明；
MinGW 的导入库同时提供 `__imp_*` 与普通符号，不影响系统 API 的链接。

### 3. x32dbg 插件加载失败：缺少 libgcc_s_dw2-1.dll（错误码 126）

**现象**：32 位插件（.dp32）拷入 `release\x32\plugins` 后，x32dbg 完全不加载；
用 `LoadLibrary` 测试返回 `ERROR_MOD_NOT_FOUND (126)`。

**根因**：MSYS2 的 32 位 gcc（i686）默认**动态链接 libgcc**，`.dp32` 的导入表
依赖 `libgcc_s_dw2-1.dll`，而该 DLL 既不在 x32dbg 目录，也不在系统 PATH。
（64 位 WinLibs 使用系统自带 UCRT，无此依赖，因此未暴露。）

**修复**：两项链接步骤均加 `-static-libgcc`，构建后验证 .dp32 导入表已无
`libgcc*` 与 `jansson.dll`，仅剩系统 DLL + `x32bridge.dll` + `x32dbg.dll`。

### 4. 32 位编译器静默失败：64/32 位同名 DLL 冲突（0xC000007B）

**现象**：`build.bat` 构建 x86 插件时，`cc1.exe`（32 位）被调用后**无任何
错误输出**却以 `-1073741701 (0xC000007B, STATUS_INVALID_IMAGE_FORMAT)` 退出，
构建失败且极其难以定位。

**根因**：mingw64 与 mingw32 的 `bin` 目录中存在**同名 DLL**
（`libiconv-2.dll`、`libintl-8.dll` 等）。PATH 中 mingw64（64 位）目录靠前时，
32 位 `cc1.exe` 加载了错误位数的同名 DLL → 映像格式错误，进程静默死亡。

**修复**：`build.bat` 在 x64 / x86 各阶段前，将**对应架构**的工具链
`bin` 目录前置到 `PATH`，保证每个进程加载到自己架构的 DLL。

### 5. gendef 不识别 `-q` 参数导致导入库生成失败

**现象**：`build.bat` 第 1 步报 `*** [-q] failed to open()`，
所有 `.def` / 导入库生成失败。

**根因**：所用 gendef 版本为 1.1，**不支持 `-q`（quiet）参数**，
将其误当作输入文件名。

**修复**：移除 `-q`，按 `gendef <dll>` 标准用法调用。

### 6. 安装失败被静默忽略（x64dbg 运行中 DLL 被锁定）

**现象**：`install.bat` 显示安装成功，但 `plugins` 目录中的插件仍是
**旧版本**——x64dbg 正在运行时 `copy` 因文件锁定失败，而错误被 `>nul` 吞掉。

**修复**：`install.bat` 中 `copy` 增加结果检查，失败时明确报错
（"is x64dbg running?"）并返回非零退出码。

### 7. 模块列表错误：PE 节页被当成独立模块（192 → 33）

**现象**：`x64dbg_get_modules` 返回大量"模块"（192 个），其中含 `".text"`、
`".rdata"`、`".data"` 等**节名**；`x64dbg_find_strings --module xxx` 只能
扫到模块头一页（0x1000 字节），搜不到节区里的字符串。

**根因**：`DbgMemMap` 返回的内存映射中，PE 节页的 `info` 字段是
**带前导空格的节名**（如 `" .text"`），与模块页的 `info`（模块名）不同，
聚合逻辑未把节页并入所属模块。

**修复**：聚合时把 `info` 以空格开头的 **节页并入其相邻模块** 的地址范围。
修复后：模块数 33（真实模块），`find_strings` 可扫描完整镜像
（例如 headless.exe 0x13000 字节全范围）。

### 8. 测试脚本被系统代理劫持（ConnectionResetError）

**现象**：`test_mcp.py` 连接 SSE 成功，但 `POST /messages` 恒报
`ConnectionResetError [WinError 10054]`（重置连接）；用 raw socket 直连
则一切正常。

**根因**：本机配置了**系统 HTTP 代理**（如 `http://127.0.0.1:12334`）。
Python `urllib` 会读取系统代理设置，把 `http://127.0.0.1:8765` 的请求也
经代理转发；该代理对回环直连返回 `502 Bad Gateway` 并重置连接。
（调试时通过 `http.client.HTTPConnection.debuglevel` 确认请求行被改写为
绝对 URI 形式 `POST http://127.0.0.1:8765/...`。）

**修复**：`test_mcp.py` / `debug_probe.py` 使用
`urllib.request.ProxyHandler({})` 显式禁用代理，直连本机回环。

### 9. 硬编码调试日志路径

**现象**：旧版把 `mcp_dbg.log` 写死在
`C:\Users\Administrator\Desktop\x\x64dbg-mcp\mcp_dbg.log`，换机器/换目录即失效，
且**默认一直写文件**。

**修复**：
- 默认**关闭**日志；`X64DBG_MCP_DEBUG=1` 环境变量开启；
- `X64DBG_MCP_LOG` 可覆盖路径；
- 默认写到**插件 DLL 所在目录**；
- 单文件超过 1MB 自动轮转。

---

## v1.1 — 功能增强

### 10. 新增 8 个 MCP 工具（22 → 30）

| 工具 | 说明 |
| ---- | ---- |
| `x64dbg_read_string` | 读取地址处的 NUL 结尾字符串（ascii / utf16） |
| `x64dbg_find_strings` | 在模块/地址范围搜索字符串（ASCII / UTF-16LE，跨页连续） |
| `x64dbg_find_pattern` | 字节模式搜索，支持 `?` 通配符（跨页匹配、不重复统计） |
| `x64dbg_get_memory_map` | 内存区域列表（基址/大小/状态/类型/保护属性） |
| `x64dbg_set_comment` | 设置/清除地址注释 |
| `x64dbg_set_label` | 设置/清除地址标签 |
| `x64dbg_open_file` | 在调试器中打开文件（不自动运行） |
| `x64dbg_attach` | 附加到指定 PID |

### 11. 其他

- `test_flow.py` 新增：完整调试流程自动化测试
  （加载 → 反汇编 → 读内存 → 搜索 → 断点 → 运行/暂停 → 寄存器 → 清理），
  支持 `--port` / `--target` 参数，x64 / x32 通用。
- 插件服务端 `/` 根路径返回 JSON 状态页（服务器名/端口/客户端数）。

---

## v1.0 — 初始版本

- MCP "HTTP with SSE" 传输层（`GET /sse` + `POST /messages?session_id=...`），
  纯 Win32 Winsock 实现，无第三方网络库。
- 22 个调试工具（状态/控制、断点、内存、寄存器、反汇编/汇编、表达式、
  命令、模块/线程/调用栈）。
- 调试事件推送（暂停/运行/断点/异常/启动/停止等 → `notifications/message`）。
- 端口持久化：端口保存到 x64dbg.ini `[x64dbg-mcp] Port`，被占用时自动
  向上探测空闲端口（SO_EXCLUSIVEADDRUSE 防止双实例劫持）。
- 仅绑定 `127.0.0.1`，不暴露到公网。

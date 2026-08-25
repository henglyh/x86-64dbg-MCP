#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
x64dbg-mcp 真实调试流程测试：
通过 MCP 工具加载 headless.exe -> 检查状态 -> 反汇编/读内存/模块/搜索 ->
设断点 -> 运行 -> 暂停 -> 读寄存器 -> 清理断点
"""
import argparse
import json
import sys
import urllib.parse
import urllib.request
import queue
import threading
import time

JSONRPC = "2.0"
BASE = "http://127.0.0.1:8765"

DEFAULT_TARGET_X64 = r"C:\Users\Administrator\Desktop\x\release\x64\headless.exe"
DEFAULT_TARGET_X32 = r"C:\Users\Administrator\Desktop\x\release\x32\headless.exe"


class SseClient:
    def __init__(self):
        self.session_id = None
        self.messages = queue.Queue()
        self._stop = threading.Event()
        self._opener = urllib.request.build_opener(urllib.request.ProxyHandler({}))

    def connect(self):
        req = urllib.request.Request(BASE + "/sse",
                                     headers={"Accept": "text/event-stream"})
        self._resp = self._opener.open(req, timeout=None)
        threading.Thread(target=self._read_loop, daemon=True).start()
        deadline = time.time() + 10
        while not self.session_id and time.time() < deadline:
            time.sleep(0.05)
        if not self.session_id:
            raise RuntimeError("no endpoint event")

    def _read_loop(self):
        event = None
        data_lines = []
        try:
            while not self._stop.is_set():
                line = self._resp.readline()
                if not line:
                    break
                line = line.decode("utf-8", "replace").rstrip("\r\n")
                if line == "":
                    if event is not None:
                        self._on_event(event, "\n".join(data_lines))
                    event = None
                    data_lines = []
                elif line.startswith("event:"):
                    event = line[6:].strip()
                elif line.startswith("data:"):
                    data_lines.append(line[5:].strip())
        except Exception:
            pass
        finally:
            self._stop.set()

    def _on_event(self, event, data):
        if event == "endpoint":
            self.session_id = urllib.parse.parse_qs(
                urllib.parse.urlparse(data).query)["session_id"][0]
        elif event == "message":
            try:
                self.messages.put(("message", json.loads(data)))
            except json.JSONDecodeError:
                pass

    def post(self, payload):
        url = f"{BASE}/messages?session_id={self.session_id}"
        req = urllib.request.Request(
            url, data=json.dumps(payload).encode(),
            headers={"Content-Type": "application/json"})
        with self._opener.open(req, timeout=10) as resp:
            resp.read()

    def rpc(self, method, params=None, _id=None):
        if _id is None:
            _id = int(time.time() * 1000) % 100000
        payload = {"jsonrpc": JSONRPC, "id": _id, "method": method}
        if params is not None:
            payload["params"] = params
        self.post(payload)
        deadline = time.time() + 15
        while time.time() < deadline:
            if self.messages.empty():
                time.sleep(0.1)
                continue
            kind, msg = self.messages.get_nowait()
            if kind != "message":
                continue
            if msg.get("id") == _id:
                return msg
        raise TimeoutError(method)


def call(client, name, args=None, expect_error=False):
    resp = client.rpc("tools/call", {"name": name, "arguments": args or {}})
    result = resp.get("result", resp)
    text = ""
    for c in result.get("content", []):
        text += c.get("text", "")
    err = result.get("isError", False)
    status = "OK " if not err else "ERR"
    if err and not expect_error:
        print(f"[{status}] {name}: {text[:200]}")
    elif err and expect_error:
        print(f"[{status}(expected)] {name}: {text[:120]}")
    else:
        print(f"[{status}] {name}: {text[:300]}")
    return text


def main():
    ap = argparse.ArgumentParser(description="x64dbg-mcp 完整调试流程测试")
    ap.add_argument("--port", type=int, default=8765)
    ap.add_argument("--target", default=None,
                    help="要加载的可执行文件路径（默认按架构选择 release 下的 headless.exe）")
    args = ap.parse_args()

    global BASE
    BASE = f"http://127.0.0.1:{args.port}"

    c = SseClient()
    c.connect()
    print(f"[+] connected session={c.session_id}")
    c.rpc("initialize", {"protocolVersion": "2025-03-26", "capabilities": {},
                         "clientInfo": {"name": "flow-test", "version": "1"}})
    c.post({"jsonrpc": JSONRPC, "method": "notifications/initialized"})

    if args.target:
        target = args.target
    else:
        import struct
        target = DEFAULT_TARGET_X64 if struct.calcsize("P") == 8 else DEFAULT_TARGET_X32
        # 客户端架构不能推断服务器架构，直接根据端口选择
        target = DEFAULT_TARGET_X32 if args.port != 8765 else DEFAULT_TARGET_X64

    call(c, "x64dbg_open_file", {"path": target})
    time.sleep(3)  # 等待加载并停在系统断点

    call(c, "x64dbg_get_state")
    call(c, "x64dbg_eval", {"expression": "headless.exe"})
    call(c, "x64dbg_disassemble", {"count": 6})
    call(c, "x64dbg_read_memory", {"address": "headless.exe", "size": 32})
    call(c, "x64dbg_get_modules")
    call(c, "x64dbg_find_strings",
         {"module": "headless.exe", "min_length": 4, "max_results": 5,
          "max_size": 0x40000})
    call(c, "x64dbg_set_breakpoint", {"address": "headless.exe+0x2000"})
    call(c, "x64dbg_list_breakpoints")
    call(c, "x64dbg_read_registers")
    call(c, "x64dbg_set_label", {"address": "headless.exe+0x2000", "text": "MCP_TEST_LABEL"})
    call(c, "x64dbg_set_comment", {"address": "headless.exe+0x2000", "text": "set by MCP"})
    call(c, "x64dbg_read_string", {"address": "headless.exe", "max_length": 128})

    call(c, "x64dbg_run")
    time.sleep(2)
    call(c, "x64dbg_get_state")
    call(c, "x64dbg_pause")
    time.sleep(1)
    call(c, "x64dbg_get_state")

    call(c, "x64dbg_delete_breakpoint", {"address": "headless.exe+0x2000"})
    call(c, "x64dbg_stop")
    time.sleep(2)
    call(c, "x64dbg_get_state")

    c._stop.set()
    print("[*] done")


if __name__ == "__main__":
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    main()

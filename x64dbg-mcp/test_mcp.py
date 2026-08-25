#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
x64dbg-mcp 插件测试脚本（Python 3 标准库，无第三方依赖）

用法:
  python test_mcp.py [--port 8765] [--tool x64dbg_get_state]

流程:
  1. 连接 SSE 端点，等待 endpoint 事件（拿到 session_id）
  2. 发送 initialize（握手）
  3. 发送 tools/list（列出全部工具）
  4. 调用指定的 tool（默认 x64dbg_get_state）
  5. 保持连接一段时间以接收调试事件通知
"""
import argparse
import json
import queue
import sys
import threading
import time
import urllib.parse
import urllib.request

JSONRPC = "2.0"
PROTOCOL_VERSION = "2025-03-26"


class SseClient:
    def __init__(self, base_url):
        self.base_url = base_url.rstrip("/")
        self.session_id = None
        self.messages = queue.Queue()
        self._stop = threading.Event()
        self._thread = None
        self.endpoint_received = threading.Event()
        # 禁用系统代理：本机回环服务不应走代理（否则 urllib 会经代理转发，
        # 代理对 127.0.0.1 的直连常返回 502/重置连接）
        self._opener = urllib.request.build_opener(urllib.request.ProxyHandler({}))

    def connect(self):
        url = self.base_url + "/sse"
        req = urllib.request.Request(url, headers={"Accept": "text/event-stream"})
        resp = self._opener.open(req, timeout=None)
        self._resp = resp
        self._thread = threading.Thread(target=self._read_loop, daemon=True)
        self._thread.start()
        if not self.endpoint_received.wait(timeout=10):
            raise RuntimeError("SSE endpoint event not received (timeout)")

    def _read_loop(self):
        event = None
        data_lines = []
        try:
            while not self._stop.is_set():
                line = self._resp.readline()
                if not line:
                    break
                line = line.decode("utf-8", errors="replace").rstrip("\n").rstrip("\r")
                if line == "":
                    if event is not None:
                        data = "\n".join(data_lines)
                        self._on_event(event, data)
                    event = None
                    data_lines = []
                    continue
                if line.startswith("event:"):
                    event = line[6:].strip()
                elif line.startswith("data:"):
                    data_lines.append(line[5:].strip())
                elif line.startswith(":"):
                    pass  # 注释帧（keepalive）
        except Exception as exc:
            if not self._stop.is_set():
                self.messages.put(("error", str(exc)))
        finally:
            self._stop.set()

    def _on_event(self, event, data):
        if event == "endpoint":
            parsed = urllib.parse.parse_qs(urllib.parse.urlparse(data).query)
            self.session_id = parsed.get("session_id", [None])[0]
            self.endpoint_received.set()
        elif event == "message":
            try:
                msg = json.loads(data)
            except json.JSONDecodeError:
                msg = {"raw": data}
            self.messages.put(("message", msg))

    def post(self, payload: dict):
        url = f"{self.base_url}/messages?session_id={self.session_id}"
        body = json.dumps(payload).encode("utf-8")
        req = urllib.request.Request(
            url, data=body,
            headers={"Content-Type": "application/json"})
        with self._opener.open(req, timeout=10) as resp:
            assert resp.status == 202, f"expected 202, got {resp.status}"
            resp.read()

    def rpc(self, method, params=None, _id=1):
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
        raise TimeoutError(f"no response for {method}")

    def wait_notification(self, timeout=20):
        """等待一条 notifications/message（调试事件）"""
        deadline = time.time() + timeout
        while time.time() < deadline:
            if self.messages.empty():
                time.sleep(0.2)
                continue
            kind, msg = self.messages.get_nowait()
            if kind == "message" and msg.get("method") == "notifications/message":
                return msg
            if kind == "error":
                raise RuntimeError(f"SSE error: {msg}")
        return None

    def close(self):
        self._stop.set()
        try:
            self._resp.close()
        except Exception:
            pass


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=8765)
    ap.add_argument("--tool", default="x64dbg_get_state",
                    help="tool 名称（默认 x64dbg_get_state）")
    ap.add_argument("--args", default="{}", help='tool 参数 JSON，如 {"address":"eip"}')
    ap.add_argument("--notify-wait", type=float, default=0,
                    help="测试完成后继续等待调试事件通知的秒数（0 不等待）")
    args = ap.parse_args()

    base = f"http://127.0.0.1:{args.port}"
    print(f"[*] connecting to {base}/sse ...")
    client = SseClient(base)
    client.connect()
    print(f"[+] SSE connected, session_id={client.session_id}")

    print("[*] initialize ...")
    resp = client.rpc("initialize", {
        "protocolVersion": PROTOCOL_VERSION,
        "capabilities": {},
        "clientInfo": {"name": "x64dbg-mcp-tester", "version": "1.0"},
    })
    print("[+] initialize:", json.dumps(resp.get("result", resp), ensure_ascii=False))

    print("[*] notifications/initialized ...")
    client.post({"jsonrpc": JSONRPC, "method": "notifications/initialized"})

    print("[*] tools/list ...")
    resp = client.rpc("tools/list")
    tools = resp.get("result", {}).get("tools", [])
    print(f"[+] {len(tools)} tools:")
    for t in tools:
        print(f"    - {t['name']}: {t.get('description', '')[:60]}")

    if args.tool:
        tool_args = json.loads(args.args)
        print(f"[*] tools/call {args.tool} {tool_args} ...")
        resp = client.rpc("tools/call", {
            "name": args.tool,
            "arguments": tool_args,
        }, _id=2)
        result = resp.get("result", resp)
        content = result.get("content", [])
        for c in content:
            print(f"[+] tool output: {c.get('text', '')}")
        if result.get("isError"):
            print("[!] tool reported error", file=sys.stderr)

    if args.notify_wait > 0:
        print(f"[*] waiting {args.notify_wait}s for debug notifications ...")
        deadline = time.time() + args.notify_wait
        while time.time() < deadline:
            n = client.wait_notification(timeout=1)
            if n:
                print("[+] notification:", json.dumps(n.get("params", {}), ensure_ascii=False))

    client.close()
    print("[*] done")


if __name__ == "__main__":
    main()

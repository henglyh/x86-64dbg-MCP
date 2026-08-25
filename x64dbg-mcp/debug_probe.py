#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""最小复现：raw socket vs urllib 对比 POST /messages"""
import json
import socket
import urllib.request

PORT = 8765
HOST = "127.0.0.1"


def raw_sse():
    s = socket.create_connection((HOST, PORT), timeout=10)
    s.sendall(b"GET /sse HTTP/1.1\r\nHost: 127.0.0.1\r\nAccept: text/event-stream\r\n\r\n")
    data = b""
    while b"session_id=" not in data:
        data += s.recv(4096)
    print("[raw] SSE head:", data[:200])
    return s, data


def raw_post(sid, body):
    s = socket.create_connection((HOST, PORT), timeout=10)
    req = (
        f"POST /messages?session_id={sid} HTTP/1.1\r\n"
        "Host: 127.0.0.1\r\n"
        "Content-Type: application/json\r\n"
        f"Content-Length: {len(body)}\r\n"
        "Connection: close\r\n\r\n"
    ).encode() + body.encode()
    s.sendall(req)
    resp = b""
    try:
        while True:
            chunk = s.recv(4096)
            if not chunk:
                break
            resp += chunk
    except socket.timeout:
        pass
    s.close()
    print(f"[raw] POST resp: {resp[:200]!r}")
    return resp


def urllib_post(sid, body):
    url = f"http://{HOST}:{PORT}/messages?session_id={sid}"
    req = urllib.request.Request(url, data=body.encode(),
                                 headers={"Content-Type": "application/json"})
    # 禁用系统代理（本机回环直连）
    opener = urllib.request.build_opener(urllib.request.ProxyHandler({}))
    try:
        with opener.open(req, timeout=10) as r:
            print(f"[url] POST status={r.status}")
            return r.status
    except Exception as e:
        print(f"[url] POST FAILED: {type(e).__name__}: {e}")
        return None


def main():
    # 1. raw socket SSE
    sse, data = raw_sse()
    print("[raw] SSE event:", data[:300])

    # 提取 session_id（endpoint 事件在 header 之后）
    txt = data.decode("utf-8", "replace")
    sid = None
    for part in txt.split("event: endpoint")[1:]:
        for ln in part.splitlines():
            if ln.startswith("data:"):
                sid = ln[5:].strip().split("session_id=")[-1]
    print("[raw] session_id =", sid)

    # 2. raw POST 通知
    print("\n--- raw socket POST notifications/initialized ---")
    raw_post(sid, '{"jsonrpc":"2.0","method":"notifications/initialized"}')

    # 3. urllib POST 通知（SSE 仍打开）
    print("\n--- urllib POST notifications/initialized (SSE open) ---")
    urllib_post(sid, '{"jsonrpc":"2.0","method":"notifications/initialized"}')

    # 4. urllib POST initialize（SSE 仍打开）
    print("\n--- urllib POST initialize (SSE open) ---")
    urllib_post(sid, json.dumps({
        "jsonrpc": "2.0", "id": 99, "method": "initialize",
        "params": {"protocolVersion": "2025-03-26", "capabilities": {},
                   "clientInfo": {"name": "probe", "version": "0"}}}))

    sse.close()
    print("\n[done]")


if __name__ == "__main__":
    main()

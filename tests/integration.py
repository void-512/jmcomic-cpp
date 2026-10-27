#!/usr/bin/env python3
"""Local protocol fixture, never contacts JMComic or needs user credentials."""
import base64
import hashlib
import http.server
import json
import os
import struct
import subprocess
import sys
import threading
import time
import urllib.parse
import zlib

lock = threading.Lock()
state = {"retry": 0, "images": 0, "active_regular": 0, "peak_regular": 0, "slow": 0, "corrupt": 0, "denied": 0, "errors": []}


def png():
    # Inverse of the reference strip transform: h=23, n=10, remainder=3.
    rows = [0] * 23
    for i in range(10):
        src = 23 - 2 * (i + 1) - 3
        dest = 2 * i + (3 if i else 0)
        for y in range(2 + (3 if i == 0 else 0)):
            rows[src + y] = dest + y
    data = b"".join(b"\0" + bytes(sum(([y, x, 91, 255] for x in range(3)), [])) for y in rows)
    def chunk(kind, data):
        return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data))
    return b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", 3, 23, 8, 6, 0, 0, 0)) + chunk(b"IDAT", zlib.compress(data)) + chunk(b"IEND", b"")


class Handler(http.server.BaseHTTPRequestHandler):
    def log_message(self, *args):
        pass

    def send(self, code, data, headers=None):
        self.send_response(code)
        self.send_header("Content-Length", str(len(data)))
        for k, v in (headers or {}).items():
            self.send_header(k, v)
        self.end_headers()
        try:
            self.wfile.write(data)
        except (BrokenPipeError, ConnectionResetError):
            pass

    def encrypted(self, data, headers=None):
        ts = self.headers["tokenparam"].split(",")[0]
        key = hashlib.md5((ts + "185Hcomic3PAPP7R").encode()).hexdigest().encode().hex()
        result = subprocess.run(["openssl", "enc", "-aes-256-ecb", "-K", key], input=json.dumps(data).encode(), capture_output=True, check=True)
        self.send(200, json.dumps({"code": 200, "data": base64.b64encode(result.stdout).decode()}).encode(), headers)

    def do_POST(self):
        self.do_GET()

    def do_GET(self):
        try:
            self.handle_request()
        except Exception as exc:
            with lock:
                state["errors"].append(str(exc))
            self.send(500, b"fixture assertion failed")

    def handle_request(self):
        url = urllib.parse.urlparse(self.path)
        path = url.path
        query = urllib.parse.parse_qs(url.query)
        if path.startswith("/media/photos/"):
            assert not self.headers.get("Cookie"), "Cookie leaked to image request"
            with lock:
                state["images"] += 1
                regular = "240002" not in path
                if regular:
                    state["active_regular"] += 1
                    state["peak_regular"] = max(state["peak_regular"], state["active_regular"])
                corrupt = "240001/001.webp" in path and state["corrupt"] == 0
                if corrupt:
                    state["corrupt"] += 1
                denied = "240003/001.webp" in path and state["denied"] == 0
                if denied:
                    state["denied"] += 1
                slow = "240002" in path and state["slow"] < 3
                if slow:
                    state["slow"] += 1
            try:
                time.sleep(2 if slow else 0.25)
                if denied:
                    self.send(403, b"access denied")
                else:
                    self.send(200, b"truncated image" if corrupt else png())
            finally:
                with lock:
                    if regular:
                        state["active_regular"] -= 1
            return
        ts, version = self.headers["tokenparam"].split(",")
        assert version == "2.0.13"
        secret = "18comicAPPContent" if path == "/chapter_view_template" else "18comicAPP"
        assert self.headers["token"] == hashlib.md5((ts + secret).encode()).hexdigest(), "Invalid request signature"
        assert abs(int(ts) - time.time()) < 60, "Stale timestamp"
        if path == "/search":
            q = query["search_query"][0]
            assert query["o"][0] in ["mr", "mv", "tf"]
            if q.startswith("keyword"):
                assert q == "keyword & 中文" and query["page"] == ["2"]
            if q == "retry":
                with lock:
                    state["retry"] += 1
                    first = state["retry"] == 1
                if first:
                    self.send(429, b"rate limited", {"Retry-After": "1"})
                    return
            if q == "redirect":
                self.encrypted({"redirect_aid": "240001"})
            else:
                self.encrypted({"total": "1", "content": [{"id": "240001", "name": "Fixture", "author": "Test"}]})
        elif path == "/album":
            self.encrypted({"id": 240001, "name": "Test:/Title", "author": ["Test"], "series": [{"id": "240001", "name": "Chapter", "sort": "1"}]})
        elif path == "/chapter":
            self.encrypted({"id": int(query["id"][0]), "name": "Chapter", "images": ["001.webp", "002.webp", "003.webp"]})
        elif path == "/chapter_view_template":
            self.send(200, b"<script>var scramble_id = 220980;</script>")
        elif path == "/login":
            form = urllib.parse.parse_qs(self.rfile.read(int(self.headers.get("Content-Length", "0"))).decode())
            if "username" in form:
                if form["username"] != ["test"] or form["password"] != ["secret"]:
                    self.send(200, b'{"code":401,"data":"invalid credentials"}')
                    return
                self.encrypted({"username": "test"}, {"Set-Cookie": "AVS=test-session; Path=/; HttpOnly"})
            else:
                assert "AVS=test-session" in self.headers.get("Cookie", ""), "Session missing"
                self.encrypted({"username": "test"})
        elif path == "/favorite":
            if query["folder_id"] == ["999"]:
                self.send(401, b"expired")
                return
            assert "AVS=test-session" in self.headers.get("Cookie", ""), "Session missing"
            assert query["page"] == ["2"] and query["folder_id"] == ["7"]
            self.encrypted({"total": "1", "list": [{"id": "240001", "name": "Fixture", "author": "Test"}], "folder_list": [{"FID": "7", "name": "Folder"}]})
        else:
            self.send(404, b"not found")


server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
thread = threading.Thread(target=server.serve_forever, daemon=True)
thread.start()
try:
    result = subprocess.run([sys.argv[1], "--integration", f"http://127.0.0.1:{server.server_port}"], check=False)
    if state["errors"]:
        raise AssertionError(state["errors"])
    assert state["retry"] == 2, state
    # Cancelled sockets can leave sleeping fixture handlers alive until their timer
    # expires; measure the regular transfer phase separately from that simulation.
    assert state["peak_regular"] <= 3, state
    # 3 original + zero for resume + one repaired + cancellation/retry.
    assert state["images"] <= 15, state
    assert state["corrupt"] == 1 and state["denied"] == 1, state
    print("Protocol fixture:", state)
    sys.exit(result.returncode)
finally:
    server.shutdown()
    server.server_close()

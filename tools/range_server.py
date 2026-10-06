#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 PokeMMO-Prospero contributors
"""Serves one file over plain HTTP with ETag and Range support, like PokeMMO's download server, for `make updater-test`.

  python3 tools/range_server.py PokeMMO-Client.zip [port] [--drop-every BYTES]

--drop-every closes connections after that many body bytes, to exercise the updater's resume.
"""
import http.server
import os
import sys

path = sys.argv[1]
port = int(sys.argv[2]) if len(sys.argv) > 2 and sys.argv[2].isdigit() else 8765
drop = int(sys.argv[sys.argv.index("--drop-every") + 1]) if "--drop-every" in sys.argv else 0
size = os.path.getsize(path)
etag = '"%x-%x"' % (int(os.path.getmtime(path)), size)


class Handler(http.server.BaseHTTPRequestHandler):
    def headers_for(self, start, end, partial):
        self.send_response(206 if partial else 200)
        self.send_header("ETag", etag)
        self.send_header("Accept-Ranges", "bytes")
        self.send_header("Content-Length", str(end - start + 1))
        if partial:
            self.send_header("Content-Range", f"bytes {start}-{end}/{size}")
        self.end_headers()

    def do_HEAD(self):
        self.headers_for(0, size - 1, False)

    def do_GET(self):
        start, end, partial = 0, size - 1, False
        spec = self.headers.get("Range", "")
        if spec.startswith("bytes="):
            a, b = spec[6:].split("-")
            start, end, partial = int(a), min(int(b), size - 1), True
        self.headers_for(start, end, partial)
        with open(path, "rb") as f:
            f.seek(start)
            left, sent = end - start + 1, 0
            while left:
                block = f.read(min(left, 1 << 16))
                if drop and sent + len(block) > drop:
                    self.wfile.write(block[: drop - sent])
                    return  # connection closed early
                self.wfile.write(block)
                sent += len(block)
                left -= len(block)

    def log_message(self, *args):
        pass


http.server.ThreadingHTTPServer(("127.0.0.1", port), Handler).serve_forever()

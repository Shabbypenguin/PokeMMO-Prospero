#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 PokeMMO-Prospero contributors
"""A stand-in for Google's OAuth device flow and the parts of Drive v3 the loader uses (loader/src/cloud.c), for tests on a PC.

python3 tools/cloud_server.py [--port 8765]; the loader reaches it with PROSPERO_CLOUD_BASE=http://127.0.0.1:8765.
Files live in memory. Every access token expires after --token-uses requests, to exercise the renewal."""
import argparse
import json
import re
import threading
import uuid
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import parse_qs, urlparse

FOLDER = "application/vnd.google-apps.folder"
lock = threading.Lock()
files = {}      # id -> {name, parents, mimeType, data}
sessions = {}   # upload session -> {meta, total, data}
tokens = {}     # access token -> uses left
polls = {}      # device code -> polls so far
args = None


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.0"

    def log_message(self, fmt, *a):
        print("cloud-server:", self.command, self.path[:120], fmt % a if "%" in fmt else "")

    def body(self):
        return self.rfile.read(int(self.headers.get("Content-Length", "0") or 0))

    def send(self, code, payload=b"", headers=None, content_type="application/json"):
        if isinstance(payload, (dict, list)):
            payload = json.dumps(payload).encode()
        self.send_response(code)
        for k, v in (headers or {}).items():
            self.send_header(k, v)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(payload)))
        self.end_headers()
        self.wfile.write(payload)

    def authorized(self):
        token = self.headers.get("Authorization", "").removeprefix("Bearer ")
        with lock:
            if tokens.get(token, 0) <= 0:
                self.send(401, {"error": {"code": 401, "message": "Invalid Credentials"}})
                return False
            tokens[token] -= 1
        return True

    def new_token(self):
        token = "ya29." + uuid.uuid4().hex
        tokens[token] = args.token_uses
        return token

    # ---- OAuth
    def oauth(self, path, form):
        if form.get("client_id", [""])[0] == "":
            return self.send(401, {"error": "invalid_client"})
        if path == "/device/code":
            code = uuid.uuid4().hex
            polls[code] = 0
            return self.send(200, {"device_code": code, "user_code": "PKMN-PS5X", "verification_url": "https://www.google.com/device",
                                   "expires_in": 600, "interval": 2})
        grant = form.get("grant_type", [""])[0]
        if grant.endswith("device_code"):
            code = form.get("device_code", [""])[0]
            if code not in polls:
                return self.send(400, {"error": "invalid_grant"})
            polls[code] += 1
            if polls[code] < 2:
                return self.send(428, {"error": "authorization_pending"})
            return self.send(200, {"access_token": self.new_token(), "expires_in": 3599, "refresh_token": "1//refresh-" + code,
                                   "token_type": "Bearer", "scope": "https://www.googleapis.com/auth/drive.file"})
        if grant == "refresh_token":
            if not form.get("refresh_token", [""])[0].startswith("1//refresh-"):
                return self.send(400, {"error": "invalid_grant"})
            return self.send(200, {"access_token": self.new_token(), "expires_in": 3599, "token_type": "Bearer"})
        return self.send(400, {"error": "unsupported_grant_type"})

    # ---- Drive
    def listing(self, query):
        q = query.get("q", [""])[0]
        parent = re.search(r"'([^']+)' in parents", q).group(1)
        name = re.search(r"name='((?:[^'\\]|\\.)*)'", q)
        name = name.group(1).replace("\\'", "'").replace("\\\\", "\\") if name else None
        with lock:
            found = [{"id": i, "name": f["name"], "mimeType": f["mimeType"], **({"size": str(len(f["data"]))} if f["mimeType"] != FOLDER else {})}
                     for i, f in files.items() if parent in f["parents"] and (name is None or f["name"] == name)]
        self.send(200, {"files": found})

    def create(self, meta, data, mime=None):
        file_id = uuid.uuid4().hex[:20]
        with lock:
            files[file_id] = {"name": meta["name"], "parents": meta.get("parents", ["root"]), "mimeType": meta.get("mimeType", mime or "application/octet-stream"),
                              "data": data}
        return file_id

    def do_POST(self):
        url = urlparse(self.path)
        query = parse_qs(url.query)
        raw = self.body()
        if url.path in ("/device/code", "/token"):
            return self.oauth(url.path, parse_qs(raw.decode()))
        if not self.authorized():
            return
        if url.path == "/drive/v3/files":
            return self.send(200, {"id": self.create(json.loads(raw), b"")})
        if url.path == "/upload/drive/v3/files" and query.get("uploadType") == ["multipart"]:
            boundary = re.search(r"boundary=(\S+)", self.headers["Content-Type"]).group(1).encode()
            parts = raw.split(b"--" + boundary)
            meta = json.loads(parts[1].split(b"\r\n\r\n", 1)[1].rstrip(b"\r\n"))
            data = parts[2].split(b"\r\n\r\n", 1)[1][:-2]
            return self.send(200, {"id": self.create(meta, data)})
        if url.path == "/upload/drive/v3/files" and query.get("uploadType") == ["resumable"]:
            session = uuid.uuid4().hex
            sessions[session] = {"meta": json.loads(raw), "total": int(self.headers["X-Upload-Content-Length"]), "data": b""}
            host = self.headers.get("Host", "127.0.0.1:%d" % args.port)
            return self.send(200, headers={"Location": "http://%s/upload/session/%s" % (host, session)})
        self.send(404, {"error": "unknown"})

    def do_PUT(self):
        url = urlparse(self.path)
        raw = self.body()
        if not url.path.startswith("/upload/session/") or not self.authorized():
            return self.send(404, {"error": "unknown"}) if not url.path.startswith("/upload/session/") else None
        session = sessions.get(url.path.rsplit("/", 1)[1])
        start, end, total = map(int, re.match(r"bytes (\d+)-(\d+)/(\d+)", self.headers["Content-Range"]).groups())
        if session is None or start != len(session["data"]) or end - start + 1 != len(raw):
            return self.send(400, {"error": "bad range"})
        session["data"] += raw
        if len(session["data"]) < total:
            return self.send(308, headers={"Range": "bytes=0-%d" % (len(session["data"]) - 1)})
        return self.send(200, {"id": self.create(session["meta"], session["data"])})

    def do_GET(self):
        url = urlparse(self.path)
        if not self.authorized():
            return
        if url.path == "/drive/v3/files":
            return self.listing(parse_qs(url.query))
        match = re.match(r"/drive/v3/files/([^/]+)$", url.path)
        if match and parse_qs(url.query).get("alt") == ["media"] and match.group(1) in files:
            return self.send(200, files[match.group(1)]["data"], content_type="application/octet-stream")
        self.send(404, {"error": "not found"})

    def do_DELETE(self):
        url = urlparse(self.path)
        if not self.authorized():
            return
        match = re.match(r"/drive/v3/files/([^/]+)$", url.path)
        with lock:
            if match and files.pop(match.group(1), None) is not None:
                return self.send(204)
        self.send(404, {"error": "not found"})


def main():
    global args
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", type=int, default=8765)
    parser.add_argument("--token-uses", type=int, default=7)
    args = parser.parse_args()
    ThreadingHTTPServer(("127.0.0.1", args.port), Handler).serve_forever()


if __name__ == "__main__":
    main()

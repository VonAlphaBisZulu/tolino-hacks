#!/bin/sh
# Tiny localhost HTTP bridge so the EPDC test app (HTML/JS in
# QWebEngineView) can issue MXCFB_SEND_UPDATE ioctls via the epdctest
# binary. Listens on 127.0.0.1:8765. Used only for the in-house
# diagnostic app; no security implications because nothing on the
# network can reach 127.0.0.1.
exec python3 - <<'PY'
import http.server, socketserver, subprocess, urllib.parse, sys
TOOL = "/mnt/onboard/.adds/epdctest"
class H(http.server.BaseHTTPRequestHandler):
    def do_GET(self):
        u = urllib.parse.urlparse(self.path)
        q = urllib.parse.parse_qs(u.query)
        if u.path == "/wf":
            wf   = q.get("wf",   ["2"])[0]
            mode = q.get("mode", ["1"])[0]
            r = subprocess.run([TOOL, wf, mode], capture_output=True, text=True, timeout=5)
            body = (r.stdout + r.stderr).strip() + "\n"
        else:
            body = "ok\n"
        self.send_response(200)
        self.send_header("Access-Control-Allow-Origin", "*")
        self.send_header("Cache-Control", "no-store")
        self.send_header("Content-Type", "text/plain")
        self.end_headers()
        self.wfile.write(body.encode())
    def log_message(self, *a, **k): pass

print("epdc-server: listening on 127.0.0.1:8765", file=sys.stderr)
with socketserver.TCPServer(("127.0.0.1", 8765), H) as s:
    s.allow_reuse_address = True
    s.serve_forever()
PY

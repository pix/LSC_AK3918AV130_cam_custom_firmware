"""Logs HTTP POSTs (webhook receiver for tests). Usage: hookrx.py port seconds"""
import http.server, sys, threading, time
class H(http.server.BaseHTTPRequestHandler):
    def do_POST(self):
        n = int(self.headers.get("Content-Length", 0)); body = self.rfile.read(n).decode(errors="replace")
        print("WEBHOOK", self.path, body, flush=True)
        self.send_response(204); self.end_headers()
    def log_message(self, *a): pass
s = http.server.HTTPServer(("0.0.0.0", int(sys.argv[1])), H)
threading.Thread(target=s.serve_forever, daemon=True).start()
time.sleep(float(sys.argv[2])); print("done", flush=True)

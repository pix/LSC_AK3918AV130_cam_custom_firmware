"""Stand-in for the push services the camera talks to (tests/notify):
ntfy, Telegram Bot API, Pushover, Discord (webhook + bot) and Gotify, on one
port (HTTP, or HTTPS with --tls). Checks the request format the way the real
services would (authentication, multipart fields, attachments), saves every
attachment and appends one JSON line per request to <out>/requests.jsonl.

    mock_push.py --port 8443 --out DIR [--tls CERT KEY] [--delay S]
                 [--fail500 N] [--rate429 N] [--max-attach BYTES] [--chunked]

Accepted credentials (anything else is refused like the real service):
  ntfy      PUT/POST /<topic>, Authorization "Bearer tk_goodtoken" or Basic
            user:pass "cam:secret"; topic "open" needs none
  Telegram  /bot123456:ABC-good_token/<method>, chat_id 4242
  Pushover  token azGoodAppToken, user uGoodUserKey
  Discord   webhook /api/webhooks/1234/goodhook, bot "Bot goodbot" on
            /api/v10/channels/987654321/messages
  Gotify    X-Gotify-Key (or ?token=) AgoodGotify
Only for tests: never point a camera at it outside the sandbox."""
import argparse, base64, email.header, json, os, ssl, sys, threading, time, urllib.parse
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

ap = argparse.ArgumentParser()
ap.add_argument("--port", type=int, required=True)
ap.add_argument("--bind", default="127.0.0.1")
ap.add_argument("--out", required=True)
ap.add_argument("--tls", nargs=2, metavar=("CERT", "KEY"))
ap.add_argument("--delay", type=float, default=0, help="seconds before answering")
ap.add_argument("--fail500", type=int, default=0, help="answer the first N requests with 500")
ap.add_argument("--rate429", type=int, default=0, help="answer the first N requests with 429 (retry_after 0.8 s)")
ap.add_argument("--max-attach", type=int, default=0, help="413 for attachments larger than this")
ap.add_argument("--chunked", action="store_true", help="chunked responses")
A = ap.parse_args()
os.makedirs(A.out, exist_ok=True)
LOCK = threading.Lock()
COUNT = [0]

TG_TOKEN, TG_CHAT = "123456:ABC-good_token", "4242"
PO_TOKEN, PO_USER = "azGoodAppToken", "uGoodUserKey"
DC_HOOK, DC_BOT, DC_CHAN = "/api/webhooks/1234/goodhook", "goodbot", "987654321"
GO_KEY = "AgoodGotify"


def multipart(ctype, body):
    """-> (fields {name: str}, files [{name, filename, ctype, data}]) or raises ValueError"""
    if not ctype.startswith("multipart/form-data"):
        raise ValueError("not multipart/form-data: " + ctype)
    bnd = None
    for p in ctype.split(";")[1:]:
        k, _, v = p.strip().partition("=")
        if k == "boundary":
            bnd = v.strip('"')
    if not bnd:
        raise ValueError("no boundary")
    b = ("--" + bnd).encode()
    end = b"\r\n" + b + b"--\r\n"
    if not body.startswith(b) or not body.endswith(end):
        raise ValueError("body does not start/end with the boundary (length wrong?)")
    fields, files = {}, []
    for part in body[len(b):-len(end)].split(b"\r\n" + b):
        if not part.startswith(b"\r\n"):
            raise ValueError("part without CRLF after the boundary")
        head, sep, data = part[2:].partition(b"\r\n\r\n")
        if not sep:
            raise ValueError("part without header end")
        hd = {}
        for line in head.decode().split("\r\n"):
            k, _, v = line.partition(":")
            hd[k.strip().lower()] = v.strip()
        cd = hd.get("content-disposition", "")
        if not cd.startswith("form-data"):
            raise ValueError("bad Content-Disposition " + cd)
        params = {}
        for p in cd.split(";")[1:]:
            k, _, v = p.strip().partition("=")
            params[k] = v.strip('"')
        if "filename" in params:
            files.append({"name": params.get("name"), "filename": params["filename"], "ctype": hd.get("content-type", ""), "data": data})
        else:
            fields[params.get("name")] = data.decode()
    return fields, files


def hdr(v):
    """RFC 2047 decoded header value (as ntfy does)"""
    if v is None:
        return None
    return "".join(t.decode(c or "ascii") if isinstance(t, bytes) else t for t, c in email.header.decode_header(v))


class H(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, *a):
        pass

    def answer(self, status, obj, extra=None):
        body = json.dumps(obj).encode() if obj is not None else b""
        self.rec["status"] = status
        self.rec["answer"] = obj
        with LOCK:                                  # recorded before answering (a slow answer may hit a closed socket)
            with open(os.path.join(A.out, "requests.jsonl"), "a") as f:
                f.write(json.dumps(self.rec) + "\n")
        if A.delay:
            time.sleep(A.delay)
        try:
            self.send_response(status)
            for k, v in (extra or {}).items():
                self.send_header(k, v)
            if body:
                self.send_header("Content-Type", "application/json")
            if A.chunked and body:
                self.send_header("Transfer-Encoding", "chunked")
            else:
                self.send_header("Content-Length", str(len(body)))
            self.send_header("Connection", "close")
            self.end_headers()
            if A.chunked and body:
                for i in range(0, len(body), 7):
                    c = body[i:i + 7]
                    self.wfile.write(b"%x\r\n%s\r\n" % (len(c), c))
                self.wfile.write(b"0\r\n\r\n")
            else:
                self.wfile.write(body)
        except OSError:
            pass

    def save(self, name, data):
        p = os.path.join(A.out, "%03d_%s_%s" % (self.rec["n"], self.rec["service"], os.path.basename(name or "file")))
        with open(p, "wb") as f:
            f.write(data)
        return p

    def handle_any(self):
        with LOCK:
            COUNT[0] += 1
            n = COUNT[0]
        n_len = int(self.headers.get("Content-Length", "0"))
        body = self.rfile.read(n_len) if n_len else b""
        u = urllib.parse.urlsplit(self.path)
        q = urllib.parse.parse_qs(u.query)
        self.rec = {"n": n, "t": time.time(), "method": self.command, "path": u.path, "query": u.query, "len": len(body),
                    "headers": {k: self.headers[k] for k in ("Authorization", "Title", "Tags", "Filename", "Message", "Priority",
                                                            "Content-Type", "X-Gotify-Key", "User-Agent", "Host") if self.headers.get(k)},
                    "service": "?", "fields": {}, "files": []}
        if "Title" in self.headers:
            self.rec["title"] = hdr(self.headers["Title"])
        if "Message" in self.headers:
            self.rec["message"] = hdr(self.headers["Message"])
        if n <= A.fail500:
            return self.answer(500, {"error": "internal error (mock)"})
        if n <= A.rate429:
            return self.answer(429, {"message": "You are being rate limited.", "retry_after": 0.8, "global": False, "error": "too many requests",
                                     "ok": False, "description": "Too Many Requests: retry after 1"}, {"Retry-After": "1"})
        p, ct = u.path, self.headers.get("Content-Type", "")
        try:
            if p.startswith("/bot"):
                return self.telegram(p, ct, body)
            if p == "/1/messages.json":
                return self.pushover(ct, body)
            if p.startswith("/api/webhooks/") or p.startswith("/api/v10/channels/"):
                return self.discord(p, ct, body)
            if p == "/message":
                return self.gotify(q, body)
            return self.ntfy(p, body)
        except ValueError as e:
            self.rec["error"] = str(e)
            return self.answer(400, {"error": "mock: " + str(e), "ok": False, "description": str(e), "message": str(e), "errors": [str(e)], "status": 0})

    do_POST = do_PUT = handle_any

    def too_big(self, n):
        return A.max_attach and n > A.max_attach

    def ntfy(self, p, body):
        self.rec["service"] = "ntfy"
        topic = p.strip("/")
        if not topic or "/" in topic:
            raise ValueError("bad topic " + p)
        auth = self.headers.get("Authorization", "")
        ok = auth == "Bearer tk_goodtoken" or auth == "Basic " + base64.b64encode(b"cam:secret").decode()
        if topic != "open" and not ok:
            return self.answer(401, {"code": 40101, "http": 401, "error": "unauthorized"})
        fn = self.headers.get("Filename")
        if fn:
            if self.command != "PUT":
                raise ValueError("attachments are sent with PUT")
            if self.too_big(len(body)):
                return self.answer(413, {"code": 41301, "http": 413, "error": "attachment too large, or bandwidth limit reached"})
            self.rec["files"].append({"name": "body", "filename": fn, "ctype": self.headers.get("Content-Type", ""), "size": len(body), "saved": self.save(fn, body)})
        else:
            self.rec["message"] = body.decode()
        return self.answer(200, {"id": "m%d" % self.rec["n"], "time": int(time.time()), "event": "message", "topic": topic})

    def files(self, files):
        for f in files:
            self.rec["files"].append({"name": f["name"], "filename": f["filename"], "ctype": f["ctype"], "size": len(f["data"]), "saved": self.save(f["filename"], f["data"])})

    def telegram(self, p, ct, body):
        self.rec["service"] = "telegram"
        parts = p.split("/")
        if len(parts) != 3 or parts[1][3:] != TG_TOKEN:
            return self.answer(401, {"ok": False, "error_code": 401, "description": "Unauthorized"})
        method = parts[2]
        fields, files = multipart(ct, body)
        self.rec["fields"] = fields
        self.files(files)
        if fields.get("chat_id") != TG_CHAT:
            return self.answer(400, {"ok": False, "error_code": 400, "description": "Bad Request: chat not found"})
        want = {"sendPhoto": "photo", "sendVideo": "video", "sendMessage": None}
        if method not in want:
            return self.answer(404, {"ok": False, "error_code": 404, "description": "Not Found"})
        if want[method] and [f["name"] for f in files] != [want[method]]:
            raise ValueError("%s needs exactly one file field %s" % (method, want[method]))
        if self.too_big(len(body)):
            return self.answer(413, {"ok": False, "error_code": 413, "description": "Request Entity Too Large"})
        return self.answer(200, {"ok": True, "result": {"message_id": self.rec["n"], "chat": {"id": 4242}, "caption": fields.get("caption", "")}})

    def pushover(self, ct, body):
        self.rec["service"] = "pushover"
        fields, files = multipart(ct, body)
        self.rec["fields"] = fields
        self.files(files)
        if fields.get("token") != PO_TOKEN:
            return self.answer(400, {"token": "invalid", "errors": ["application token is invalid"], "status": 0, "request": "r1"})
        if fields.get("user") != PO_USER:
            return self.answer(400, {"user": "invalid", "errors": ["user identifier is not a valid user, group, or subscribed user key"], "status": 0, "request": "r2"})
        if not fields.get("message"):
            return self.answer(400, {"message": "cannot be blank", "errors": ["message cannot be blank"], "status": 0, "request": "r3"})
        return self.answer(200, {"status": 1, "request": "r%d" % self.rec["n"]})

    def discord(self, p, ct, body):
        self.rec["service"] = "discord"
        if p.startswith("/api/webhooks/"):
            if p != DC_HOOK:
                return self.answer(401, {"message": "Invalid Webhook Token", "code": 50027})
        elif p != "/api/v10/channels/%s/messages" % DC_CHAN or self.headers.get("Authorization") != "Bot " + DC_BOT:
            return self.answer(401, {"message": "401: Unauthorized", "code": 0})
        fields, files = multipart(ct, body)
        self.rec["fields"] = fields
        self.files(files)
        pj = json.loads(fields["payload_json"])
        self.rec["payload"] = pj
        if not pj.get("content"):
            raise ValueError("empty content")
        if [f["name"] for f in files] not in ([], ["files[0]"]):
            raise ValueError("file fields must be files[0]")
        if self.too_big(len(body)):
            return self.answer(413, {"message": "Request entity too large", "code": 40005})
        return self.answer(200, {"id": str(self.rec["n"]), "content": pj["content"]})

    def gotify(self, q, body):
        self.rec["service"] = "gotify"
        key = self.headers.get("X-Gotify-Key") or (q.get("token") or [""])[0]
        if key != GO_KEY:
            return self.answer(401, {"error": "Unauthorized", "errorCode": 401, "errorDescription": "you need to provide a valid access token or user credentials to access this api"})
        if not self.headers.get("Content-Type", "").startswith("application/json"):
            raise ValueError("not JSON")
        js = json.loads(body)
        self.rec["json"] = js
        if not isinstance(js.get("message"), str) or not js["message"]:
            return self.answer(400, {"error": "Bad Request", "errorCode": 400, "errorDescription": "Field 'message' is required"})
        return self.answer(200, {"id": self.rec["n"], "appid": 1, "message": js["message"], "title": js.get("title", ""), "priority": js.get("priority", 0)})


srv = ThreadingHTTPServer((A.bind, A.port), H)
srv.daemon_threads = True
if A.tls:
    ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    ctx.load_cert_chain(A.tls[0], A.tls[1])
    srv.socket = ctx.wrap_socket(srv.socket, server_side=True, do_handshake_on_connect=False)
print("mock_push listening on %s:%d%s" % (A.bind, A.port, " (TLS)" if A.tls else ""), flush=True)
srv.serve_forever()

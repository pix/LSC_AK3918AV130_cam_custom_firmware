"""Stand-ins for the Cloudflare v4, DuckDNS and acme-dns APIs, backed by
pebble-challtestsrv (the DNS server Pebble queries). Request shapes follow
the providers' documentation; credentials are checked like the real ones.
Usage: mock_dns_apis.py <port> <challtestsrv management URL> [tls_cert tls_key]
Every request is logged to stdout as one JSON line (the tests read it)."""
import json, ssl, sys, threading, urllib.request
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import urlparse, parse_qs

PORT, MGMT = int(sys.argv[1]), sys.argv[2]
CF_TOKEN, DUCK_TOKEN = "cf-test-token_123", "duck-token-0000"
ZONES = {"example.test": "023e105f4ecef8ad9ca31a8372d0c353"}
records = {}                    # cloudflare id -> (name, value)
acmedns = {}                    # username -> {password, subdomain, values}
ACMEDNS_TARGETS = []            # names whose _acme-challenge CNAMEs to the account (set by /test/acmedns-names)
lock = threading.Lock()

def txt(op, host, value=""):
    body = {"host": host.rstrip(".") + "."}
    if op == "set": body["value"] = value
    urllib.request.urlopen(urllib.request.Request(f"{MGMT}/{op}-txt", json.dumps(body).encode()), timeout=5).read()

def zone_obj(name, zid):        # a realistic, large zone object (tests the camera's JSON token budget)
    return {"id": zid, "name": name, "status": "active", "paused": False, "type": "full", "development_mode": 0,
            "name_servers": ["ns1.example.net", "ns2.example.net"], "original_name_servers": None, "original_registrar": None,
            "original_dnshost": None, "modified_on": "2026-01-01T00:00:00Z", "created_on": "2020-01-01T00:00:00Z",
            "activated_on": "2020-01-01T00:00:00Z", "meta": {"step": 2, "custom_certificate_quota": 0, "page_rule_quota": 3,
            "phishing_detected": False}, "owner": {"id": None, "type": "user", "email": None},
            "account": {"id": "a" * 32, "name": "Test account"}, "tenant": {"id": None, "name": None},
            "tenant_unit": {"id": None}, "permissions": ["#dns_records:edit", "#dns_records:read", "#zone:read"],
            "plan": {"id": "0" * 32, "name": "Free Website", "price": 0, "currency": "USD", "frequency": "", "is_subscribed": False,
                     "can_subscribe": False, "legacy_id": "free", "legacy_discount": False, "externally_managed": False}}

class H(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    def log_message(self, *a): pass
    def send(self, code, body, ctype="application/json"):
        b = body if isinstance(body, bytes) else (json.dumps(body) if not isinstance(body, str) else body).encode()
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(b)))
        self.end_headers()
        self.wfile.write(b)
    def body(self):
        n = int(self.headers.get("Content-Length") or 0)
        return self.rfile.read(n) if n else b""
    def log(self, **kw):
        print(json.dumps({"method": self.command, "path": self.path, **kw}), flush=True)

    def cf(self, method):
        if self.headers.get("Authorization") != "Bearer " + CF_TOKEN:
            self.log(api="cloudflare", auth="bad")
            return self.send(400, {"success": False, "errors": [{"code": 6111, "message": "Invalid format for Authorization header"}],
                                   "messages": [], "result": None})
        u = urlparse(self.path)
        p = u.path[len("/client/v4"):]
        if method == "GET" and p == "/zones":
            name = parse_qs(u.query).get("name", [""])[0]
            res = [zone_obj(name, ZONES[name])] if name in ZONES else []
            self.log(api="cloudflare", op="zones", name=name, found=len(res))
            return self.send(200, {"result": res, "result_info": {"page": 1, "per_page": 20, "total_pages": 1, "count": len(res),
                                   "total_count": len(res)}, "success": True, "errors": [], "messages": []})
        parts = p.strip("/").split("/")
        if len(parts) >= 3 and parts[0] == "zones" and parts[2] == "dns_records" and parts[1] in ZONES.values():
            if method == "POST":
                r = json.loads(self.body())
                ok = r.get("type") == "TXT" and r.get("name", "").startswith("_acme-challenge.") and len(r.get("content", "")) == 43
                if not ok:
                    self.log(api="cloudflare", op="create", bad=r)
                    return self.send(400, {"success": False, "errors": [{"code": 9005, "message": "Content for TXT record is invalid."}]})
                with lock:
                    rid = "%032x" % (len(records) + 1)
                    records[rid] = (r["name"], r["content"])
                txt("set", r["name"], r["content"])
                self.log(api="cloudflare", op="create", name=r["name"], ttl=r.get("ttl"), id=rid)
                return self.send(200, {"result": {"id": rid, "zone_id": parts[1], "name": r["name"], "type": "TXT", "content": r["content"],
                                       "proxiable": False, "proxied": False, "ttl": r.get("ttl", 1), "meta": {}, "comment": None, "tags": []},
                                       "success": True, "errors": [], "messages": []})
            if method == "DELETE" and len(parts) == 4:
                with lock:
                    rec = records.pop(parts[3], None)
                if rec:
                    txt("clear", rec[0])
                    with lock:      # put back the other values of that name still in use
                        for n, v in records.values():
                            if n == rec[0]: txt("set", n, v)
                self.log(api="cloudflare", op="delete", id=parts[3], found=bool(rec))
                return self.send(200 if rec else 404, {"result": {"id": parts[3]}, "success": bool(rec),
                                 "errors": [] if rec else [{"code": 81044, "message": "Record does not exist."}]})
        self.send(404, {"success": False, "errors": [{"code": 7003, "message": "Could not route"}]})

    def duck(self):
        q = parse_qs(urlparse(self.path).query)
        dom, tok = q.get("domains", [""])[0], q.get("token", [""])[0]
        self.log(api="duckdns", domains=dom, txt=q.get("txt", [""])[0], clear=q.get("clear", [""])[0])
        if tok != DUCK_TOKEN or not dom or "." in dom:
            return self.send(200, "KO", "text/plain")
        name = f"_acme-challenge.{dom}.duckdns.org"
        txt("clear", name)                         # DuckDNS keeps a single TXT value per domain
        if q.get("clear", [""])[0] != "true":
            txt("set", name, q["txt"][0])
        self.send(200, "OK\n\n" + q.get("txt", [""])[0] + "\nUPDATED", "text/plain")

    def do_GET(self):
        p = urlparse(self.path).path
        if p.startswith("/client/v4/"): return self.cf("GET")
        if p == "/update": return self.duck()
        self.send(404, "not found", "text/plain")
    def do_DELETE(self):
        if self.path.startswith("/client/v4/"): return self.cf("DELETE")
        self.send(404, "x", "text/plain")
    def do_POST(self):
        p = urlparse(self.path).path
        if p.startswith("/client/v4/"): return self.cf("POST")
        if p == "/test/acmedns-names":
            ACMEDNS_TARGETS[:] = json.loads(self.body()); return self.send(200, {})
        if p == "/register":
            self.body()
            with lock:
                n = len(acmedns) + 1
                user = f"u{n:04d}-0000-4000-8000-000000000000"
                acmedns[user] = {"password": f"pw{n:04d}secretsecretsecretsecretsecret", "subdomain": f"{n:08d}-sub", "values": []}
                a = acmedns[user]
            self.log(api="acmedns", op="register", user=user)
            return self.send(201, {"username": user, "password": a["password"], "fulldomain": a["subdomain"] + ".auth.example.test",
                                   "subdomain": a["subdomain"], "allowfrom": []})
        if p == "/update":
            user, key = self.headers.get("X-Api-User"), self.headers.get("X-Api-Key")
            r = json.loads(self.body() or b"{}")
            a = acmedns.get(user)
            self.log(api="acmedns", op="update", user=user, sub=r.get("subdomain"), txt=r.get("txt"))
            if not a or a["password"] != key or r.get("subdomain") != a["subdomain"]:
                return self.send(401, {"error": "forbidden"})
            if len(r.get("txt", "")) != 43:
                return self.send(400, {"error": "bad_txt"})
            a["values"] = (a["values"] + [r["txt"]])[-2:]      # acme-dns keeps the two latest
            for n in ACMEDNS_TARGETS:                          # stands for the user's CNAMEs
                txt("clear", "_acme-challenge." + n.lstrip("*."))
                for v in a["values"]: txt("set", "_acme-challenge." + n.lstrip("*."), v)
            return self.send(200, {"txt": r["txt"]})
        self.send(404, "x", "text/plain")

srv = ThreadingHTTPServer(("0.0.0.0", PORT), H)
if len(sys.argv) > 4:
    ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    ctx.load_cert_chain(sys.argv[3], sys.argv[4])
    srv.socket = ctx.wrap_socket(srv.socket, server_side=True)
print(json.dumps({"ready": PORT}), flush=True)
srv.serve_forever()

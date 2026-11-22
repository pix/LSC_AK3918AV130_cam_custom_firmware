"""Independent checks of what the camera code produced (run.sh):
- every status JSON line parses;
- the JWS requests verify with Python's cryptography (ES256 over the
  protected header + payload, raw r||s), carry the right nonce / url / jwk
  or kid, and the thumbprint matches RFC 7638.
Usage: check_json.py <custom.json> <acme output dir>"""
import base64, hashlib, json, sys, os
from cryptography.hazmat.primitives.asymmetric import ec, utils
from cryptography.hazmat.primitives import hashes

fails = 0
def check(ok, what):
    global fails
    print(("PASS " if ok else "FAIL ") + what)
    fails += not ok

def b64d(s):
    return base64.urlsafe_b64decode(s + "=" * (-len(s) % 4))

for i, line in enumerate(open(sys.argv[1])):
    try:
        j = json.loads(line)
        check("custom" in j, f"tls status JSON line {i + 1} parses")
    except Exception as e:
        check(False, f"tls status JSON line {i + 1}: {e}: {line[:200]}")

d = sys.argv[2]
try:
    s = json.load(open(os.path.join(d, "status.json")))
    check(len(s["log"]) == 24 and s["state"] == "error", "acme status JSON parses (24 log lines)")
except Exception as e:
    check(False, f"acme status JSON: {e}")

for n in (1, 2, 3):
    body, jwk_s, thumb, nonce = open(os.path.join(d, f"jws{n}.txt")).read().split("\n")[:4]
    req = json.loads(body)
    prot = json.loads(b64d(req["protected"]))
    jwk = json.loads(jwk_s)
    pub = ec.EllipticCurvePublicNumbers(int.from_bytes(b64d(jwk["x"]), "big"), int.from_bytes(b64d(jwk["y"]), "big"),
                                        ec.SECP256R1()).public_key()
    sig = b64d(req["signature"])
    check(len(sig) == 64, f"jws{n}: raw 64-byte signature")
    der = utils.encode_dss_signature(int.from_bytes(sig[:32], "big"), int.from_bytes(sig[32:], "big"))
    try:
        pub.verify(der, (req["protected"] + "." + req["payload"]).encode(), ec.ECDSA(hashes.SHA256()))
        check(True, f"jws{n}: ES256 signature verifies")
    except Exception:
        check(False, f"jws{n}: ES256 signature verifies")
    check(prot["alg"] == "ES256" and prot["nonce"] == nonce and prot["url"].endswith("/new-acct"), f"jws{n}: alg/nonce/url")
    if n < 3:
        check("jwk" in prot and "kid" not in prot and prot["jwk"] == jwk, f"jws{n}: jwk header (new account)")
        check(json.loads(b64d(req["payload"])) == {"termsOfServiceAgreed": True}, f"jws{n}: payload")
    else:
        check("kid" in prot and "jwk" not in prot, "jws3: kid header")
        check(req["payload"] == "", "jws3: empty payload (POST-as-GET)")
    canon = json.dumps({"crv": jwk["crv"], "kty": jwk["kty"], "x": jwk["x"], "y": jwk["y"]}, separators=(",", ":"), sort_keys=True)
    t = base64.urlsafe_b64encode(hashlib.sha256(canon.encode()).digest()).rstrip(b"=").decode()
    check(t == thumb, f"jws{n}: RFC 7638 thumbprint")
print(f"check_json: {fails} failed")
sys.exit(1 if fails else 0)

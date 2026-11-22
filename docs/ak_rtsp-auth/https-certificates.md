# HTTPS certificates: camera CA, your own certificate, ACME

The camera always has its own certificate authority (Security > HTTPS). Install that CA once per device and the
page, RTSPS (322) and ONVIF over HTTPS are trusted for the camera's addresses and the DNS names you list.
Two other ways avoid installing anything on the devices.

## Your own certificate (upload)

Security > HTTPS > Your own certificate: upload the chain (server certificate first, then the intermediates) and
its unencrypted private key, as two files or one file holding both.

- Accepted: EC P-256 / P-384 or RSA 2048-4096 keys (PKCS#8, SEC1 or traditional PEM), up to 8 certificates, up to
  16 DNS names (wildcards `*.example.org`) and 8 IPv4 addresses in subjectAltName. Refused with a clear message:
  wrong key, chain in the wrong order, encrypted key, expired / not yet valid, a CA certificate, no serverAuth,
  key usage without digitalSignature, no name or address.
- Which connections get it: clients whose SNI is one of its names, and clients that connect to one of its IP
  addresses without SNI (what browsers do for `https://192.168.1.5/`). Everything else keeps the camera
  certificate, so a wrong certificate cannot lock you out.
- **Use it for every connection** (off by default): replaces the camera certificate everywhere. Clients using a
  name or address the certificate does not cover will see a warning. While the certificate is expired or after
  it is removed, the camera certificate comes back by itself.
- Its names are accepted by the host check (DNS-rebinding protection) like the HTTPS DNS names.
- From a script, e.g. a certbot / acme.sh deploy hook on another machine:

      cat fullchain.pem privkey.pem | curl --digest -u admin:PASSWORD -H "X-Requested-With: lsc" \
           --data-binary @- https://camera.example.org/api/tls/custom

  `POST /api/tls/custom/clear` removes it; `GET /api/tls` describes it (`custom`, `custom_all`).
  `POST /api/tls` with `custom_all=1|0` switches "every connection".

## ACME (Let's Encrypt or another ACME server)

Security > Public certificate (ACME). The camera gets the certificate itself and renews it 30 days before it
expires. It becomes "your own certificate" (it replaces an uploaded one).

| Validation | What you need | Camera reachable from the internet? |
|---|---|---|
| HTTP (port 80) | each name points to your public address, port 80 forwarded to the camera | yes, port 80 |
| Cloudflare | API token with Zone > DNS > Edit and Zone > Zone > Read | no |
| DuckDNS | your DuckDNS token; names under `yours.duckdns.org` | no |
| acme-dns | an acme-dns server; "Register" creates the account, then add `_acme-challenge.NAME CNAME <target>` | no |

Wildcard names need a DNS validation. A name can point to the camera's private address (e.g. `cam.example.org`
-> `192.168.1.5` in your DNS): with a DNS validation, nothing has to be open to the internet.

Settings (`/api/config`, also MQTT / backup): `acme.enabled`, `acme.server` (`letsencrypt`, `staging` or a
directory URL), `acme.email`, `acme.domains`, `acme.challenge` (`http`, `cloudflare`, `duckdns`, `acmedns`),
`acme.dns_token` (secret, never shown), `acme.dns_api` (acme-dns server; for the others only to test with a
stand-in), `acme.dns_user`, `acme.dns_sub`, `acme.dns_target`, `acme.dns_wait`. "Extra trusted CA"
(`POST /api/acme/ca`) adds a CA for a private ACME server or acme-dns; Let's Encrypt, Cloudflare and DuckDNS are
verified with the built-in roots (`acme_roots.pem`).

Behaviour that protects the camera and your rate limits:
- everything runs in its own thread; the web server never waits on the network (acme-dns registration included);
- requests time out (connect 15 s, response 60 s), polling is bounded (10 min), responses over 256 KB are refused;
- failures retry after 1 h, doubling to 24 h; after a success the automatic check waits at least 12 h;
- a new key for every certificate; the account key stays on the camera (`ak_rtsp_acme_account.key`, 0600);
- a replaced certificate is freed as soon as no handshake uses it, so frequent uploads (deploy hooks) or
  renewals do not grow memory;
- API tokens are never logged or shown; the activity log is in the page (Activity).

## HTTP -> HTTPS redirect

With "Redirect HTTP to HTTPS" on, port 80 answers browsers with a 301 to `https://`. These stay on plain HTTP,
because the clients using them cannot follow a redirect or do not trust the camera CA:

- `/api/tls/ca.crt` (a new device fetches the CA before it trusts HTTPS);
- `/hook/...` webhooks (automations do not follow a POST redirect);
- `/onvif/...` and `/snapshot.jpg` (the snapshot URL ONVIF hands out): NVRs and Home Assistant's ONVIF
  integration never follow a redirect. WS-Security and HTTP Digest never send the password itself.

**Home Assistant ONVIF: use port 80.** The integration only speaks plain HTTP. On port 443 it used to fail
with "[Errno 104] Connection reset by peer": the HTTPS port dropped the plain request. A plain HTTP request
on 443 now gets a readable `400` ("this is the camera's HTTPS port: use http:// on port 80").

## Reset button

Holding the reset button 10 s (or a `reset_auth` file on the SD card) resets the login to admin/admin and also
turns off "redirect HTTP to HTTPS" and "use it for every connection". Certificates, the camera CA, its DNS
names and the ACME settings are kept. `reset_settings` on the SD card only resets picture / night settings.

## Files (`/etc/config`, included in the settings backup)

`ak_rtsp_tls_custom.crt` / `.key` (0600) / `.src` (upload | acme), `ak_rtsp_acme_account.key` (0600),
`ak_rtsp_acme_account` (directory + account URL), `ak_rtsp_acme_ca.crt`; `custom_all=` in `ak_rtsp_tls.conf`.

## Tests

- `tests/tls/acme/run.sh` (PC, ASan + UBSan + LeakSanitizer; `VALGRIND=1` also runs both programs under
  valgrind memcheck): upload validation for 25 kinds of certificates and keys, SNI and
  address selection over real handshakes, every-connection mode, reset path, persistence, expiry; JSON reader
  (+ 60 000 fuzzed documents), base64url, URL / name / setting validation, HTTP client against a fake server
  (lengths, chunked, slow, oversized, garbage), JWS checked independently with Python `cryptography`.
- `tests/tls/acme/e2e_pebble.sh host|camera <ip>`: the real flow against Pebble (Let's Encrypt's test server,
  which also rejects nonces at random): HTTP-01 on the camera's port 80, Cloudflare / DuckDNS / acme-dns through
  stand-in APIs (HTTP and HTTPS) feeding Pebble's DNS, wildcards, account cache, renewal decision, and the
  failures (bad tokens, unknown zone, untrusted server, wrong URL, server down, validation failure).
- `tests/tls/acme/camera_upload_test.sh <ip> <fixtures>`: the upload API on the camera, SNI on 443 and 322,
  every-connection mode, the reset flag, an address certificate verified by curl, RSA-4096 handshake time.
- `tests/tls/acme/camera_stress.sh`: parallel handshakes + uploads during an RTSP stream; no restart, bounded
  memory.

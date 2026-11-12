# Running the camera behind Traefik

Tested with Traefik v3.6.25 (2026-11-12): admin page (all tabs, saving, live view over wss, snapshots,
recordings page), RTSPS main and `/sub` with audio, both with TLS terminated at Traefik and with TLS
passthrough, and login lockout attributed to the real client.

## On the camera (Security tab)
- **Reverse proxy address** (`web.proxies`): Traefik's IP as the camera sees it. Requests from that address
  are attributed to the client in `X-Forwarded-For` (HTTP) or in the PROXY protocol header (RTSP/RTSPS), so a
  wrong password from outside locks out that client, not Traefik. They also skip the per-address connection
  cap and the Host check, and are never redirected to HTTPS. Headers from any other address are ignored.
- **DNS names** (HTTPS certificate): only needed for TLS passthrough (clients then see the camera's own
  certificate and check the public name against it).
- Download the camera CA (`/api/tls/ca.crt`) for Traefik's `rootCAs` (pins the camera).

## Traefik dynamic configuration (file provider)
```yaml
http:
  routers:
    cam:
      rule: "Host(`cam.example.org`)"
      entryPoints: [websecure]
      tls: {}                               # your public certificate (e.g. certResolver)
      service: cam
  services:
    cam:
      loadBalancer:
        serversTransport: camtls
        servers:
          - url: "https://192.168.1.50"      # the camera (HTTPS, port 443)
  serversTransports:
    camtls:
      rootCAs: ["/etc/traefik/camera-ca.crt"]

tcp:
  routers:
    # RTSPS, TLS terminated by Traefik and re-encrypted to the camera
    rtsps:
      entryPoints: [rtsps]                  # e.g. address ":322"
      rule: "HostSNI(`cam.example.org`)"
      tls: {}
      service: cam-rtsps
    # or: TLS passthrough (clients see the camera certificate)
    # rtsps-pass:
    #   entryPoints: [rtsps]
    #   rule: "HostSNI(`cam.example.org`)"
    #   tls: { passthrough: true }
    #   service: cam-rtsps-pass
  services:
    cam-rtsps:
      loadBalancer:
        serversTransport: camtcp
        servers:
          - address: "192.168.1.50:322"
            tls: true
    cam-rtsps-pass:
      loadBalancer:
        serversTransport: camtcp-pass
        servers:
          - address: "192.168.1.50:322"
  serversTransports:
    camtcp:
      proxyProtocol: { version: 2 }         # gives the camera the client address
      tls:
        serverName: "192.168.1.50"          # required for TCP TLS; the camera IP or one of its DNS names
        rootCAs: ["/etc/traefik/camera-ca.crt"]
    camtcp-pass:
      proxyProtocol: { version: 1 }
```

## Notes
- RTSPS is TCP only (interleaved). Clients must use TCP transport (`ffmpeg -rtsp_transport tcp`, go2rtc and
  Home Assistant do by default); a UDP SETUP over RTSPS gets 461.
- HostSNI routing needs the client to send SNI, i.e. connect with the host name, not an IP.
- Serve the admin page at the root of a host name (not under a path prefix): the page uses absolute paths
  and Digest login is bound to the exact request path.
- Plain RTSP (554) can be proxied the same way with a non-TLS TCP router (`HostSNI(\`*\`)` on its own
  entry point) and `proxyProtocol`.

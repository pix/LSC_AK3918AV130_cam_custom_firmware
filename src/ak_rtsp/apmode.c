#include "apmode.h"
#include "apframe.h"

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>
#include <poll.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <net/if.h>
#include <linux/netlink.h>
#include <linux/genetlink.h>
#include <linux/nl80211.h>

#define MAX_STA 4

static int       g_cmd = -1, g_ev = -1;     /* command socket (acks) / event socket (frames) */
static uint16_t  g_fam;
static uint32_t  g_seq;
static int       g_ifindex;
static char      g_ifname[IFNAMSIZ];
static ap_cfg_t  g_cfg;
static volatile int g_running, g_stop;
static pthread_t g_tid;
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static char      g_err[96] = "";
static struct { int used; uint8_t mac[6]; uint16_t aid; } g_sta[MAX_STA];

static void set_err(const char *what, int e)
{
    snprintf(g_err, sizeof(g_err), "%s: %s", what, e ? strerror(e < 0 ? -e : e) : "ok");
    printf("[ap] %s\n", g_err);
}

/* ---- tiny generic-netlink message builder ----------------------------------- */
typedef struct { uint8_t b[1536]; size_t len; int bad; } msg_t;

static void m_init(msg_t *m, uint16_t type, uint8_t cmd)
{
    memset(m, 0, sizeof(*m));
    m->len = NLMSG_HDRLEN + GENL_HDRLEN;
    struct nlmsghdr *h = (struct nlmsghdr *)m->b;
    h->nlmsg_type = type;
    h->nlmsg_flags = NLM_F_REQUEST | NLM_F_ACK;
    struct genlmsghdr *g = (struct genlmsghdr *)(m->b + NLMSG_HDRLEN);
    g->cmd = cmd;
    g->version = 0;
}

static void m_put(msg_t *m, uint16_t type, const void *d, size_t n)
{
    size_t need = NLA_ALIGN(NLA_HDRLEN + n);
    if (m->bad || need > sizeof(m->b) - m->len) { m->bad = 1; return; }
    struct nlattr *a = (struct nlattr *)(m->b + m->len);
    a->nla_type = type;
    a->nla_len = (uint16_t)(NLA_HDRLEN + n);
    if (n) memcpy((uint8_t *)a + NLA_HDRLEN, d, n);
    m->len += need;
}
static void m_u32(msg_t *m, uint16_t t, uint32_t v) { m_put(m, t, &v, 4); }
static void m_u16(msg_t *m, uint16_t t, uint16_t v) { m_put(m, t, &v, 2); }
static void m_flag(msg_t *m, uint16_t t) { m_put(m, t, NULL, 0); }

static int nl_open(void)
{
    int fd = socket(AF_NETLINK, SOCK_RAW | SOCK_CLOEXEC, NETLINK_GENERIC);
    if (fd < 0) return -1;
    struct sockaddr_nl a;
    memset(&a, 0, sizeof(a));
    a.nl_family = AF_NETLINK;
    if (bind(fd, (struct sockaddr *)&a, sizeof(a)) < 0) { close(fd); return -1; }
    return fd;
}

/* Send m on fd and wait for its ACK/error. If reply_attr is non-zero, the
 * first u16 attribute of that type in a data reply is stored in *reply. */
static int nl_call(int fd, msg_t *m, uint16_t reply_attr, uint16_t *reply)
{
    if (m->bad) return -EMSGSIZE;
    struct nlmsghdr *h = (struct nlmsghdr *)m->b;
    h->nlmsg_len = (uint32_t)m->len;
    h->nlmsg_seq = ++g_seq;
    struct sockaddr_nl k;
    memset(&k, 0, sizeof(k));
    k.nl_family = AF_NETLINK;
    if (sendto(fd, m->b, m->len, 0, (struct sockaddr *)&k, sizeof(k)) < 0) return -errno;

    static uint8_t rb[8192];
    for (int tries = 0; tries < 20; tries++) {
        struct pollfd p = { fd, POLLIN, 0 };
        if (poll(&p, 1, 2000) <= 0) return -ETIMEDOUT;
        ssize_t n = recv(fd, rb, sizeof(rb), 0);
        if (n < 0) { if (errno == EINTR) continue; return -errno; }
        for (struct nlmsghdr *r = (struct nlmsghdr *)rb; NLMSG_OK(r, (size_t)n); r = NLMSG_NEXT(r, n)) {
            if (r->nlmsg_seq != g_seq) continue;
            if (r->nlmsg_type == NLMSG_ERROR) {
                if (r->nlmsg_len < NLMSG_LENGTH(sizeof(struct nlmsgerr))) return -EPROTO;
                return ((struct nlmsgerr *)NLMSG_DATA(r))->error;   /* 0 = ack */
            }
            if (reply_attr && reply && r->nlmsg_len >= NLMSG_LENGTH(GENL_HDRLEN)) {
                uint8_t *p = (uint8_t *)NLMSG_DATA(r) + GENL_HDRLEN;
                size_t left = r->nlmsg_len - NLMSG_LENGTH(GENL_HDRLEN);
                while (left >= NLA_HDRLEN) {
                    struct nlattr *a = (struct nlattr *)p;
                    if (a->nla_len < NLA_HDRLEN || a->nla_len > left) break;
                    if ((a->nla_type & NLA_TYPE_MASK) == reply_attr && a->nla_len >= NLA_HDRLEN + 2)
                        memcpy(reply, p + NLA_HDRLEN, 2);
                    size_t step = NLA_ALIGN(a->nla_len);
                    if (step > left) break;
                    p += step; left -= step;
                }
            }
        }
    }
    return -ETIMEDOUT;
}

static int resolve_family(void)
{
    msg_t m;
    m_init(&m, GENL_ID_CTRL, CTRL_CMD_GETFAMILY);
    m_put(&m, CTRL_ATTR_FAMILY_NAME, "nl80211", 8);
    g_fam = 0;
    int r = nl_call(g_cmd, &m, CTRL_ATTR_FAMILY_ID, &g_fam);
    return (r == 0 && g_fam) ? 0 : (r ? r : -ENOENT);
}

static int if_updown(int up)
{
    int s = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (s < 0) return -errno;
    struct ifreq ifr;
    memset(&ifr, 0, sizeof(ifr));
    snprintf(ifr.ifr_name, sizeof(ifr.ifr_name), "%s", g_ifname);
    int r = ioctl(s, SIOCGIFFLAGS, &ifr);
    if (r == 0) {
        if (up) ifr.ifr_flags |= IFF_UP; else ifr.ifr_flags &= ~IFF_UP;
        r = ioctl(s, SIOCSIFFLAGS, &ifr);
    }
    int e = r ? -errno : 0;
    close(s);
    return e;
}

static int set_iftype(uint32_t t)
{
    msg_t m;
    m_init(&m, g_fam, NL80211_CMD_SET_INTERFACE);
    m_u32(&m, NL80211_ATTR_IFINDEX, (uint32_t)g_ifindex);
    m_u32(&m, NL80211_ATTR_IFTYPE, t);
    return nl_call(g_cmd, &m, 0, NULL);
}

static int send_frame(const uint8_t *f, size_t n)
{
    msg_t m;
    m_init(&m, g_fam, NL80211_CMD_FRAME);
    m_u32(&m, NL80211_ATTR_IFINDEX, (uint32_t)g_ifindex);
    m_put(&m, NL80211_ATTR_FRAME, f, n);
    m_flag(&m, NL80211_ATTR_DONT_WAIT_FOR_ACK);
    int r = nl_call(g_cmd, &m, 0, NULL);
    if (r == -EINVAL || r == -EOPNOTSUPP) {      /* driver without no-ack TX */
        m_init(&m, g_fam, NL80211_CMD_FRAME);
        m_u32(&m, NL80211_ATTR_IFINDEX, (uint32_t)g_ifindex);
        m_put(&m, NL80211_ATTR_FRAME, f, n);
        r = nl_call(g_cmd, &m, 0, NULL);
    }
    return r;
}

static int del_station(const uint8_t *mac)
{
    msg_t m;
    m_init(&m, g_fam, NL80211_CMD_DEL_STATION);
    m_u32(&m, NL80211_ATTR_IFINDEX, (uint32_t)g_ifindex);
    m_put(&m, NL80211_ATTR_MAC, mac, 6);
    return nl_call(g_cmd, &m, 0, NULL);
}

static int add_station(const uint8_t *mac, uint16_t aid, const ap_rx_t *rx)
{
    struct nl80211_sta_flag_update fl;
    msg_t m;
    /* 1st try: full client state in one go (mac80211 FULL_AP_CLIENT_STATE). */
    m_init(&m, g_fam, NL80211_CMD_NEW_STATION);
    m_u32(&m, NL80211_ATTR_IFINDEX, (uint32_t)g_ifindex);
    m_put(&m, NL80211_ATTR_MAC, mac, 6);
    m_u16(&m, NL80211_ATTR_STA_AID, aid);
    m_u16(&m, NL80211_ATTR_STA_LISTEN_INTERVAL, rx->listen_int ? rx->listen_int : 10);
    m_put(&m, NL80211_ATTR_STA_SUPPORTED_RATES, rx->rates, rx->n_rates);
    fl.mask = (1u << NL80211_STA_FLAG_AUTHENTICATED) | (1u << NL80211_STA_FLAG_ASSOCIATED) |
              (1u << NL80211_STA_FLAG_AUTHORIZED);
    fl.set = fl.mask;
    m_put(&m, NL80211_ATTR_STA_FLAGS2, &fl, sizeof(fl));
    int r = nl_call(g_cmd, &m, 0, NULL);
    if (r == 0) return 0;

    /* Fallback: add plain, then authorize. */
    m_init(&m, g_fam, NL80211_CMD_NEW_STATION);
    m_u32(&m, NL80211_ATTR_IFINDEX, (uint32_t)g_ifindex);
    m_put(&m, NL80211_ATTR_MAC, mac, 6);
    m_u16(&m, NL80211_ATTR_STA_AID, aid);
    m_u16(&m, NL80211_ATTR_STA_LISTEN_INTERVAL, rx->listen_int ? rx->listen_int : 10);
    m_put(&m, NL80211_ATTR_STA_SUPPORTED_RATES, rx->rates, rx->n_rates);
    r = nl_call(g_cmd, &m, 0, NULL);
    if (r != 0) return r;
    m_init(&m, g_fam, NL80211_CMD_SET_STATION);
    m_u32(&m, NL80211_ATTR_IFINDEX, (uint32_t)g_ifindex);
    m_put(&m, NL80211_ATTR_MAC, mac, 6);
    fl.mask = 1u << NL80211_STA_FLAG_AUTHORIZED;
    fl.set = fl.mask;
    m_put(&m, NL80211_ATTR_STA_FLAGS2, &fl, sizeof(fl));
    return nl_call(g_cmd, &m, 0, NULL);
}

static int register_frame(uint16_t type)
{
    msg_t m;
    m_init(&m, g_fam, NL80211_CMD_REGISTER_FRAME);
    m_u32(&m, NL80211_ATTR_IFINDEX, (uint32_t)g_ifindex);
    m_u16(&m, NL80211_ATTR_FRAME_TYPE, type);
    m_put(&m, NL80211_ATTR_FRAME_MATCH, NULL, 0);
    return nl_call(g_ev, &m, 0, NULL);   /* frames are delivered to the registering socket */
}

/* ---- station table ---------------------------------------------------------- */
static int sta_find(const uint8_t *mac)
{
    for (int i = 0; i < MAX_STA; i++) if (g_sta[i].used && !memcmp(g_sta[i].mac, mac, 6)) return i;
    return -1;
}

static void sta_forget(const uint8_t *mac)
{
    int i = sta_find(mac);
    if (i >= 0) { del_station(mac); g_sta[i].used = 0; }
}

static void mac_str(const uint8_t *m, char *o)
{
    snprintf(o, 18, "%02x:%02x:%02x:%02x:%02x:%02x", m[0], m[1], m[2], m[3], m[4], m[5]);
}

/* ---- frame handling ---------------------------------------------------------- */
static void handle_frame(const uint8_t *f, size_t n)
{
    ap_rx_t rx;
    uint8_t out[512];
    size_t ol;
    char ms[18];
    if (ap_parse(f, n, &rx) != 0) return;
    mac_str(rx.sa, ms);

    switch (rx.subtype_fc) {
    case FC_PROBE_REQ:
        if (ap_probe_matches(&g_cfg, &rx) && (ol = ap_build_probe_resp(&g_cfg, rx.sa, out, sizeof(out))))
            send_frame(out, ol);
        break;
    case FC_AUTH:
        if (memcmp(rx.bssid, g_cfg.bssid, 6) != 0 || rx.auth_seq != 1) break;
        pthread_mutex_lock(&g_lock);
        sta_forget(rx.sa);   /* re-auth: start clean */
        pthread_mutex_unlock(&g_lock);
        if ((ol = ap_build_auth_resp(&g_cfg, rx.sa, rx.auth_alg == 0 ? 0 : 13, out, sizeof(out))))
            send_frame(out, ol);
        printf("[ap] auth from %s alg %u\n", ms, rx.auth_alg);
        break;
    case FC_ASSOC_REQ:
    case FC_REASSOC_REQ: {
        if (memcmp(rx.bssid, g_cfg.bssid, 6) != 0) break;
        int reassoc = rx.subtype_fc == FC_REASSOC_REQ;
        pthread_mutex_lock(&g_lock);
        int slot = sta_find(rx.sa);
        if (slot < 0) for (int i = 0; i < MAX_STA; i++) if (!g_sta[i].used) { slot = i; break; }
        uint16_t status = 0, aid = 0;
        if (slot < 0) status = 17;                         /* AP full */
        else if (rx.n_rates == 0) status = 18;             /* no common rates */
        else {
            if (g_sta[slot].used) { del_station(rx.sa); g_sta[slot].used = 0; }
            aid = (uint16_t)(slot + 1);
            int r = add_station(rx.sa, aid, &rx);
            if (r == 0) { g_sta[slot].used = 1; memcpy(g_sta[slot].mac, rx.sa, 6); g_sta[slot].aid = aid; }
            else { status = 1; printf("[ap] NEW_STATION failed for %s: %s\n", ms, strerror(-r)); }
        }
        pthread_mutex_unlock(&g_lock);
        if ((ol = ap_build_assoc_resp(&g_cfg, rx.sa, reassoc, status, aid, out, sizeof(out))))
            send_frame(out, ol);
        printf("[ap] %sassoc from %s -> status %u aid %u\n", reassoc ? "re" : "", ms, status, aid);
        break;
    }
    case FC_DISASSOC:
    case FC_DEAUTH:
        pthread_mutex_lock(&g_lock);
        sta_forget(rx.sa);
        pthread_mutex_unlock(&g_lock);
        printf("[ap] %s left\n", ms);
        break;
    }
}

static void *ev_thread(void *arg)
{
    (void)arg;
    static uint8_t rb[8192];
    while (!g_stop) {
        struct pollfd p = { g_ev, POLLIN, 0 };
        if (poll(&p, 1, 500) <= 0) continue;
        ssize_t n = recv(g_ev, rb, sizeof(rb), MSG_DONTWAIT);
        if (n <= 0) continue;
        for (struct nlmsghdr *h = (struct nlmsghdr *)rb; NLMSG_OK(h, (size_t)n); h = NLMSG_NEXT(h, n)) {
            if (h->nlmsg_type != g_fam || h->nlmsg_len < NLMSG_LENGTH(GENL_HDRLEN)) continue;
            struct genlmsghdr *g = NLMSG_DATA(h);
            if (g->cmd != NL80211_CMD_FRAME) continue;
            uint8_t *a = (uint8_t *)g + GENL_HDRLEN;
            size_t left = h->nlmsg_len - NLMSG_LENGTH(GENL_HDRLEN);
            while (left >= NLA_HDRLEN) {
                struct nlattr *at = (struct nlattr *)a;
                if (at->nla_len < NLA_HDRLEN || at->nla_len > left) break;
                if ((at->nla_type & NLA_TYPE_MASK) == NL80211_ATTR_FRAME)
                    handle_frame(a + NLA_HDRLEN, at->nla_len - NLA_HDRLEN);
                size_t step = NLA_ALIGN(at->nla_len);
                if (step > left) break;
                a += step; left -= step;
            }
        }
    }
    return NULL;
}


/* ---- capability query (non-disruptive: interface stays in station mode) ----- */
static void nla_iter(const uint8_t *p, size_t left, void (*cb)(uint16_t, const uint8_t *, size_t, void *), void *ctx)
{
    while (left >= NLA_HDRLEN) {
        const struct nlattr *a = (const struct nlattr *)p;
        if (a->nla_len < NLA_HDRLEN || a->nla_len > left) break;
        cb((uint16_t)(a->nla_type & NLA_TYPE_MASK), p + NLA_HDRLEN, a->nla_len - NLA_HDRLEN, ctx);
        size_t step = NLA_ALIGN(a->nla_len);
        if (step > left) break;
        p += step; left -= step;
    }
}

typedef struct { char *o; size_t cap, n; int ap_sme, probe_off; uint32_t features; } caps_t;
static void capf(caps_t *c, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
#include <stdarg.h>
static void capf(caps_t *c, const char *fmt, ...)
{
    if (c->n >= c->cap) return;
    va_list ap; va_start(ap, fmt);
    int k = vsnprintf(c->o + c->n, c->cap - c->n, fmt, ap);
    va_end(ap);
    if (k > 0) c->n += (size_t)k;
    if (c->n > c->cap) c->n = c->cap;
}

static void cb_iftypes(uint16_t t, const uint8_t *v, size_t l, void *ctx)
{ (void)v; (void)l; capf((caps_t *)ctx, " %u", t); }

static void cb_frametypes_inner(uint16_t t, const uint8_t *v, size_t l, void *ctx)
{
    if (t == NL80211_ATTR_FRAME_TYPE && l >= 2) capf((caps_t *)ctx, " 0x%02x", v[0]);
}
static void cb_frametypes(uint16_t iftype, const uint8_t *v, size_t l, void *ctx)
{
    capf((caps_t *)ctx, "\n    iftype %u:", iftype);
    nla_iter(v, l, cb_frametypes_inner, ctx);
}

static void cb_wiphy(uint16_t t, const uint8_t *v, size_t l, void *ctx)
{
    caps_t *c = ctx;
    switch (t) {
    case NL80211_ATTR_DEVICE_AP_SME:     c->ap_sme = 1; capf(c, "device_ap_sme: yes\n"); break;
    case NL80211_ATTR_PROBE_RESP_OFFLOAD: if (l >= 4) { memcpy(&c->probe_off, v, 4); capf(c, "probe_resp_offload: 0x%x\n", c->probe_off); } break;
    case NL80211_ATTR_FEATURE_FLAGS:     if (l >= 4) { memcpy(&c->features, v, 4); capf(c, "feature_flags: 0x%08x (full_ap_client_state=%d)\n", c->features, !!(c->features & NL80211_FEATURE_FULL_AP_CLIENT_STATE)); } break;
    case NL80211_ATTR_SUPPORTED_IFTYPES: capf(c, "supported_iftypes:"); nla_iter(v, l, cb_iftypes, c); capf(c, "   (3=AP)\n"); break;
    case NL80211_ATTR_RX_FRAME_TYPES:    capf(c, "rx_frame_types (registrable):"); nla_iter(v, l, cb_frametypes, c); capf(c, "\n"); break;
    case NL80211_ATTR_TX_FRAME_TYPES:    capf(c, "tx_frame_types:"); nla_iter(v, l, cb_frametypes, c); capf(c, "\n"); break;
    case NL80211_ATTR_WIPHY_NAME:        capf(c, "wiphy: %.*s\n", (int)l, (const char *)v); break;
    case NL80211_ATTR_MAX_AP_ASSOC_STA:  if (l >= 4) { uint32_t x; memcpy(&x, v, 4); capf(c, "max_ap_assoc_sta: %u\n", x); } break;
    }
}

int apmode_caps(const char *ifname, char *out, size_t cap)
{
    caps_t c = { out, cap, 0, 0, 0, 0 };
    out[0] = 0;
    int ifi = (int)if_nametoindex(ifname);
    int fd = nl_open();
    if (!ifi || fd < 0) { if (fd >= 0) close(fd); return -ENODEV; }
    int save = g_cmd; g_cmd = fd;
    int r = resolve_family();
    if (r) { g_cmd = save; close(fd); return r; }
    msg_t m;
    m_init(&m, g_fam, NL80211_CMD_GET_WIPHY);
    struct nlmsghdr *h = (struct nlmsghdr *)m.b;
    h->nlmsg_flags = NLM_F_REQUEST;
    m_u32(&m, NL80211_ATTR_IFINDEX, (uint32_t)ifi);
    h->nlmsg_len = (uint32_t)m.len;
    h->nlmsg_seq = ++g_seq;
    struct sockaddr_nl k; memset(&k, 0, sizeof(k)); k.nl_family = AF_NETLINK;
    if (sendto(fd, m.b, m.len, 0, (struct sockaddr *)&k, sizeof(k)) < 0) { r = -errno; goto done; }
    static uint8_t rb[65536];
    struct pollfd p = { fd, POLLIN, 0 };
    if (poll(&p, 1, 3000) <= 0) { r = -ETIMEDOUT; goto done; }
    ssize_t n = recv(fd, rb, sizeof(rb), 0);
    if (n <= 0) { r = -EIO; goto done; }
    for (struct nlmsghdr *rh = (struct nlmsghdr *)rb; NLMSG_OK(rh, (size_t)n); rh = NLMSG_NEXT(rh, n)) {
        if (rh->nlmsg_type == NLMSG_ERROR) { r = ((struct nlmsgerr *)NLMSG_DATA(rh))->error; continue; }
        if (rh->nlmsg_type != g_fam || rh->nlmsg_len < NLMSG_LENGTH(GENL_HDRLEN)) continue;
        nla_iter((uint8_t *)NLMSG_DATA(rh) + GENL_HDRLEN, rh->nlmsg_len - NLMSG_LENGTH(GENL_HDRLEN), cb_wiphy, &c);
    }
    if (!c.ap_sme) capf(&c, "device_ap_sme: no\n");
done:
    g_cmd = save;
    close(fd);
    return r;
}

/* ---- public ------------------------------------------------------------------- */
static void close_socks(void)
{
    if (g_cmd >= 0) close(g_cmd);
    if (g_ev >= 0) close(g_ev);
    g_cmd = g_ev = -1;
}

static int read_mac(const char *ifname, uint8_t *mac)
{
    char p[64];
    snprintf(p, sizeof(p), "/sys/class/net/%s/address", ifname);
    FILE *f = fopen(p, "r");
    if (!f) return -1;
    unsigned v[6];
    int ok = fscanf(f, "%x:%x:%x:%x:%x:%x", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5]) == 6;
    fclose(f);
    if (!ok) return -1;
    for (int i = 0; i < 6; i++) mac[i] = (uint8_t)v[i];
    return 0;
}

int apmode_start(const char *ifname, const char *ssid, int channel)
{
    if (g_running) return 0;
    if (strlen(ssid) > AP_SSID_MAX || channel < 1 || channel > 13) return -EINVAL;
    snprintf(g_ifname, sizeof(g_ifname), "%s", ifname);
    g_ifindex = (int)if_nametoindex(ifname);
    if (!g_ifindex) { set_err("if_nametoindex", errno); return -ENODEV; }

    memset(&g_cfg, 0, sizeof(g_cfg));
    if (read_mac(ifname, g_cfg.bssid) != 0) { set_err("read mac", EIO); return -EIO; }
    snprintf(g_cfg.ssid, sizeof(g_cfg.ssid), "%s", ssid);
    g_cfg.channel = (uint8_t)channel;
    g_cfg.beacon_int = 100;
    memset(g_sta, 0, sizeof(g_sta));

    g_cmd = nl_open();
    g_ev = nl_open();
    if (g_cmd < 0 || g_ev < 0) { set_err("netlink socket", errno); close_socks(); return -EIO; }
    int r = resolve_family();
    if (r) { set_err("nl80211 family", r); close_socks(); return r; }

    if_updown(0);
    r = set_iftype(NL80211_IFTYPE_AP);
    if (r) { set_err("set iftype AP", r); goto fail; }
    r = if_updown(1);
    if (r) { set_err("interface up", r); goto fail; }

    static const uint16_t types[] = { FC_PROBE_REQ, FC_AUTH, FC_ASSOC_REQ, FC_REASSOC_REQ, FC_DISASSOC, FC_DEAUTH };
    int registered = 0;
    for (size_t i = 0; i < sizeof(types) / sizeof(types[0]); i++) {
        int rr = register_frame(types[i]);
        if (rr == 0) registered++;
        else printf("[ap] register frame 0x%02x: %s (continuing)\n", types[i], strerror(-rr));
    }
    printf("[ap] %d/%zu management frame types registered\n", registered, sizeof(types) / sizeof(types[0]));

    uint8_t head[256], tail[64];
    size_t hl = ap_build_beacon_head(&g_cfg, head, sizeof(head));
    size_t tl = ap_build_beacon_tail(&g_cfg, tail, sizeof(tail));
    msg_t m;
    m_init(&m, g_fam, NL80211_CMD_START_AP);
    m_u32(&m, NL80211_ATTR_IFINDEX, (uint32_t)g_ifindex);
    m_put(&m, NL80211_ATTR_BEACON_HEAD, head, hl);
    m_put(&m, NL80211_ATTR_BEACON_TAIL, tail, tl);
    m_u32(&m, NL80211_ATTR_BEACON_INTERVAL, g_cfg.beacon_int);
    m_u32(&m, NL80211_ATTR_DTIM_PERIOD, 2);
    m_put(&m, NL80211_ATTR_SSID, g_cfg.ssid, strlen(g_cfg.ssid));
    m_u32(&m, NL80211_ATTR_HIDDEN_SSID, NL80211_HIDDEN_SSID_NOT_IN_USE);
    m_u32(&m, NL80211_ATTR_AUTH_TYPE, NL80211_AUTHTYPE_OPEN_SYSTEM);
    m_u32(&m, NL80211_ATTR_WIPHY_FREQ, (uint32_t)(2407 + 5 * channel));
    m_u32(&m, NL80211_ATTR_WIPHY_CHANNEL_TYPE, NL80211_CHAN_NO_HT);
    r = nl_call(g_cmd, &m, 0, NULL);
    if (r) { set_err("start AP", r); goto fail; }

    g_stop = 0;
    if (pthread_create(&g_tid, NULL, ev_thread, NULL) != 0) { set_err("thread", EAGAIN); r = -EAGAIN; goto fail_stop; }
    g_running = 1;
    set_err("AP started", 0);
    return 0;

fail_stop:
    m_init(&m, g_fam, NL80211_CMD_STOP_AP);
    m_u32(&m, NL80211_ATTR_IFINDEX, (uint32_t)g_ifindex);
    nl_call(g_cmd, &m, 0, NULL);
fail:
    if_updown(0);
    set_iftype(NL80211_IFTYPE_STATION);
    if_updown(1);
    close_socks();
    return r;
}

void apmode_stop(void)
{
    if (!g_running) return;
    g_stop = 1;
    pthread_join(g_tid, NULL);
    pthread_mutex_lock(&g_lock);
    for (int i = 0; i < MAX_STA; i++) {
        if (!g_sta[i].used) continue;
        uint8_t out[64];
        size_t ol = ap_build_deauth(&g_cfg, g_sta[i].mac, 3, out, sizeof(out));
        if (ol) send_frame(out, ol);
        del_station(g_sta[i].mac);
        g_sta[i].used = 0;
    }
    pthread_mutex_unlock(&g_lock);
    msg_t m;
    m_init(&m, g_fam, NL80211_CMD_STOP_AP);
    m_u32(&m, NL80211_ATTR_IFINDEX, (uint32_t)g_ifindex);
    nl_call(g_cmd, &m, 0, NULL);
    if_updown(0);
    set_iftype(NL80211_IFTYPE_STATION);
    if_updown(1);
    close_socks();
    g_running = 0;
    set_err("AP stopped", 0);
}

int apmode_running(void) { return g_running; }

int apmode_station_count(void)
{
    int n = 0;
    pthread_mutex_lock(&g_lock);
    for (int i = 0; i < MAX_STA; i++) n += g_sta[i].used;
    pthread_mutex_unlock(&g_lock);
    return n;
}

const char *apmode_last_error(void) { return g_err; }

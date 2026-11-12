#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <stdint.h>
#include <time.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <poll.h>
#include <pthread.h>
#include <sys/time.h>
#include "tls.h"
#include "control.h"
#include "speaker.h"
#include "substream.h"
#include "web.h"
#include <arpa/inet.h>
#include <sys/random.h>
#define SUB_FRAME_MAX (128 * 1024)
#include "monotime.h"

/* ---- audio codec for RTSP: G.711 u-law (PCMU, payload type 0) by default,
 * which go2rtc/WebRTC and most players take as-is; L16 (raw 16-bit PCM)
 * as before. Applies to sessions started after the change. */
static volatile int g_audio_pcmu = 1;
static int audio_codec_get(char *o, size_t c) { snprintf(o, c, "%s", g_audio_pcmu ? "pcmu" : "l16"); return 0; }
static int audio_codec_set(const char *v, const char **e)
{
    if (!strcmp(v, "pcmu")) g_audio_pcmu = 1;
    else if (!strcmp(v, "l16")) g_audio_pcmu = 0;
    else { *e = "must be pcmu or l16"; return -1; }
    return 0;
}

void rtsp_register_params(void)
{
    ctrl_param_t p = { "audio.codec", audio_codec_get, audio_codec_set, CTRL_PERSIST };
    control_register(&p);
}

/* ITU-T G.711 decoders (8-bit -> 16-bit linear) */
static int16_t ulaw_to_linear(uint8_t u)
{
    u = (uint8_t)~u;
    int t = ((u & 0x0f) << 3) + 0x84;
    t <<= (u & 0x70) >> 4;
    return (int16_t)((u & 0x80) ? (0x84 - t) : (t - 0x84));
}
static int16_t alaw_to_linear(uint8_t a)
{
    a ^= 0x55;
    int t = (a & 0x0f) << 4, seg = (a & 0x70) >> 4;
    if (seg == 0) t += 8; else if (seg == 1) t += 0x108; else { t += 0x108; t <<= seg - 1; }
    return (int16_t)((a & 0x80) ? t : -t);
}

/* ITU-T G.711 u-law encoder (16-bit linear -> 8-bit) */
static uint8_t linear_to_ulaw(int16_t pcm)
{
    const int BIAS = 0x84, CLIP = 32635;
    int sign = (pcm >> 8) & 0x80, s = pcm;
    if (sign) s = -s;
    if (s > CLIP) s = CLIP;
    s += BIAS;
    int exp = 7;
    for (int mask = 0x4000; !(s & mask) && exp > 0; mask >>= 1) exp--;
    int mant = (s >> (exp + 3)) & 0x0f;
    return (uint8_t)~(sign | (exp << 4) | mant);
}
#include <stdbool.h>
#include <smolrtsp.h>
#include <smolrtsp/types/rtcp.h>
#include <interface99.h>
#include <datatype99.h>

#include "ak_rtsp.h"
#include "ak_ioctls.h"
#include "audio.h"
#include "rtsp.h"
#include "venc.h"
#include "ae.h"
#include "auth.h"
#include "live.h"
#include "conn.h"
#include <stdarg.h>
#include <strings.h>

/* -------------------------------------------------------------------------
 * Client connection context conforming to SmolRTSP_Controller interface
 * ------------------------------------------------------------------------- */
typedef struct {
    int socket_fd;
    bool tls;            /* RTSPS: only interleaved (TCP) transport, inside TLS */
    struct sockaddr_storage addr;
    socklen_t addr_len;

    uint64_t session_id;
    bool is_playing;

    bool is_tcp;
    uint8_t rtp_channel;
    uint8_t rtcp_channel;

    int rtp_udp_fd;
    int rtcp_udp_fd;
    uint16_t client_rtp_port;
    uint16_t client_rtcp_port;

    SmolRTSP_RtpTransport *rtp_transport;
    SmolRTSP_NalTransport *nal_transport;

    /* Audio track (streamid=1). Plain RTP, no NAL wrapping — payload is raw
     * L16 samples. NULL until SETUP is called for the audio URI; audio
     * frames are only sent once this is non-NULL. Separate UDP fds from the
     * video track since each track gets its own client_port pair. */
    SmolRTSP_RtpTransport *audio_rtp_transport;
    int audio_rtp_udp_fd;
    int audio_rtcp_udp_fd;

    /* time(NULL) value of the next scheduled RTCP Sender Report — see
     * send_rtcp_reports() below. 0 means "not yet scheduled". */
    time_t next_rtcp_sr_sec;

    /* Media from the live hub (live.c): this client's own cursor, so
     * several clients can play at once without slowing each other. */
    bool          subscribed;
    bool          pcmu;        /* audio codec chosen at DESCRIBE */
    bool          sub;         /* rtsp://camera/sub: 640x360 second stream (video.sub) */
    uint32_t      sub_cur;
    /* ONVIF audio backchannel (talk from go2rtc / Home Assistant / NVRs):
     * the client sends G.711 RTP on this interleaved channel (-1 = none). */
    bool          bc_offered;
    int           bc_channel;
    int64_t       bc_last_ms;
    live_cursor_t cur;
    uint32_t      audio_rtp_ts;

    /* Digest auth state (see Client_before). */
    bool   authed;
    int    auth_failures;
    time_t connected_at;
} Client;

declImpl(SmolRTSP_Controller, Client);

static void Client_drop(VSelf) {
    VSELF(Client);
    if (self->nal_transport) {
        VTABLE(SmolRTSP_NalTransport, SmolRTSP_Droppable).drop(self->nal_transport);
    } else if (self->rtp_transport) {
        VTABLE(SmolRTSP_RtpTransport, SmolRTSP_Droppable).drop(self->rtp_transport);
    }
    if (self->rtp_udp_fd >= 0) close(self->rtp_udp_fd);
    if (self->rtcp_udp_fd >= 0) close(self->rtcp_udp_fd);

    if (self->audio_rtp_transport)
        VTABLE(SmolRTSP_RtpTransport, SmolRTSP_Droppable).drop(self->audio_rtp_transport);
    if (self->audio_rtp_udp_fd >= 0) close(self->audio_rtp_udp_fd);
    if (self->audio_rtcp_udp_fd >= 0) close(self->audio_rtcp_udp_fd);
}

impl(SmolRTSP_Droppable, Client);

/* Generic per-track transport setup (used for both the video and audio
 * tracks — see setup_video_transport()/setup_audio_transport() below).
 * Creates the RtpTransport and its UDP fds (for UDP) but does NOT wrap it
 * in a NalTransport — that's video-specific and done by the caller. */
static int setup_rtp_transport(Client *self, SmolRTSP_Context *ctx, const SmolRTSP_Request *req,
                                uint8_t payload_ty, uint32_t clock_rate,
                                SmolRTSP_RtpTransport **out_transport,
                                int *out_rtp_fd, int *out_rtcp_fd) {
    CharSlice99 transport_val;
    if (!SmolRTSP_HeaderMap_find(&req->header_map, SMOLRTSP_HEADER_TRANSPORT, &transport_val)) {
        smolrtsp_respond(ctx, SMOLRTSP_STATUS_BAD_REQUEST, "Transport not present");
        return -1;
    }

    SmolRTSP_TransportConfig config;
    if (smolrtsp_parse_transport(&config, transport_val) == -1) {
        smolrtsp_respond(ctx, SMOLRTSP_STATUS_BAD_REQUEST, "Malformed Transport");
        return -1;
    }

    if (self->tls && config.lower != SmolRTSP_LowerTransport_TCP) {
        /* RTP over UDP would leave the TLS session unencrypted */
        smolrtsp_respond(ctx, SMOLRTSP_STATUS_UNSUPPORTED_TRANSPORT,
                         "RTSPS supports RTP/AVP/TCP (interleaved) only");
        return -1;
    }

    if (config.lower == SmolRTSP_LowerTransport_TCP) {
        ifLet(config.interleaved, SmolRTSP_ChannelPair_Some, interleaved) {
            self->is_tcp = true;
            self->rtp_channel = interleaved->rtp_channel;
            self->rtcp_channel = interleaved->rtcp_channel;

            printf("[ak_rtsp] Transport: TCP interleaved (RTP ch %d, RTCP ch %d)\n",
                   (int)interleaved->rtp_channel, (int)interleaved->rtcp_channel);

            SmolRTSP_Transport t = smolrtsp_transport_tcp(
                SmolRTSP_Context_get_writer(ctx), interleaved->rtp_channel, 0);

            *out_transport = SmolRTSP_RtpTransport_new(t, payload_ty, clock_rate);

            smolrtsp_header(ctx, SMOLRTSP_HEADER_TRANSPORT,
                "RTP/AVP/TCP;unicast;interleaved=%d-%d",
                (int)interleaved->rtp_channel, (int)interleaved->rtcp_channel);
            return 0;
        }
        smolrtsp_respond(ctx, SMOLRTSP_STATUS_BAD_REQUEST, "interleaved not found");
        return -1;
    } else {
        ifLet(config.client_port, SmolRTSP_PortPair_Some, client_port) {
            self->is_tcp = false;
            self->client_rtp_port = client_port->rtp_port;
            self->client_rtcp_port = client_port->rtcp_port;

            char ip_str[64] = "unknown";
            if (self->addr.ss_family == AF_INET) {
                inet_ntop(AF_INET, &((struct sockaddr_in *)&self->addr)->sin_addr,
                          ip_str, sizeof(ip_str));
            }
            printf("[ak_rtsp] Transport: UDP client %s RTP:%u RTCP:%u\n",
                   ip_str, client_port->rtp_port, client_port->rtcp_port);

            void *ip = smolrtsp_sockaddr_ip((struct sockaddr *)&self->addr);
            *out_rtp_fd = smolrtsp_dgram_socket(self->addr.ss_family, ip,
                                                 client_port->rtp_port);
            if (*out_rtp_fd < 0) {
                printf("[ak_rtsp] Failed to create UDP RTP socket\n");
                smolrtsp_respond_internal_error(ctx);
                return -1;
            }

            *out_rtcp_fd = smolrtsp_dgram_socket(self->addr.ss_family, ip,
                                                  client_port->rtcp_port);

            struct sockaddr_storage local_addr;
            socklen_t local_len = sizeof(local_addr);
            uint16_t local_rtp_port = 0, local_rtcp_port = 0;

            if (getsockname(*out_rtp_fd, (struct sockaddr *)&local_addr, &local_len) == 0)
                local_rtp_port = ntohs(((struct sockaddr_in *)&local_addr)->sin_port);
            if (*out_rtcp_fd >= 0 &&
                getsockname(*out_rtcp_fd, (struct sockaddr *)&local_addr, &local_len) == 0)
                local_rtcp_port = ntohs(((struct sockaddr_in *)&local_addr)->sin_port);

            printf("[ak_rtsp] Local UDP: RTP:%u RTCP:%u\n", local_rtp_port, local_rtcp_port);

            SmolRTSP_Transport t = smolrtsp_transport_udp(*out_rtp_fd);
            *out_transport = SmolRTSP_RtpTransport_new(t, payload_ty, clock_rate);

            smolrtsp_header(ctx, SMOLRTSP_HEADER_TRANSPORT,
                "RTP/AVP/UDP;unicast;client_port=%u-%u;server_port=%u-%u",
                client_port->rtp_port, client_port->rtcp_port,
                local_rtp_port, local_rtcp_port);
            return 0;
        }
        smolrtsp_respond(ctx, SMOLRTSP_STATUS_BAD_REQUEST, "client_port not found");
        return -1;
    }
}

static int setup_video_transport(Client *self, SmolRTSP_Context *ctx, const SmolRTSP_Request *req) {
    if (setup_rtp_transport(self, ctx, req, 96, 90000,
                             &self->rtp_transport, &self->rtp_udp_fd, &self->rtcp_udp_fd) < 0)
        return -1;
    self->nal_transport = SmolRTSP_NalTransport_new(self->rtp_transport);
    return 0;
}

static int setup_audio_transport(Client *self, SmolRTSP_Context *ctx, const SmolRTSP_Request *req) {
    return setup_rtp_transport(self, ctx, req, self->pcmu ? 0 : 97, AUDIO_SAMPLE_RATE,
                                &self->audio_rtp_transport,
                                &self->audio_rtp_udp_fd, &self->audio_rtcp_udp_fd);
}

static bool find_header_ci(const SmolRTSP_Request *req, const char *name, CharSlice99 *out);

/* unpredictable RTSP session id (kernel RNG; never 0) */
static uint64_t session_id_new(void)
{
    uint64_t v = 0;
    if (getrandom(&v, sizeof v, 0) != (ssize_t)sizeof v) v = (uint64_t)rand() << 32 ^ (uint64_t)rand() ^ (uint64_t)mono_ms();
    v &= 0x7fffffffffffffffull;               /* printed with %llu by some clients as signed */
    return v ? v : 1;
}

static void Client_options(VSelf, SmolRTSP_Context *ctx, const SmolRTSP_Request *req) {
    VSELF(Client);
    (void)self; (void)req;
    smolrtsp_header(ctx, SMOLRTSP_HEADER_PUBLIC, "DESCRIBE, SETUP, PLAY, TEARDOWN");
    smolrtsp_respond_ok(ctx);
}

static void Client_describe(VSelf, SmolRTSP_Context *ctx, const SmolRTSP_Request *req) {
    VSELF(Client);
    char sdp_buf[1024], audio_fmt[96], bc[160] = "";
    CharSlice99 require;
    self->bc_offered = find_header_ci(req, "Require", &require) &&
                       memmem(require.ptr, require.len, "www.onvif.org/ver20/backchannel", 31) != NULL;
    if (self->bc_offered)        /* a=sendonly: the client sends, the camera plays it */
        snprintf(bc, sizeof(bc), "m=audio 0 RTP/AVP 0 8\r\nc=IN IP4 0.0.0.0\r\na=rtpmap:0 PCMU/8000\r\na=rtpmap:8 PCMA/8000\r\n"
                                 "a=control:streamid=2\r\na=sendonly\r\n");
    {   /* path /sub (or ONVIF profile token "sub") selects the second stream */
        const char *u = CharSlice99_alloca_c_str(req->start_line.uri);
        const char *p = strstr(u, "://");
        p = p ? strchr(p + 3, '/') : u;
        self->sub = p && (!strncmp(p, "/sub", 4) && (p[4] == 0 || p[4] == '/' || p[4] == '?'));
        if (self->sub && !substream_running()) {
            smolrtsp_respond(ctx, SMOLRTSP_STATUS_NOT_FOUND, "second stream is off (video.sub)");
            return;
        }
        self->sub_cur = 0;
    }
    self->pcmu = g_audio_pcmu;
    if (self->pcmu) snprintf(audio_fmt, sizeof(audio_fmt), "m=audio 0 RTP/AVP 0\r\nc=IN IP4 0.0.0.0\r\na=rtpmap:0 PCMU/8000\r\n");
    else snprintf(audio_fmt, sizeof(audio_fmt), "m=audio 0 RTP/AVP 97\r\nc=IN IP4 0.0.0.0\r\na=rtpmap:97 L16/%d/%d\r\n",
                  AUDIO_SAMPLE_RATE, AUDIO_CHANNELS);
    /* Audio track uses payload type 97 with L16 (linear PCM): anyka_ipc's own
     * capture stream is raw 8kHz/mono/16-bit, not G.711, so RFC 3551 L16
     * needs no fmtp at all. PT 97 (dynamic) matches the convention anyka_ipc's
     * own RTSP SDP builder uses for all of its audio codec options ("Session 3"
     * correction), even though it's not required for L16 specifically. */
    snprintf(sdp_buf, sizeof(sdp_buf),
        "v=0\r\n"
        "o=- %llu %llu IN IP4 0.0.0.0\r\n"
        "s=ak_rtsp\r\n"
        "t=0 0\r\n"
        "m=video 0 RTP/AVP 96\r\n"
        "c=IN IP4 0.0.0.0\r\n"
        "a=rtpmap:96 H264/90000\r\n"
        "a=fmtp:96 packetization-mode=1\r\n"
        "a=control:streamid=0\r\n"
        "%s"
        "a=control:streamid=1\r\n"
        "%s",
        (unsigned long long)time(NULL), (unsigned long long)time(NULL), audio_fmt, bc);

    smolrtsp_header(ctx, SMOLRTSP_HEADER_CONTENT_TYPE, "application/sdp");
    smolrtsp_body(ctx, CharSlice99_from_str(sdp_buf));
    smolrtsp_respond_ok(ctx);
}

static void Client_setup(VSelf, SmolRTSP_Context *ctx, const SmolRTSP_Request *req) {
    VSELF(Client);

    /* Which track this SETUP is for is carried in the request URI, matching
     * the "a=control:streamid=N" values handed out in Client_describe above
     * (clients build the SETUP URI as <base>/<control-value>). */
    const char *uri = CharSlice99_alloca_c_str(req->start_line.uri);
    bool is_audio = strstr(uri, "streamid=1") != NULL;

    if (strstr(uri, "streamid=2")) {             /* ONVIF audio backchannel: interleaved TCP only */
        CharSlice99 tv;
        SmolRTSP_TransportConfig tc;
        if (!SmolRTSP_HeaderMap_find(&req->header_map, SMOLRTSP_HEADER_TRANSPORT, &tv) ||
            smolrtsp_parse_transport(&tc, tv) == -1 || tc.lower != SmolRTSP_LowerTransport_TCP) {
            smolrtsp_respond(ctx, SMOLRTSP_STATUS_UNSUPPORTED_TRANSPORT, "backchannel needs RTP/AVP/TCP");
            return;
        }
        int ch = -1;
        ifLet(tc.interleaved, SmolRTSP_ChannelPair_Some, il) { ch = il->rtp_channel; }
        if (ch < 0) { smolrtsp_respond(ctx, SMOLRTSP_STATUS_BAD_REQUEST, "interleaved not found"); return; }
        self->bc_channel = ch;
        smolrtsp_header(ctx, SMOLRTSP_HEADER_TRANSPORT, "RTP/AVP/TCP;unicast;interleaved=%d-%d", ch, ch + 1);
        printf("[ak_rtsp] backchannel: audio from the client on interleaved channel %d\n", ch);
        if (self->session_id == 0) self->session_id = session_id_new();
        smolrtsp_header(ctx, SMOLRTSP_HEADER_SESSION, "%llu", (unsigned long long)self->session_id);
        smolrtsp_respond_ok(ctx);
        return;
    }

    if (is_audio ? self->audio_rtp_transport != NULL : self->rtp_transport != NULL) {
        smolrtsp_respond(ctx, SMOLRTSP_STATUS_METHOD_NOT_VALID_IN_THIS_STATE, "track already set up");
        return;
    }
    if (is_audio) {
        if (setup_audio_transport(self, ctx, req) < 0) return;
    } else {
        if (setup_video_transport(self, ctx, req) < 0) return;
    }

    /* Session ID must stay the same across both tracks' SETUP calls within
     * one session — only generate it once. */
    if (self->session_id == 0) self->session_id = session_id_new();
    smolrtsp_header(ctx, SMOLRTSP_HEADER_SESSION, "%llu", (unsigned long long)self->session_id);
    smolrtsp_respond_ok(ctx);
}

static void Client_play(VSelf, SmolRTSP_Context *ctx, const SmolRTSP_Request *req) {
    VSELF(Client);
    (void)req;
    self->is_playing = true;
    if (!self->subscribed) { live_subscribe(); self->subscribed = true; }
    smolrtsp_header(ctx, SMOLRTSP_HEADER_RANGE, "npt=now-");
    smolrtsp_respond_ok(ctx);
}

static void Client_teardown(VSelf, SmolRTSP_Context *ctx, const SmolRTSP_Request *req) {
    VSELF(Client);
    (void)req;
    self->is_playing = false;
    smolrtsp_respond_ok(ctx);
}

static void Client_unknown(VSelf, SmolRTSP_Context *ctx, const SmolRTSP_Request *req) {
    VSELF(Client);
    (void)self; (void)req;
    smolrtsp_respond(ctx, SMOLRTSP_STATUS_METHOD_NOT_ALLOWED, "Method Not Allowed");
}

/* Case-insensitive header lookup (SmolRTSP_HeaderMap_find is case-sensitive). */
static bool find_header_ci(const SmolRTSP_Request *req, const char *name, CharSlice99 *out) {
    size_t nlen = strlen(name);
    for (size_t i = 0; i < req->header_map.len; i++) {
        CharSlice99 k = req->header_map.headers[i].key;
        if (k.len == nlen && strncasecmp(k.ptr, name, nlen) == 0) {
            *out = req->header_map.headers[i].value;
            return true;
        }
    }
    return false;
}

static SmolRTSP_ControlFlow Client_before(VSelf, SmolRTSP_Context *ctx,
                                           const SmolRTSP_Request *req) {
    VSELF(Client);
    const char *method = CharSlice99_alloca_c_str(req->start_line.method);
    printf("[ak_rtsp] %s %s CSeq=%u\n", method,
           CharSlice99_alloca_c_str(req->start_line.uri), req->cseq);

    /* OPTIONS stays open (capability probe, reveals nothing). Everything
     * else needs Digest credentials. Once a client has authenticated on
     * this connection, later requests still carry Authorization and are
     * re-verified — standard client behaviour, cheap to check. */
    SmolRTSP_Method options = SMOLRTSP_METHOD_OPTIONS;
    if (SmolRTSP_Method_eq(&req->start_line.method, &options))
        return SmolRTSP_ControlFlow_Continue;

    int verdict = 0;
    CharSlice99 authz;
    int has_authz = find_header_ci(req, "Authorization", &authz);
    if (has_authz && auth_throttled((const struct sockaddr *)&self->addr)) {
        smolrtsp_respond(ctx, SMOLRTSP_STATUS_FORBIDDEN, "too many failed logins, try later");
        self->auth_failures = 5;                   /* drop the connection */
        return SmolRTSP_ControlFlow_Break;
    }
    if (has_authz && authz.len < 1024) {
        char hdr[1024];
        memcpy(hdr, authz.ptr, authz.len);
        hdr[authz.len] = '\0';
        verdict = auth_digest_verify(hdr, method);
    }
    if (has_authz && verdict != -1) auth_record((const struct sockaddr *)&self->addr, verdict == 1);
    if (verdict == 1) {
        self->authed = true;
        return SmolRTSP_ControlFlow_Continue;
    }

    char chal[256];
    auth_digest_challenge(chal, sizeof(chal), 0, verdict == -1);
    smolrtsp_header(ctx, SMOLRTSP_HEADER_WWW_AUTHENTICATE, "%s", chal);
    smolrtsp_respond(ctx, SMOLRTSP_STATUS_UNAUTHORIZED, "Unauthorized");
    if (find_header_ci(req, "Authorization", &authz))
        self->auth_failures++;
    printf("[ak_rtsp] 401 for %s (%s)\n", method,
           verdict == -1 ? "stale nonce" : "no/bad credentials");
    return SmolRTSP_ControlFlow_Break;
}

static void Client_after(VSelf, ssize_t ret, SmolRTSP_Context *ctx,
                          const SmolRTSP_Request *req) {
    VSELF(Client);
    (void)self; (void)ctx; (void)req;
    if (ret < 0) printf("[ak_rtsp] Failed to respond to client\n");
}

impl(SmolRTSP_Controller, Client);

/* -------------------------------------------------------------------------
 * H.264 frame packetization
 *
 * Walks Annex-B start codes, wraps each NALU in SmolRTSP_NalUnit, sets the
 * marker bit on the last VCL NALU of each access unit, and hands it to
 * SmolRTSP_NalTransport_send_packet for FU-A fragmentation if needed.
 * Timestamps: VENC gives milliseconds → multiply by 90 for 90 kHz RTP clock.
 * ------------------------------------------------------------------------- */
static int send_frame_nalus(Client *client, uint8_t *data, size_t len,
                             uint32_t timestamp)
{
    int failed = 0;
    size_t i = 0;
    while (i + 3 < len) {
        size_t start_code_len = 0;
        if (data[i] == 0x00 && data[i+1] == 0x00 &&
            data[i+2] == 0x00 && data[i+3] == 0x01)
            start_code_len = 4;
        else if (data[i] == 0x00 && data[i+1] == 0x00 && data[i+2] == 0x01)
            start_code_len = 3;

        if (start_code_len > 0) {
            size_t nalu_start = i + start_code_len;
            size_t next_nalu_start = len;
            size_t j = nalu_start;
            while (j + 2 < len) {
                if (data[j] == 0x00 && data[j+1] == 0x00 && data[j+2] == 0x01) {
                    next_nalu_start = (j > 0 && data[j-1] == 0x00) ? j - 1 : j;
                    break;
                }
                j++;
            }

            size_t nalu_len = next_nalu_start - nalu_start;
            if (nalu_len > 0) {
                uint8_t nalu_type = data[nalu_start] & 0x1F;
                SmolRTSP_H264NalHeader h264_hdr =
                    SmolRTSP_H264NalHeader_parse(data[nalu_start]);
                SmolRTSP_NalUnit nalu = {
                    .header  = SmolRTSP_NalHeader_H264(h264_hdr),
                    .payload = U8Slice99_new(data + nalu_start + 1, nalu_len - 1),
                };
                bool is_vcl = (nalu_type >= 1 && nalu_type <= 5);
                if (SmolRTSP_NalTransport_send_packet(
                        client->nal_transport,
                        SmolRTSP_RtpTimestamp_Raw(timestamp),
                        is_vcl, nalu) == -1) {
                    if (!failed) perror("send RTP/NAL");
                    failed = 1;
                    if (client->is_tcp) return -1;
                }
            }
            i = next_nalu_start;
        } else {
            i++;
        }
    }
    return failed ? -1 : 0;
}

/* Encoder output: the main loop calls this every ~20 ms. Every frame goes
 * to the live hub (live.c), from which all RTSP/RTSPS clients and web
 * viewers read with their own cursors; with nobody subscribed the frame is
 * just released, so the ring never wraps. */
static volatile int g_kbps_measured;
int rtsp_measured_kbps(void) { return g_kbps_measured; }

static void frame_stats(const struct venc_stream_req *req, uint64_t ms_ts)
{
    static int frame_count = 0;
    /* Always log I-frames while someone watches; a heartbeat every ~10 s */
    if ((req->frame_type_raw == FRAME_TYPE_I && rtsp_client_connected()) || frame_count % 150 == 0)
        printf("[ak_rtsp] frame#%d type=%s len=%d ts=%u clients=%d\n", frame_count,
               req->frame_type_raw == FRAME_TYPE_I ? "I" : req->frame_type_raw == FRAME_TYPE_P ? "P" : "B",
               req->len, (uint32_t)(ms_ts * 90), rtsp_client_connected());
    frame_count++;

    /* Active bitrate over 2 full GOPs (I-frames are much larger than
     * P-frames on a static scene; a shorter window swings wildly). */
    static uint64_t win_start = 0, win_bytes = 0;
    static int win_frames = 0;
    const uint64_t win_ms = (uint64_t)(ENC_GOP * 2) * 1000 / ENC_FPS;
    if (win_start == 0) win_start = ms_ts;
    win_bytes += (uint64_t)req->len;
    win_frames++;
    if (ms_ts - win_start >= win_ms) {
        g_kbps_measured = (int)((double)(win_bytes * 8) / (double)(ms_ts - win_start));
        if (rtsp_client_connected())
            printf("[ak_rtsp] bitrate: %.0f kbps (%d frames over %llums)\n",
                   (double)(win_bytes * 8) / (double)(ms_ts - win_start), win_frames,
                   (unsigned long long)(ms_ts - win_start));
        win_start = ms_ts; win_bytes = 0; win_frames = 0;
    }
}

static int started_count_dummy_;
void drain_venc_ring(void)
{
    for (int i = 0; i < 8; i++) {
        struct venc_stream_req req;
        memset(&req, 0, sizeof(req));
        req.chn_id = 0;
        req.len    = venc_max_frame_size(ENC_WIDTH, ENC_HEIGHT);
        if (ioctl(g_chn0_fd, VENC_IOCTL_GET_STREAM, &req) != 0) return;   /* nothing (more) yet */
        uint32_t off = (uint32_t)req.frame_offset - g_venc_phys;
        if (off >= ENC_VENC_SIZE || req.len <= 0 || (uint32_t)req.len > ENC_VENC_SIZE - off) {
            ioctl(g_chn0_fd, VENC_IOCTL_RELEASE_STREAM, &req);
            continue;
        }
        void *data = (char *)g_venc_virt + off;
        uint64_t ms_ts = (uint64_t)req.ts_hi << 32 | (uint64_t)(uint32_t)req.ts_lo;
        frame_stats(&req, ms_ts);
        static int started;
        if (!started && ++started_count_dummy_ >= 150) { started = 1; venc_startup_ok(); ae_tnr_confirm(); }
        live_push_frame((const uint8_t *)data, (size_t)req.len, req.frame_type_raw == FRAME_TYPE_I, ms_ts);
        ioctl(g_chn0_fd, VENC_IOCTL_RELEASE_STREAM, &req);
    }
}

/* -------------------------------------------------------------------------
 * Audio: drain whatever periods audio.c's capture thread has ready and send
 * them as plain RTP (no NAL framing — this isn't H.264).
 *
 * Timestamp: a running sample counter (RFC 3551's intended scheme for a
 * fixed-rate PCM stream like L16), NOT wall-clock time. Originally used
 * SmolRTSP_RtpTimestamp_SysClockUs(clock_gettime(CLOCK_MONOTONIC)), which
 * hardware-confirmed broke badly: CLOCK_MONOTONIC is time-since-boot, while
 * the video track's timestamp (rtsp.c's send_video_frame, ms_ts*90) comes
 * from the VENC hardware encoder's own internal clock, which resets near
 * zero when the encoder channel initializes — a totally different epoch.
 * Whatever gap exists between "camera booted" and "ak_rtsp started the VENC
 * channel" (30-40s observed) showed up directly as a matching offset
 * between the two tracks' first timestamps, which broke VLC's initial A/V
 * sync badly (~34s stream-buffering stall, then large "buffer too late"
 * drops). A monotonically-incrementing sample count has no wall-clock
 * dependency at all and matches the actual data rate exactly, avoiding this
 * whole class of bug. Starts from a random 32-bit value per RFC 3550 §5.1
 * (receivers must not assume a stream's initial timestamp is 0). RFC 3551
 * L16 payload must be network (big-endian) byte order; our samples are
 * native (little-endian on ARM), so each sample is byte-swapped before
 * sending. */
static void send_audio_frame(Client *client, const live_item_t *it)
{
    if (!client->audio_rtp_transport || !it->n) return;

    uint16_t net_pcm[LIVE_APER];
    uint8_t *ulaw = (uint8_t *)net_pcm;
    size_t n_samples = it->n > LIVE_APER ? LIVE_APER : it->n;
    if (client->pcmu)
        for (size_t i = 0; i < n_samples; i++) ulaw[i] = linear_to_ulaw(it->pcm[i]);
    else
        for (size_t i = 0; i < n_samples; i++) net_pcm[i] = htons((uint16_t)it->pcm[i]);

    if (client->audio_rtp_ts == 0) client->audio_rtp_ts = (uint32_t)rand() | 1;   /* per session, RFC 3550 */

    U8Slice99 payload = U8Slice99_new((uint8_t *)net_pcm, n_samples * (client->pcmu ? 1 : sizeof(uint16_t)));
    if (SmolRTSP_RtpTransport_send_packet(
            client->audio_rtp_transport, SmolRTSP_RtpTimestamp_Raw(client->audio_rtp_ts),
            true, U8Slice99_empty(), payload) == -1)
        perror("send RTP/audio");

    client->audio_rtp_ts += (uint32_t)n_samples;
}

/* -------------------------------------------------------------------------
 * RTCP Sender Reports.
 *
 * Added to fix a VLC(/live555)-specific bug: without ANY RTCP SR at all
 * (this server previously opened RTCP sockets but never sent anything on
 * them), VLC's audio track eventually hit a hard "Timestamp conversion
 * failed" error and dropped audio permanently for the rest of the session
 * — video was unaffected. ffplay played the identical stream with no
 * dropout, confirming the wire data itself was correct and the receiver
 * just had no RTP-timestamp-to-wallclock mapping to fall back on once its
 * own internal clock model drifted.
 * An SR gives a receiver exactly that mapping (an (NTP time, RTP
 * timestamp) pair) so it can correct for drift indefinitely instead of
 * eventually giving up. UDP-transport only for now — TCP-interleaved RTCP
 * would need to write framed bytes over the RTSP TCP connection instead of
 * a separate socket; not implemented since this project only exercises UDP
 * transport in practice. */
#define RTCP_SR_INTERVAL_SEC 5

static void send_one_rtcp_sr(SmolRTSP_RtpTransport *transport, int rtcp_fd) {
    if (!transport || rtcp_fd < 0) return;
    /* An SR before the first RTP packet would carry RTP timestamp 0 and make
     * receivers (ffmpeg) anchor the track's clock there: every following
     * packet then looks ~2^32 ticks off (DTS discontinuity, broken durations). */
    if (SmolRTSP_RtpTransport_pkt_count(transport) == 0) return;

    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    /* Unix epoch (1970) -> NTP epoch (1900): RFC 3550 §4. */
    uint32_t ntp_sec  = (uint32_t)ts.tv_sec + 2208988800u;
    uint32_t ntp_frac = (uint32_t)(((uint64_t)ts.tv_nsec << 32) / 1000000000ull);

    SmolRTSP_RtcpSr sr = {
        .padding     = false,
        .rc          = 0,
        .ssrc        = SmolRTSP_RtpTransport_ssrc(transport),
        .ntp_sec     = htonl(ntp_sec),
        .ntp_frac    = htonl(ntp_frac),
        .rtp_ts      = htonl(SmolRTSP_RtpTransport_last_rtp_ts(transport)),
        .pkt_count   = htonl(SmolRTSP_RtpTransport_pkt_count(transport)),
        .octet_count = htonl(SmolRTSP_RtpTransport_octet_count(transport)),
    };

    uint8_t buf[SMOLRTSP_RTCP_SR_SIZE_BASE];
    if (write(rtcp_fd, SmolRTSP_RtcpSr_serialize(sr, buf), sizeof(buf)) < 0)
        perror("send RTCP SR");
}

/* Sends an SR for every track that's been SETUP, at most once every
 * RTCP_SR_INTERVAL_SEC seconds. Called once per client-loop iteration —
 * cheap to call every time since it no-ops between intervals. */
static void send_rtcp_reports(Client *client) {
    time_t now = time(NULL);
    if (now < client->next_rtcp_sr_sec) return;
    if (!client->rtp_transport || SmolRTSP_RtpTransport_pkt_count(client->rtp_transport) == 0)
        return;                                   /* nothing sent yet: no timestamps to report */
    if (client->next_rtcp_sr_sec == 0) {
        /* A session starts with the frames since the last keyframe, sent as a
         * burst. An SR now would pair "now" with the timestamp of a frame up to
         * a GOP old, and receivers would shift the clock back (DTS going
         * backwards). Wait until the backlog is gone. */
        client->next_rtcp_sr_sec = now + 3;
        return;
    }
    client->next_rtcp_sr_sec = now + RTCP_SR_INTERVAL_SEC;

    send_one_rtcp_sr(client->rtp_transport, client->rtcp_udp_fd);
    send_one_rtcp_sr(client->audio_rtp_transport, client->audio_rtcp_udp_fd);
}

/* -------------------------------------------------------------------------
 * Client connection loop
 *
 * Uses poll() for incoming RTSP commands; the client socket remains blocking for writes:
 *   timeout = 0   when is_playing (check for commands, then immediately send)
 *   timeout = 5000 ms when idle (block until a request arrives)
 * ------------------------------------------------------------------------- */
/* One RTP packet of backchannel audio: G.711 (PT 0 u-law / 8 A-law) into the
 * speaker's jitter buffer, same path as talk-back from the web page. */
static void backchannel_rtp(Client *c, const uint8_t *p, size_t n)
{
    if ((p[0] >> 6) != 2) return;
    size_t h = 12 + (size_t)(p[0] & 0x0f) * 4;
    if (p[0] & 0x10) {                            /* header extension */
        if (n < h + 4) return;
        h += 4 + (size_t)(p[h + 2] << 8 | p[h + 3]) * 4;
    }
    if (n <= h) return;
    if (p[0] & 0x20) {                            /* padding: count must fit the payload */
        size_t pad = p[n - 1];
        if (pad == 0 || pad > n - h) return;
        n -= pad;
    }
    if (n <= h) return;
    int pt = p[1] & 0x7f;
    if (pt != 0 && pt != 8) return;
    int16_t pcm[1500];
    size_t k = n - h > 1500 ? 1500 : n - h;
    for (size_t i = 0; i < k; i++) pcm[i] = pt == 0 ? ulaw_to_linear(p[h + i]) : alaw_to_linear(p[h + i]);
    int64_t now = mono_ms();
    if (!c->bc_last_ms || now - c->bc_last_ms > 2000) printf("[ak_rtsp] backchannel audio started (%s)\n", pt ? "PCMA" : "PCMU");
    c->bc_last_ms = now;
    if (speaker_ready()) speaker_queue_stream(pcm, k);
}

/* SmolRTSP_Writer over a conn_t (plain TCP or TLS). Responses and the
 * interleaved RTP packets of a session both go through it; one thread. */
typedef conn_t ConnWriter;
static ssize_t ConnWriter_write(VSelf, CharSlice99 data) {
    VSELF(ConnWriter);
    return conn_send_all(self, data.ptr, data.len) == 0 ? (ssize_t)data.len : -1;
}
static void ConnWriter_lock(VSelf) { VSELF(ConnWriter); (void)self; }
static void ConnWriter_unlock(VSelf) { VSELF(ConnWriter); (void)self; }
static size_t ConnWriter_filled(VSelf) { VSELF(ConnWriter); (void)self; return 0; }
static int ConnWriter_vwritef(VSelf, const char *restrict fmt, va_list ap) {
    VSELF(ConnWriter);
    char small[512];
    va_list ap2;
    va_copy(ap2, ap);
    int n = vsnprintf(small, sizeof(small), fmt, ap2);
    va_end(ap2);
    if (n < 0) return n;
    if ((size_t)n < sizeof(small))
        return conn_send_all(self, small, (size_t)n) == 0 ? n : -1;
    char *big = malloc((size_t)n + 1);
    if (!big) return -1;
    vsnprintf(big, (size_t)n + 1, fmt, ap);
    int ret = conn_send_all(self, big, (size_t)n) == 0 ? n : -1;
    free(big);
    return ret;
}
static int ConnWriter_writef(VSelf, const char *restrict fmt, ...) {
    VSELF(ConnWriter);
    va_list ap;
    va_start(ap, fmt);
    int ret = ConnWriter_vwritef(self, fmt, ap);
    va_end(ap);
    return ret;
}
impl(SmolRTSP_Writer, ConnWriter);

static int g_rtsp_clients;
static pthread_mutex_t g_clients_lock = PTHREAD_MUTEX_INITIALIZER;
int rtsp_client_connected(void) { return g_rtsp_clients; }

static void handle_client_inner(conn_t *cn, struct sockaddr_storage *client_addr,
                                socklen_t addr_len);

typedef struct { int fd; int tls; struct sockaddr_storage addr; socklen_t len; } accept_t;

/* PROXY protocol (v1 text or v2 binary), sent by a reverse proxy such as
 * Traefik (TCP router, proxyProtocol) before the RTSP or TLS bytes. Only
 * read when the connection comes from an address in web.proxies; the client
 * address it carries replaces the proxy's (login backoff, RTP destination is
 * unused: RTSPS through a proxy is TCP-interleaved). 0 ok/none, -1 broken. */
static int read_proxy_header_(int fd, struct sockaddr_storage *addr, socklen_t *len);
static int read_proxy_header(int fd, struct sockaddr_storage *addr, socklen_t *len)
{
    int r = read_proxy_header_(fd, addr, len);
    struct timeval none = { 0, 0 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &none, sizeof(none));
    return r;
}
static int read_proxy_header_(int fd, struct sockaddr_storage *addr, socklen_t *len)
{
    static const uint8_t sig2[12] = { 0x0D, 0x0A, 0x0D, 0x0A, 0x00, 0x0D, 0x0A, 0x51, 0x55, 0x49, 0x54, 0x0A };
    uint8_t pk[16];
    struct timeval tv = { 3, 0 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    ssize_t n = recv(fd, pk, sizeof pk, MSG_PEEK);
    if (n >= 6 && !memcmp(pk, "PROXY ", 6)) {                 /* v1: "PROXY TCP4 src dst sp dp\r\n" */
        char line[108];
        size_t l = 0;
        while (l < sizeof line - 1) {
            if (recv(fd, line + l, 1, 0) != 1) return -1;
            if (l && line[l - 1] == '\r' && line[l] == '\n') { line[l - 1] = 0; break; }
            l++;
        }
        if (l >= sizeof line - 1) return -1;
        char proto[8], src[48], dst[48];
        unsigned sp = 0, dp = 0;
        if (sscanf(line, "PROXY %7s %47s %47s %u %u", proto, src, dst, &sp, &dp) == 5 && !strcmp(proto, "TCP4")) {
            struct sockaddr_in *sin = (struct sockaddr_in *)addr;
            if (inet_pton(AF_INET, src, &sin->sin_addr) == 1) { sin->sin_family = AF_INET; sin->sin_port = htons((uint16_t)sp); *len = sizeof *sin; }
        }
        return 0;
    }
    if (n >= 16 && !memcmp(pk, sig2, 12)) {                   /* v2 */
        uint8_t h[16];
        if (recv(fd, h, 16, MSG_WAITALL) != 16) return -1;
        size_t al = (size_t)h[14] << 8 | h[15];
        uint8_t body[216];
        if (al > sizeof body || (al && recv(fd, body, al, MSG_WAITALL) != (ssize_t)al)) return -1;
        if ((h[12] & 0x0F) == 1 && (h[13] >> 4) == 1 && al >= 12) {   /* PROXY command, AF_INET */
            struct sockaddr_in *sin = (struct sockaddr_in *)addr;
            sin->sin_family = AF_INET;
            memcpy(&sin->sin_addr, body, 4);
            memcpy(&sin->sin_port, body + 8, 2);
            *len = sizeof *sin;
        }
        return 0;
    }
    return n < 0 ? -1 : 0;                                    /* no header: plain connection */
}

static void *client_thread(void *arg)
{
    accept_t *a = arg;
    if (a->addr.ss_family == AF_INET && web_is_proxy(((struct sockaddr_in *)&a->addr)->sin_addr.s_addr) &&
        read_proxy_header(a->fd, &a->addr, &a->len) != 0) {
        close(a->fd);
        a->fd = -1;
    }
    conn_t *cn = a->fd < 0 ? NULL : a->tls ? tls_accept(a->fd, 5000) : conn_plain(a->fd);
    if (cn) {
        if (a->tls) {
            struct timeval none = { 0, 0 };   /* session reads are polled */
            setsockopt(a->fd, SOL_SOCKET, SO_RCVTIMEO, &none, sizeof(none));
        }
        struct timeval tv = { 5, 0 };         /* a stuck client must not block forever */
        setsockopt(a->fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
        handle_client_inner(cn, &a->addr, a->len);
    } else if (!a->tls && a->fd >= 0) {
        close(a->fd);                          /* tls_accept closes on failure */
    }
    free(a);
    pthread_mutex_lock(&g_clients_lock);
    g_rtsp_clients--;
    pthread_mutex_unlock(&g_clients_lock);
    return NULL;
}

int rtsp_start_client(int fd, int tls, struct sockaddr_storage *addr, socklen_t len)
{
    pthread_mutex_lock(&g_clients_lock);
    int ok = g_rtsp_clients < RTSP_MAX_CLIENTS;
    if (ok) g_rtsp_clients++;
    pthread_mutex_unlock(&g_clients_lock);
    if (!ok) {
        printf("[ak_rtsp] %d clients already connected, refusing another\n", RTSP_MAX_CLIENTS);
        close(fd);
        return -1;
    }
    accept_t *a = calloc(1, sizeof *a);
    if (a) {                                   /* before the thread can read it */
        a->fd = fd; a->tls = tls; a->len = len;
        memcpy(&a->addr, addr, len);
    }
    pthread_t t;
    pthread_attr_t at;
    pthread_attr_init(&at);
    pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
    pthread_attr_setstacksize(&at, 256 * 1024);
    int rc = a ? pthread_create(&t, &at, client_thread, a) : -1;
    pthread_attr_destroy(&at);
    if (!a || rc != 0) {
        free(a);
        close(fd);
        pthread_mutex_lock(&g_clients_lock);
        g_rtsp_clients--;
        pthread_mutex_unlock(&g_clients_lock);
        return -1;
    }
    return 0;
}

static void handle_client_inner(conn_t *cn, struct sockaddr_storage *client_addr,
                                socklen_t addr_len)
{
    int client_fd = cn->fd;

    Client client;
    memset(&client, 0, sizeof(client));
    client.socket_fd = client_fd;
    client.tls = conn_is_tls(cn);
    client.bc_channel = -1;
    memcpy(&client.addr, client_addr, addr_len);
    client.addr_len = addr_len;
    client.rtp_udp_fd = -1;
    client.rtcp_udp_fd = -1;
    client.audio_rtp_udp_fd = -1;
    client.audio_rtcp_udp_fd = -1;

    SmolRTSP_Controller controller = DYN(Client, SmolRTSP_Controller, &client);
    SmolRTSP_Writer writer = DYN(ConnWriter, SmolRTSP_Writer, cn);
    uint8_t *frame = malloc(LIVE_MAX_FRAME);
    uint8_t *subbuf = NULL;               /* second-stream frames (rtsp://camera/sub) */
    live_item_t *item = malloc(sizeof(live_item_t));
    if (!frame || !item) { free(frame); free(item); conn_close(cn); return; }

    char buf[4096];
    size_t buf_len = 0;

    printf("[ak_rtsp] Client connected%s\n", client.tls ? " (RTSPS)" : "");
    client.connected_at = (time_t)mono_s();
    uint32_t last_req = mono_s();

    while (client_fd >= 0) {
        /* conn_readable also sees data already decrypted inside TLS */
        int poll_ret = conn_readable(cn, client.is_playing ? 0 : 5000);

        /* The server handles one client at a time: don't let a client that
         * never authenticates hold the slot. */
        uint32_t now_s = mono_s();
        if (!client.authed && now_s - (uint32_t)client.connected_at > 10) {
            printf("[ak_rtsp] unauthenticated client >10s, dropping\n");
            break;
        }
        if (!client.is_playing && now_s - last_req > 60) {
            printf("[ak_rtsp] client idle >60s without playing, dropping\n");
            break;
        }

        if (poll_ret < 0) {
            if (errno == EINTR) continue;
            perror("poll");
            break;
        }

        if (poll_ret > 0) {
            if (buf_len >= sizeof(buf) - 1) { printf("[ak_rtsp] request too large\n"); break; }
            ssize_t n = conn_recv_nb(cn, buf + buf_len, sizeof(buf) - buf_len - 1);
            last_req = mono_s();
            if (n == -2) {
                /* nothing yet (partial TLS record) */
            } else if (n < 0) {
                printf("[ak_rtsp] receive error, closing\n"); break;
            } else if (n == 0) {
                printf("[ak_rtsp] Client disconnected\n");
                break;
            } else {
                buf_len += n;
                buf[buf_len] = '\0';

                /* Interleaved binary frames from the client ($ ch len16 data):
                 * backchannel audio, or RTCP receiver reports (ignored). */
                size_t pos = 0;
                while (pos < buf_len && buf[pos] == '$') {
                    if (buf_len - pos < 4) break;
                    size_t fl = (size_t)((uint8_t)buf[pos + 2] << 8 | (uint8_t)buf[pos + 3]);
                    if (buf_len - pos < 4 + fl) break;
                    if ((uint8_t)buf[pos + 1] == client.bc_channel && fl >= 12)
                        backchannel_rtp(&client, (const uint8_t *)buf + pos + 4, fl);
                    pos += 4 + fl;
                }
                if (pos) {
                    memmove(buf, buf + pos, buf_len - pos);
                    buf_len -= pos;
                    buf[buf_len] = '\0';
                }
                if (!buf_len || buf[0] == '$') continue;   /* rest of a frame still to come */

                SmolRTSP_Request req = SmolRTSP_Request_uninit();
                CharSlice99 input = CharSlice99_new(buf, buf_len);
                SmolRTSP_ParseResult res = SmolRTSP_Request_parse(&req, input);

                bool should_consume = false;
                size_t bytes_to_consume = 0;

                match(res) {
                    of(SmolRTSP_ParseResult_Success, status) {
                        match(*status) {
                            of(SmolRTSP_ParseStatus_Complete, offset) {
                                should_consume = true;
                                bytes_to_consume = *offset;
                            }
                            otherwise {}
                        }
                    }
                    of(SmolRTSP_ParseResult_Failure, err) {
                        (void)err;
                        printf("[ak_rtsp] Request parse failed (%zu bytes buffered)\n",
                               buf_len);
                        buf_len = 0;
                    }
                }

                if (should_consume) {
                    smolrtsp_dispatch(writer, controller, &req);
                    if (client.auth_failures >= 5) {
                        printf("[ak_rtsp] too many auth failures, dropping client\n");
                        break;
                    }
                    if (bytes_to_consume < buf_len) {
                        memmove(buf, buf + bytes_to_consume, buf_len - bytes_to_consume);
                        buf_len -= bytes_to_consume;
                    } else {
                        buf_len = 0;
                    }
                }
            }
        }

        if (client.is_playing && client.sub) {
            /* second stream: video from substream.c, audio from the hub */
            if (!subbuf) subbuf = malloc(SUB_FRAME_MAX);
            int key = 0; uint64_t ts = 0;
            int n = subbuf ? substream_next(&client.sub_cur, subbuf, SUB_FRAME_MAX, &key, &ts, 0) : 0;
            if (n > 0 && client.nal_transport &&
                send_frame_nalus(&client, subbuf, (size_t)n, (uint32_t)(ts * 90)) < 0 && client.is_tcp) {
                printf("[ak_rtsp] client stopped receiving, closing\n");
                break;
            }
            int k = live_next(&client.cur, frame, LIVE_MAX_FRAME, item, n > 0 ? 0 : 20);
            if (k == LIVE_AUDIO) send_audio_frame(&client, item);
            send_rtcp_reports(&client);
        } else if (client.is_playing) {
            /* wait (briefly) for the next frame or audio period from the hub */
            int k = live_next(&client.cur, frame, LIVE_MAX_FRAME, item, 40);
            if (k == LIVE_VIDEO && client.nal_transport &&
                send_frame_nalus(&client, frame, item->len, (uint32_t)(item->ts * 90)) < 0 && client.is_tcp) {
                printf("[ak_rtsp] client stopped receiving, closing\n");   /* TCP send timed out */
                break;
            }
            else if (k == LIVE_AUDIO)
                send_audio_frame(&client, item);   /* no-op until the audio track is SETUP */
            send_rtcp_reports(&client); /* no-op for tracks not yet SETUP, or off-interval */
        }
    }

    if (client.subscribed) live_unsubscribe();
    free(subbuf);
    free(frame);
    free(item);
    VCALL(DYN(Client, SmolRTSP_Droppable, &client), drop);
    conn_close(cn);
    printf("[ak_rtsp] Client connection closed\n");
}

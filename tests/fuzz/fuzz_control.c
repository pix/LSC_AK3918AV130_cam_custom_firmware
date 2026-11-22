/* libFuzzer harness: settings registry and command protocol (control.c), and
 * through it every registered setting's validator (mqtt.*, acme.*, onvif.*,
 * record.*, events.*, sched.*, web.*, device.*, audio.*, sound.*, ...; the
 * hardware modules md/imgx/venc/night/audio/tamper/npu/substream are not in
 * the host build, so their settings are not registered here).
 *
 * Input: byte 0 = mode, byte 1 = selector, byte 2 = output size, rest = text.
 *   mode 0: control_exec() for each line of the text (LIST / GET / SET, as the
 *           web page's /api/set and MQTT/ONVIF use it); output buffer of the
 *           exact size 8*byte2 (0 = none)
 *   mode 1: control_set_value(<setting #selector>, text) (web /api/config,
 *           MQTT commands), then control_get_value() into an exact-size buffer
 *   mode 2: control_set_value(<every setting>, text)
 *   mode 3: the port-8091 session (handle_control_client: AUTH, then commands)
 *           with the text as what the client sends (loopback only on the camera)
 * Every mode ends with control_params_json(prefix, exact-size buffer) and a
 * reset of all settings to their start values (deterministic inputs).
 * In control.c, sleep() (the 1 + n s penalty after a wrong AUTH) is a no-op. */
#include <unistd.h>
#include <signal.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
static unsigned fz_sleep(unsigned s) { (void)s; return 0; }
#define sleep fz_sleep
#include "control.c"
#undef sleep

#include "rtsp.h"
#include "mqtt.h"
#include "acme.h"
#include "events.h"
#include "sounddet.h"
#include "sched.h"
#include "web.h"
#include "term.h"
#include "record.h"
#include "onvif.h"
#include "snapshot.h"
void aec_register_params(void);

static char g_def[MAX_REG][512];
static ae_tuning_t g_def_ae;
static night_tuning_t g_def_nt;
static char g_def_ntp[128], g_def_tz[128];
static int g_def_fb, g_def_vol;

static void init_once(void)
{
    static int done;
    if (done) return;
    done = 1;
    signal(SIGPIPE, SIG_IGN);
    auth_init();
    rtsp_register_params();
    sys_register_params();
    aec_register_params();
    mqtt_init();
    acme_init();
    events_init();
    sounddet_init();
    sched_init();
    web_register_params();
    term_init();
    record_init();
    onvif_init();
    snapshot_init();
    for (int i = 0; i < g_nreg; i++) g_reg[i].get(g_def[i], sizeof g_def[i]);
    ae_get_tuning(&g_def_ae);
    night_get_tuning(&g_def_nt);
    snprintf(g_def_ntp, sizeof g_def_ntp, "%s", sys_time_server());
    snprintf(g_def_tz, sizeof g_def_tz, "%s", sys_time_tz());
    g_def_fb = net_get_fallback();
    g_def_vol = speaker_get_volume();
}

static void reset_all(void)
{
    for (int i = 0; i < g_nreg; i++) {
        char cur[512] = "";
        g_reg[i].get(cur, sizeof cur);
        if (strcmp(cur, g_def[i])) { const char *e; g_reg[i].set(g_def[i], &e); }
    }
    ae_set_tuning(&g_def_ae);
    night_set_tuning(&g_def_nt);
    g_isp_saturation = g_isp_contrast = g_isp_brightness = g_isp_sharpness = 0;
    g_img_flip = g_img_mirror = 0;
    ircut_set_ir_mode(0);
    if (strcmp(sys_time_server(), g_def_ntp)) sys_time_set_server(g_def_ntp);
    if (strcmp(sys_time_tz(), g_def_tz)) sys_time_set_tz(g_def_tz);
    net_set_fallback(g_def_fb);
    speaker_set_volume(g_def_vol);
    g_loading_settings = 0;
}

static void control_session(const char *text, size_t n)
{
    int sv[2];
    if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sv)) abort();
    int big = 1 << 20;
    setsockopt(sv[0], SOL_SOCKET, SO_SNDBUF, &big, sizeof big);
    setsockopt(sv[1], SOL_SOCKET, SO_SNDBUF, &big, sizeof big);
    if (n && write(sv[1], text, n) != (ssize_t)n) abort();
    shutdown(sv[1], SHUT_WR);
    /* replies are only read after the session: non-blocking, so many LISTs
     * cannot fill the socket and block reply() (all input is already queued,
     * so reads never see EAGAIN) */
    fcntl(sv[0], F_SETFL, O_NONBLOCK);
    struct sockaddr_in peer = { 0 };
    peer.sin_family = AF_INET;
    peer.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    handle_control_client(sv[0], (const struct sockaddr *)&peer);
    g_client_authed = 0;
    close(sv[0]);
    char sink[16384];
    while (recv(sv[1], sink, sizeof sink, MSG_DONTWAIT) > 0) {}
    close(sv[1]);
}

int LLVMFuzzerTestOneInput(const uint8_t *d, size_t n)
{
    init_once();
    if (n < 3 || n > 4096) return 0;
    int mode = d[0] & 3;
    unsigned sel = d[1];
    size_t cap = (size_t)d[2] * 8;
    char *text = malloc(n - 3 + 1);
    memcpy(text, d + 3, n - 3);
    text[n - 3] = 0;
    char *out = malloc(cap ? cap : 1);
    const char *err;

    if (mode == 0) {
        char *save = NULL;
        for (char *l = strtok_r(text, "\n", &save); l; l = strtok_r(NULL, "\n", &save))
            control_exec(l, cap ? out : NULL, cap);
    } else if (mode == 1 && g_nreg) {
        const char *name = g_reg[sel % g_nreg].name;
        control_set_value(name, text, &err);
        if (cap) control_get_value(name, out, cap, sel & 0x80);
    } else if (mode == 2) {
        for (int i = 0; i < g_nreg; i++) control_set_value(g_reg[i].name, text, &err);
    } else if (mode == 3) {
        control_session(text, n - 3);
    }

    /* JSON of one module's settings ("" = all), as the web page reads them */
    char prefix[32] = "";
    if (g_nreg && (sel & 1)) {
        const char *nm = g_reg[(sel >> 1) % g_nreg].name, *dot = strchr(nm, '.');
        snprintf(prefix, sizeof prefix, "%.*s", dot ? (int)(dot - nm + 1) : (int)strlen(nm), nm);
    }
    if (cap) control_params_json(prefix, out, cap);
    char big[8192];
    control_params_json("", big, sizeof big);

    reset_all();
    free(out);
    free(text);
    return 0;
}

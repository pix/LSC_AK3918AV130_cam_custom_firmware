/* Development terminal: a root shell on a pseudo-terminal, over a WebSocket.
 *
 * Off unless dev.terminal = on (System tab). A session needs a one-time token
 * (60 s) from an authenticated request with X-Requested-With, like the live
 * view, and the page's Origin. One session at a time; it ends when the page
 * closes, the shell exits, or after 60 minutes without input.
 *
 * WebSocket messages: camera -> page: binary frames of terminal output.
 * page -> camera: text or binary frames of keyboard input; a frame starting
 * with a NUL byte is a control message: "\0R<cols>x<rows>" resizes. */
#define _GNU_SOURCE                 /* ptsname_r */
#include "term.h"
#include "monotime.h"
#include "ctrl_params.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

static int s_enabled;
CTRL_BOOL(p_en, s_enabled)
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static int g_active;
static volatile int g_kick;              /* a newer session asked this one to end */
static struct { char tok[33]; uint32_t until; } g_tok;

int term_enabled(void) { return s_enabled; }

int term_token_new(char out[33])
{
    if (!s_enabled) return -1;
    uint8_t r[16];
    int fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    if (fd < 0) return -1;
    ssize_t n = read(fd, r, sizeof r);
    close(fd);
    if (n != (ssize_t)sizeof r) return -1;
    for (int i = 0; i < 16; i++) snprintf(out + 2 * i, 3, "%02x", r[i]);
    pthread_mutex_lock(&g_lock);
    memcpy(g_tok.tok, out, 33);
    g_tok.until = mono_s() + 60;
    pthread_mutex_unlock(&g_lock);
    return 0;
}

int term_busy(void)
{
    pthread_mutex_lock(&g_lock);
    int b = g_active;
    pthread_mutex_unlock(&g_lock);
    return b;
}

int term_token_take(const char *tok)
{
    if (!s_enabled) return 0;
    if (!tok || strlen(tok) != 32) return 0;
    pthread_mutex_lock(&g_lock);
    unsigned char d = 0;
    for (int k = 0; k < 32; k++) d |= (unsigned char)(g_tok.tok[k] ^ tok[k]);
    int ok = !d && g_tok.tok[0] && mono_s() <= g_tok.until;
    if (ok) memset(&g_tok, 0, sizeof g_tok);            /* single use */
    pthread_mutex_unlock(&g_lock);
    return ok;
}

/* ---- WebSocket framing ---------------------------------------------------- */
static int ws_send(conn_t *cn, const uint8_t *d, size_t len)
{
    uint8_t h[10];
    size_t hl = 0;
    h[hl++] = 0x82;                                     /* FIN + binary */
    if (len < 126) h[hl++] = (uint8_t)len;
    else { h[hl++] = 126; h[hl++] = (uint8_t)(len >> 8); h[hl++] = (uint8_t)len; }   /* len <= 4096 */
    if (conn_send_all(cn, h, hl) != 0) return -1;
    return conn_send_all(cn, d, len);
}

typedef struct { uint8_t b[8192]; size_t n; } rx_t;

/* returns 1 when the page is gone or misbehaves */
static int ws_input(conn_t *cn, rx_t *rx, int ptm, int64_t *last_in)
{
    if (rx->n == sizeof rx->b) return 1;
    ssize_t n = conn_recv_nb(cn, rx->b + rx->n, sizeof rx->b - rx->n);
    if (n == -2) return 0;
    if (n <= 0) return 1;
    rx->n += (size_t)n;
    for (;;) {
        if (rx->n < 2) return 0;
        uint8_t op = rx->b[0] & 0x0f;
        uint64_t len = rx->b[1] & 0x7f;
        size_t h = 2;
        if (!(rx->b[1] & 0x80)) return 1;                /* clients must mask */
        if (len == 126) { if (rx->n < 4) return 0; len = (uint64_t)rx->b[2] << 8 | rx->b[3]; h = 4; }
        else if (len == 127) return 1;
        if (h + 4 + len > sizeof rx->b) return 1;
        if (rx->n < h + 4 + len) return 0;
        uint8_t *m = rx->b + h, *d = rx->b + h + 4;
        for (uint64_t i = 0; i < len; i++) d[i] ^= m[i & 3];
        if (op == 0x8) return 1;                          /* close */
        if ((op == 0x1 || op == 0x2) && len) {
            *last_in = mono_ms();
            if (d[0] == 0) {                              /* control */
                unsigned c = 0, r = 0;
                char t[24];
                size_t tl = len - 1 < sizeof t - 1 ? (size_t)len - 1 : sizeof t - 1;
                memcpy(t, d + 1, tl); t[tl] = 0;
                if (sscanf(t, "R%ux%u", &c, &r) == 2 && c >= 10 && c <= 400 && r >= 4 && r <= 200) {
                    struct winsize ws = { (unsigned short)r, (unsigned short)c, 0, 0 };
                    ioctl(ptm, TIOCSWINSZ, &ws);
                }
            } else {
                size_t off = 0;
                while (off < len) {
                    ssize_t w = write(ptm, d + off, (size_t)len - off);
                    if (w < 0) { if (errno == EINTR) continue; if (errno == EAGAIN) { usleep(2000); continue; } return 1; }
                    off += (size_t)w;
                }
            }
        }
        size_t used = h + 4 + (size_t)len;
        memmove(rx->b, rx->b + used, rx->n - used);
        rx->n -= used;
    }
}

typedef struct { conn_t *cn; char peer[48]; } sess_t;

static void *session(void *arg)
{
    sess_t *s = arg;
    conn_t *cn = s->cn;
    int ptm = posix_openpt(O_RDWR | O_NOCTTY | O_CLOEXEC);
    char slave[64] = "";
    pid_t pid = -1;
    rx_t *rx = calloc(1, sizeof *rx);
    if (ptm < 0 || grantpt(ptm) != 0 || unlockpt(ptm) != 0 || ptsname_r(ptm, slave, sizeof slave) != 0 || !rx) {
        const char *msg = "\r\ncannot open a terminal on the camera\r\n";
        ws_send(cn, (const uint8_t *)msg, strlen(msg));
        goto out;
    }
    struct winsize ws = { 24, 80, 0, 0 };
    ioctl(ptm, TIOCSWINSZ, &ws);
    pid = fork();
    if (pid == 0) {                                       /* child: async-signal-safe calls until exec */
        setsid();
        int sl = open(slave, O_RDWR);
        if (sl < 0) _exit(127);
        ioctl(sl, TIOCSCTTY, 0);
        dup2(sl, 0); dup2(sl, 1); dup2(sl, 2);
        for (int fd = 3; fd < 1024; fd++) close(fd);      /* never hand the camera's devices to the shell */
        struct sigaction dfl;
        memset(&dfl, 0, sizeof dfl);
        dfl.sa_handler = SIG_DFL;
        sigaction(SIGPIPE, &dfl, NULL);                   /* ignored in ak_rtsp; would stay ignored across exec */
        sigset_t none;
        sigemptyset(&none);
        sigprocmask(SIG_SETMASK, &none, NULL);
        static char *const envp[] = { "TERM=xterm", "HOME=/", "SHELL=/bin/sh",
                                      "PATH=/bin:/sbin:/usr/bin:/usr/sbin:/tmp/sd/maint", "PS1=\\h:\\w# ", NULL };
        if (chdir("/") != 0) { /* keep going */ }
        execle("/bin/sh", "-sh", (char *)NULL, envp);
        _exit(127);
    }
    if (pid < 0) goto out;
    printf("[term] session opened from %s (shell pid %d)\n", s->peer, (int)pid);
    fcntl(ptm, F_SETFL, fcntl(ptm, F_GETFL) | O_NONBLOCK);
    int64_t last_in = mono_ms();
    uint8_t out[4096];
    for (;;) {
        struct pollfd p[2] = { { ptm, POLLIN, 0 }, { cn->fd, POLLIN, 0 } };
        if (g_kick) {
            const char *msg = "\r\n[session taken over from another tab or device]\r\n";
            ws_send(cn, (const uint8_t *)msg, strlen(msg));
            break;
        }
        int ready = conn_readable(cn, 0);                 /* data already decrypted inside TLS */
        int pr = ready > 0 ? 0 : poll(p, 2, 250);
        if (pr < 0 && errno != EINTR) break;
        if (p[0].revents & (POLLIN | POLLHUP | POLLERR)) {
            ssize_t n = read(ptm, out, sizeof out);
            if (n > 0) { if (ws_send(cn, out, (size_t)n) != 0) break; }
            else if (n == 0 || (errno != EAGAIN && errno != EINTR)) break;   /* EIO: the shell exited */
        }
        if (ready > 0 || (p[1].revents & (POLLIN | POLLHUP | POLLERR))) {
            if (ws_input(cn, rx, ptm, &last_in)) break;
        }
        if (mono_ms() - last_in > 3600000) {
            const char *msg = "\r\n[closed after 60 minutes without input]\r\n";
            ws_send(cn, (const uint8_t *)msg, strlen(msg));
            break;
        }
    }
out:
    if (pid > 0) {
        kill(-pid, SIGHUP);                               /* the whole session (setsid) */
        for (int i = 0; i < 20 && waitpid(pid, NULL, WNOHANG) == 0; i++) usleep(50000);
        if (waitpid(pid, NULL, WNOHANG) == 0) { kill(-pid, SIGKILL); waitpid(pid, NULL, 0); }
        printf("[term] session from %s closed\n", s->peer);
    }
    if (ptm >= 0) close(ptm);
    free(rx);
    conn_close(cn);
    free(s);
    pthread_mutex_lock(&g_lock);
    g_active = 0;
    pthread_mutex_unlock(&g_lock);
    return NULL;
}

int term_ws_start(conn_t *cn, const char *peer)
{
    if (!s_enabled) return -1;
    /* one session at a time: a new one takes over (a forgotten tab must not
     * lock everyone out). The old session ends its shell and closes. */
    for (int i = 0; i < 60; i++) {
        pthread_mutex_lock(&g_lock);
        if (!g_active) { g_active = 1; g_kick = 0; pthread_mutex_unlock(&g_lock); goto start; }
        g_kick = 1;
        pthread_mutex_unlock(&g_lock);
        usleep(50000);
    }
    return -2;                                            /* the old one did not end within 3 s */
start:;
    sess_t *s = calloc(1, sizeof *s);
    pthread_t t;
    pthread_attr_t a;
    pthread_attr_init(&a);
    pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED);
    pthread_attr_setstacksize(&a, 64 * 1024);
    int rc = s ? (s->cn = cn, snprintf(s->peer, sizeof s->peer, "%s", peer ? peer : "?"),
                  pthread_create(&t, &a, session, s)) : -1;
    pthread_attr_destroy(&a);
    if (rc != 0) {
        free(s);
        pthread_mutex_lock(&g_lock); g_active = 0; pthread_mutex_unlock(&g_lock);
        return -1;
    }
    return 0;
}

void term_init(void) { CTRL_REG("dev.terminal", p_en, CTRL_PERSIST); }

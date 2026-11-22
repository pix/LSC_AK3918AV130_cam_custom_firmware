/* End-to-end harness on the PC: the camera's tls.c + acme.c against a real
 * ACME server (Pebble). Serves http-01 tokens on HTTP_PORT and HTTPS (with
 * SNI selection) on HTTPS_PORT, applies settings given as name=value,
 * runs one issuance and prints the status JSON.
 * Usage: e2e_host <http port> <https port> [ca.pem|-] [register] name=value...
 * Exit 0 when the issuance ended in state "ok". */
#include "tls.h"
#include "acme.h"
#include "conn.h"
#include "control.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>

static ctrl_param_t g_params[32];
static int g_np;
int control_register(const ctrl_param_t *p) { g_params[g_np++] = *p; return 0; }
static const ctrl_param_t *pfind(const char *n) { for (int i = 0; i < g_np; i++) if (!strcmp(g_params[i].name, n)) return &g_params[i]; return NULL; }
int control_get_value(const char *name, char *out, size_t cap, int reveal)
{
    (void)reveal;
    const ctrl_param_t *p = pfind(name);
    if (!p) return -1;
    out[0] = 0;
    return p->get(out, cap);
}
int control_set_value(const char *name, const char *value, const char **err)
{
    const ctrl_param_t *p = pfind(name);
    if (!p) { *err = "unknown setting"; return -1; }
    return p->set(value, err);
}

static int listen_on(int port)
{
    int s = socket(AF_INET, SOCK_STREAM, 0), one = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons((uint16_t)port) };
    if (bind(s, (struct sockaddr *)&a, sizeof a) || listen(s, 16)) { perror("bind"); exit(2); }
    return s;
}

static void *http_thread(void *arg)            /* like web.c: GET /.well-known/acme-challenge/<token> */
{
    int s = listen_on((int)(intptr_t)arg);
    for (;;) {
        int c = accept(s, NULL, NULL);
        if (c < 0) continue;
        char req[2048] = "", path[300] = "";
        ssize_t n = recv(c, req, sizeof req - 1, 0);
        if (n > 0) sscanf(req, "GET %299s", path);
        char ka[256], resp[512];
        int k = !strncmp(path, "/.well-known/acme-challenge/", 28) && strlen(path + 28) < 128 ? acme_http01(path + 28, ka, sizeof ka) : -1;
        int l = k > 0 ? snprintf(resp, sizeof resp, "HTTP/1.1 200 OK\r\nContent-Type: application/octet-stream\r\nContent-Length: %d\r\nConnection: close\r\n\r\n%s", k, ka)
                      : snprintf(resp, sizeof resp, "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
        printf("[e2e] http-01 request %s -> %s\n", path, k > 0 ? "200" : "404");
        send(c, resp, (size_t)l, MSG_NOSIGNAL);
        close(c);
    }
    return NULL;
}

static void *https_thread(void *arg)
{
    int s = listen_on((int)(intptr_t)arg);
    for (;;) {
        int fd = accept(s, NULL, NULL);
        if (fd < 0) continue;
        conn_t *c = tls_accept(fd, 5000);
        if (!c) continue;
        char b[1024];
        conn_recv_nb(c, b, sizeof b);
        const char *r = "HTTP/1.1 200 OK\r\nContent-Length: 3\r\nConnection: close\r\n\r\nok\n";
        conn_send_all(c, r, strlen(r));
        conn_close(c);
    }
    return NULL;
}

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    acme_init();
    if (tls_init() != 0) { printf("tls_init failed\n"); return 2; }
    pthread_t t;
    pthread_create(&t, NULL, http_thread, (void *)(intptr_t)atoi(argv[1]));
    pthread_create(&t, NULL, https_thread, (void *)(intptr_t)atoi(argv[2]));
    const char *e;
    if (strcmp(argv[3], "-")) {
        FILE *f = fopen(argv[3], "r");
        char pem[8192];
        size_t n = fread(pem, 1, sizeof pem, f);
        fclose(f);
        if (tls_acme_ca_set(pem, n, &e)) { printf("extra CA: %s\n", e); return 2; }
    } else tls_acme_ca_set(NULL, 0, &e);
    int i = 4, reg = 0;
    if (argc > 4 && !strcmp(argv[4], "register")) { reg = 1; i++; }
    for (; i < argc; i++) {
        char *eq = strchr(argv[i], '=');
        if (!eq) continue;
        *eq = 0;
        if (control_set_value(argv[i], eq + 1, &e)) { printf("setting %s: %s\n", argv[i], e); return 2; }
    }
    if (reg) {
        char out[300], err[200];
        if (acme_dns_register(out, sizeof out, err, sizeof err) < 0) { printf("register: %s\n", err); return 3; }
        printf("[e2e] register: %s\n", out);
    }
    static char js[8192];
    if (getenv("E2E_SERVE")) {                         /* keep serving (SNI checks); issue in the background */
        acme_start();
        sleep((unsigned)atoi(getenv("E2E_SERVE")));
        return 0;
    }
    acme_start();
    acme_run_now();
    int waited = 0;
    for (; waited < 400; waited++) {
        sleep(1);
        acme_status_json(js, sizeof js);
        if (waited > 3 && !strstr(js, "\"state\":\"running\"") && !strstr(js, "\"state\":\"waiting\"")) break;
    }
    printf("STATUS %s\n", js);
    tls_info_json(js, sizeof js);
    printf("TLS %s\n", js);
    if (getenv("E2E_LINGER")) sleep((unsigned)atoi(getenv("E2E_LINGER")));   /* time for SNI checks */
    acme_status_json(js, sizeof js);
    return strstr(js, "\"state\":\"ok\"") ? 0 : 1;
}

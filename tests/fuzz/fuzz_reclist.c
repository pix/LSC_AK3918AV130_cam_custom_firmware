/* libFuzzer harness: recording names / paths that come from requests.
 *   web.c   parse_rec_list() (POST /api/rec/tar, /api/rec/delete-many bodies)
 *           + what handle_conn does with the list (day/name split, sizes
 *           kept at byte 40, tar_header()); /rec/<day>/<name> splitting and
 *           send_recording() (Range header parsing, small-file path)
 *   record.c record_path(), record_delete(), record_list_json(), record_is_live()
 * Byte 0 selects the mode, the rest is the request text:
 *   0  list body (one "YYYY-MM-DD/name" per line / comma)
 *   1  form body: day=..&name=.. (POST /api/rec/delete, GET /api/rec/list?day=)
 *   2  "<path>\n<Range header value>" for GET /rec/...
 * Fixture recordings live under the host build's REC_ROOT and are recreated
 * when an input deletes them. sleep/sync are no-ops (as in fuzz_http_request.c). */
#include <unistd.h>
#include <sys/socket.h>
#include <poll.h>
#include <signal.h>
#include <sched.h>
#include "web.c"

void sync(void) {}

#define REC_DAY REC_DIR "/2026-11-01"

static void put_file(const char *path, size_t size)
{
    struct stat st;
    if (stat(path, &st) == 0 && (size_t)st.st_size == size) return;
    FILE *f = fopen(path, "w");
    if (!f) return;
    for (size_t i = 0; i < size; i++) fputc((int)(i & 0xff), f);
    fclose(f);
}

static void fixtures(void)
{
    mkdir(REC_DIR, 0755);
    mkdir(REC_DAY, 0755);
    put_file(REC_DAY "/12-00-00_motion.mp4", 1000);
    put_file(REC_DAY "/12-00-05_motion.jpg", 300);
    put_file(REC_DAY "/13-00-00_cont.mp4", 400 * 1024);
}

static void *drain(void *arg)
{
    int fd = *(int *)arg;
    char b[16384];
    while (recv(fd, b, sizeof b, 0) > 0) {}
    return NULL;
}

static void wait_senders(void)
{
    for (;;) {
        pthread_mutex_lock(&g_send_lock);
        int busy = g_senders;
        pthread_mutex_unlock(&g_send_lock);
        if (!busy) return;
        sched_yield();
    }
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    static int inited;
    if (!inited) { inited = 1; signal(SIGPIPE, SIG_IGN); }
    if (size < 1) return 0;
    fixtures();
    char *s = malloc(size);
    memcpy(s, data + 1, size - 1);
    s[size - 1] = 0;
    int mode = data[0] % 3;

    if (mode == 0) {
        char (*list)[48] = NULL;
        int n = parse_rec_list(s, &list, 64);
        for (int i = 0; i < n; i++) {
            char full[256], day[16], *slash = strchr(list[i], '/');
            snprintf(day, sizeof(day), "%.*s", (int)(slash - list[i]), list[i]);
            record_path(day, slash + 1, full, sizeof(full));
            struct stat st;
            long long sz = stat(full, &st) == 0 ? (long long)st.st_size : 0;
            memcpy(list[i] + 40, &sz, sizeof(sz));
            char h[512];
            tar_header(h, list[i], *(long long *)(list[i] + 40), 0);
            if (data[0] & 0x80) {                         /* delete-many */
                *slash = 0;
                record_delete(list[i], slash + 1);
            }
        }
        free(list);
    } else if (mode == 1) {
        char day[16] = "", name[64] = "", full[256];
        form_get(s, "day", day, sizeof(day));
        form_get(s, "name", name, sizeof(name));
        record_path(day, name, full, sizeof full);
        record_is_live(day, name);
        static char json[65536];
        record_list_json(day, json, sizeof json);
        if (data[0] & 0x80) record_delete(day, name);
    } else {
        char *nl = strchr(s, '\n');
        const char *range = "";
        if (nl) { *nl = 0; range = nl + 1; }
        char path[256], rng[64];
        snprintf(path, sizeof path, "%s", s);
        snprintf(rng, sizeof rng, "%s", range);           /* req_t.range is 64 bytes */
        char *q = strchr(path, '?');
        if (q) *q = 0;
        if (!strncmp(path, "/rec/", 5)) {                 /* = handle_conn */
            char day[16] = "", name[64] = "";
            const char *slash = strchr(path + 5, '/');
            if (slash && (size_t)(slash - (path + 5)) < sizeof(day)) {
                memcpy(day, path + 5, (size_t)(slash - (path + 5)));
                snprintf(name, sizeof(name), "%s", slash + 1);
                int sv[2];
                if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sv) == 0) {
                    pthread_t th;
                    pthread_create(&th, NULL, drain, &sv[0]);
                    conn_t *cn = conn_plain(sv[1]);
                    if (!send_recording(cn, day, name, rng, data[0] & 0x80)) conn_close(cn);
                    shutdown(sv[0], SHUT_WR);
                    pthread_join(th, NULL);
                    close(sv[0]);
                    wait_senders();
                }
            }
        }
    }
    free(s);
    return 0;
}

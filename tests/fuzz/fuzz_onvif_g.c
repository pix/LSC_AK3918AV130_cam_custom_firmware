/* libFuzzer harness: ONVIF Profile G SOAP requests (onvif_g.c: search,
 * recording and replay services) on a small recordings tree, as onvif.c
 * hands them over after the login check.
 *
 * Input: byte 0 = action index (k_action[], modulo), byte 1 = flags
 *   bit0: path of the action's own service (else /onvif/device_service)
 *   bit1: onvif.replay off
 *   bit2: a search token in the answer is followed up (results paging with
 *         MaxResults = byte 2, GetSearchState, EndSearch)
 * then the SOAP body (NUL-terminated and limited to 16 KB as web.c does).
 * Search sessions persist between inputs like on the camera (bounded table). */
#include "onvif_g.c"                 /* s_enabled set directly (no settings file written per input) */
#include "record.h"
#include "pg_fixture.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static const char *const k_action[] = {
    "GetServiceCapabilities", "GetRecordingSummary", "GetRecordingInformation", "GetMediaAttributes", "FindRecordings",
    "GetRecordingSearchResults", "FindEvents", "GetEventSearchResults", "GetSearchState", "EndSearch", "FindPTZPosition",
    "GetRecordings", "GetRecordingConfiguration", "GetTrackConfiguration", "GetRecordingJobs", "CreateRecording",
    "GetReplayUri", "GetReplayConfiguration", "SetReplayConfiguration", "GetDeviceInformation", "",
};
#define NACT (sizeof k_action / sizeof k_action[0])

static const char *path_of(const char *a)
{
    if (!strcmp(a, "GetReplayUri") || strstr(a, "ReplayConfiguration")) return "/onvif/replay_service";
    if (!strcmp(a, "GetRecordings") || strstr(a, "Configuration") || strstr(a, "RecordingJobs") || !strcmp(a, "CreateRecording"))
        return "/onvif/recording_service";
    return "/onvif/search_service";
}

static void put(const char *p) { FILE *f = fopen(p, "wb"); if (f) { fputs("garbage, not an mp4 at all", f); fclose(f); } }

int LLVMFuzzerInitialize(int *argc, char ***argv)
{
    (void)argc; (void)argv;
    mkdir(REC_ROOT, 0755); mkdir(REC_DIR, 0755);
    mkdir(REC_DIR "/2026-01-01", 0755); mkdir(REC_DIR "/2026-01-02", 0755); mkdir(REC_DIR "/2026-1-03", 0755);
    pg_make_mp4(REC_DIR "/2026-01-01/10-00-00_motion.mp4", 30, 15, 1);
    pg_make_mp4(REC_DIR "/2026-01-01/10-30-00_sound.mp4", 20, 10, 0);
    pg_make_mp4(REC_DIR "/2026-01-01/12-00-00_timelapse.mp4", 5, 1, 0);
    put(REC_DIR "/2026-01-01/11-00-00_cont.mp4");
    put(REC_DIR "/2026-01-01/11-30-00_event.jpg");
    pg_make_mp4(REC_DIR "/2026-01-02/09-00-00_event.mp4", 16, 15, 1);
    onvif_g_init();
    return 0;
}

static void call(const char *path, const char *action, const char *body, char **out)
{
    int code = 0;
    *out = NULL;
    if (!onvif_g_handle(path, action, body, "http://192.168.1.50", "192.168.1.50", out, &code)) *out = NULL;
}

int LLVMFuzzerTestOneInput(const uint8_t *d, size_t n)
{
    if (n < 3) return 0;
    const char *action = k_action[d[0] % NACT];
    uint8_t flags = d[1], maxr = d[2];
    s_enabled = !(flags & 2);
    size_t bl = n - 3 > 16384 ? 16384 : n - 3;
    char *body = malloc(bl + 1);
    memcpy(body, d + 3, bl);
    body[bl] = 0;
    char *out;
    call(flags & 1 ? path_of(action) : "/onvif/device_service", action, body, &out);
    if (out && (flags & 4)) {
        const char *t = strstr(out, "SearchToken>search-");
        if (t) {
            char tok[32] = "";
            sscanf(t + 12, "%31[^<]", tok);
            int ev = strstr(out, "FindEventsResponse") != NULL;
            char req[256], *o2;
            snprintf(req, sizeof req, "<x:%s><x:SearchToken>%s</x:SearchToken><x:MaxResults>%d</x:MaxResults></x:%s>",
                     ev ? "GetEventSearchResults" : "GetRecordingSearchResults", tok, maxr, "y");
            for (int i = 0; i < 3; i++) {
                call("/onvif/search_service", ev ? "GetEventSearchResults" : "GetRecordingSearchResults", req, &o2);
                free(o2);
            }
            snprintf(req, sizeof req, "<x:SearchToken>%s</x:SearchToken>", tok);
            call("/onvif/search_service", "GetSearchState", req, &o2); free(o2);
            if (maxr & 1) { call("/onvif/search_service", "EndSearch", req, &o2); free(o2); }
        }
    }
    free(out);
    char buf[1024];
    onvif_g_caps("http://192.168.1.50", buf, sizeof buf);
    onvif_g_services("http://192.168.1.50", buf, sizeof buf);
    (void)onvif_g_scope();
    free(body);
    return 0;
}

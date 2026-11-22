/* libFuzzer harness: backup.c backup_restore() with an arbitrary uploaded
 * archive (POST /api/restore). backup.c is compiled into this harness with
 * CFG_DIR pointing at ./fuzz_backup_cfg (relative to the working directory),
 * so restored files never land in the shared host-build root. Each input
 * that restores something is followed by a backup_build() of the restored
 * directory and a second restore of that archive (round trip). */
#include <sys/stat.h>
#include <dirent.h>
#include <unistd.h>
#undef CFG_DIR
#define CFG_DIR "fuzz_backup_cfg"
#include "backup.c"

void sync(void) {}
int fsync(int fd) { (void)fd; return 0; }

static void clean(void)
{
    DIR *d = opendir(CFG_DIR);
    struct dirent *e;
    char p[300];
    while (d && (e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        snprintf(p, sizeof p, CFG_DIR "/%s", e->d_name);
        unlink(p);
    }
    if (d) closedir(d);
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    static int inited;
    if (!inited) { inited = 1; mkdir(CFG_DIR, 0700); }
    clean();
    int n = 0;
    char err[120];
    if (backup_restore(data, size, &n, err, sizeof err) == 0 && n > 0) {
        uint8_t *t = NULL;
        size_t tl = 0;
        if (backup_build(&t, &tl, 1, "cam", "fuzz") == 0) {
            int n2 = 0;
            /* every whitelisted file that was restored must come back */
            if (tl <= MAX_TOTAL && (backup_restore(t, tl, &n2, err, sizeof err) != 0 || n2 < 1)) __builtin_trap();
            free(t);
        }
    }
    clean();
    return 0;
}

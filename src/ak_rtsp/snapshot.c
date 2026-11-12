#include "snapshot.h"
#include "vi.h"
#include "jpeg_enc.h"
#include <pthread.h>
#include "ctrl_params.h"

/* image.jpeg_quality: snapshots, event pictures, MQTT pictures */
static int s_quality = 80;
CTRL_INT(p_q, s_quality, 30, 95)
int  snapshot_quality(void) { return s_quality; }
void snapshot_init(void) { CTRL_REG("image.jpeg_quality", p_q, CTRL_PERSIST); }

int snapshot_jpeg(uint8_t *out, size_t cap, int quality)
{
    if (quality <= 0) quality = s_quality;
    static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
    static uint8_t nv12[640 * 368 * 3 / 2];
    int w, h, n = -1;
    pthread_mutex_lock(&lock);
    if (vi_copy_sub_frame(nv12, sizeof(nv12), &w, &h) >= 0)
        n = jpeg_encode_nv12(nv12, w, h, quality, out, (int)cap);
    pthread_mutex_unlock(&lock);
    return n;
}

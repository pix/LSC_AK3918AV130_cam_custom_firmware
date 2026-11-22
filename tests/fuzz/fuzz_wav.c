/* libFuzzer harness: speaker.c speaker_play_wav(), the parser + resampler for
 * WAV files uploaded to /hook/sound (token, no login) - whole input = file.
 * The decoded samples go to the speaker queue, which is flushed per input. */
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include "speaker.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    /* the web server passes a heap copy of exactly the body: do the same so
     * ASan sees reads past the end */
    uint8_t *d = size ? (uint8_t *)__builtin_malloc(size) : NULL;
    if (size) memcpy(d, data, size);
    const char *err = NULL;
    speaker_stop();
    if (d) speaker_play_wav(d, size, &err);
    speaker_stop();
    __builtin_free(d);
    return 0;
}

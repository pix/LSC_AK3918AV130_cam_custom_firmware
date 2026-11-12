#include <stdint.h>
#include <stddef.h>
#include "speaker.h"
int LLVMFuzzerTestOneInput(const uint8_t *d, size_t n){ const char *e; speaker_play_wav(d,n,&e); speaker_stop(); return 0; }

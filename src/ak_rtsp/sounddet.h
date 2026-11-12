#pragma once
/* Loud-sound detection on the microphone (event "sound").
 * Settings: sound.enabled (off by default), sound.threshold (dBFS, -60..-5,
 * default -30), sound.hold (s the event stays on after the last loud period).
 * Ignores the camera's own speaker (chime, talk-back) and 1.5 s after it. */
#include <stddef.h>
#include <stdint.h>
void sounddet_init(void);
/* Called from the audio capture thread with every period (16-bit, 8 kHz). */
void sounddet_feed(const int16_t *pcm, size_t n);
int  sounddet_level_db(void);          /* last period, dBFS (-99 = silence) */

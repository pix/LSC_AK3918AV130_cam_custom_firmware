#pragma once
/* Speaker output: a sample queue drained by audio.c's playback loop, the
 * speaker amplifier GPIO (on only while sound plays), volume, and the
 * built-in sounds. 16-bit mono PCM at 8 kHz. */
#include <stddef.h>
#include <stdint.h>

#define SPEAKER_RATE     8000
#define SPEAKER_AMP_GPIO 69          /* _ht_hw_settings.ini: speak_en_gpio=69 (owned by ak_pcm driver) */

int    speaker_init(void);
/* Amplifier switch, provided by audio.c (speaker-enable ioctl). */
void   speaker_set_amp_fn(int (*fn)(int on));
/* Queue samples; returns samples accepted (drops what doesn't fit). */
size_t speaker_queue(const int16_t *pcm, size_t n);
/* Live talk-back: same queue, with a jitter buffer (playback starts once
 * ~150 ms is buffered, restarts the same way after an underrun) and a
 * latency cap that drops the oldest samples, never the newest. */
size_t speaker_queue_stream(const int16_t *pcm, size_t n);
void   speaker_stop(void);           /* flush the queue */
int    speaker_busy(void);
int    speaker_playing(void);        /* locked: audio is queued and playing */
size_t speaker_queued(void);          /* samples waiting */
/* Playback loop side: fills out[n] (queued audio, else silence) and drives
 * the amplifier. Returns number of real (non-silence) samples. */
size_t speaker_fill(int16_t *out, size_t n);
void   speaker_set_volume(int percent);   /* 0..100 */
int    speaker_get_volume(void);
int    speaker_ready(void);          /* 1 once playback write() works */
void   speaker_set_ready(int ok);

/* Built-in sounds by name: "chime", "custom" (user WAV baked into this
 * build, if any), "tone" (440 Hz test). 0 ok, -1 unknown/empty. */
int    speaker_play_builtin(const char *name);
/* Decode a WAV (PCM 8/16-bit, mono/stereo, any rate) and queue it,
 * resampled to 8 kHz mono. Returns 0 or -1 with *err. Untrusted input. */
int    speaker_play_wav(const uint8_t *data, size_t len, const char **err);

#pragma once
#include <stdint.h>
#include <stddef.h>

/* Acoustic echo cancellation for the microphone (audio.aec).
 *
 * The speaker signal comes back through the PCM loopback tap
 * (/dev/pcmC0D0l). SpeexDSP's fixed-point MDF (partitioned-block
 * frequency-domain adaptive filter, with double-talk handling) removes the
 * linear echo, then SpeexDSP's preprocessor suppresses the residual echo.
 *
 * Alignment: loopback and capture run on the same codec clock but are read
 * by two threads, in different period sizes. Each read is timestamped; the
 * least-delayed reads over a sliding window give each stream's
 * sample-index-to-time offset, so the reference sample played at the time a
 * microphone sample was captured is known to a few samples. The microphone
 * is delayed by a fixed AEC_HOLD so the (coarser) loopback periods have
 * arrived before a microphone block is processed.
 *
 * Thread use: aec_ref_push() from the loopback reader, aec_process() from
 * the capture thread, everything else from anywhere. */

#define AEC_HOLD 768                /* samples of microphone delay (96 ms) */

void aec_register_params(void);     /* audio.aec, aec_state, aec_delay, ... */
int  aec_saved_on(void);            /* audio.aec saved as on (before settings load) */
int  aec_enabled(void);

/* Loopback reader: n reference samples, t_us = mono_us() taken right after
 * the read that returned them. */
void aec_ref_push(const int16_t *s, size_t n, int64_t t_us);
void aec_ref_lost(void);            /* loopback stopped working */
void aec_ref_attach(void);          /* the loopback reader is running */

/* Capture thread: n microphone samples just read at t_us. Replaced in place
 * by the echo-cancelled signal, AEC_HOLD samples later. Returns 1 when the
 * canceller ran, 0 when audio was passed through untouched (off, no
 * reference yet). */
int  aec_process(int16_t *mic, size_t n, int64_t t_us);

/* 1 while the canceller is running on a live reference: callers can then
 * leave the microphone open while the speaker plays (full duplex). */
int  aec_active(void);

/* Debug: records `secs` seconds of (raw mic, aligned reference, output) as
 * interleaved 3-channel s16le. Blocks until done. Returns a malloc'd buffer
 * (caller frees) and its size, or NULL. */
void *aec_dump(int secs, size_t *len);
/* Debug: loopback as read + raw microphone: u32 nref, u32 nmic, samples. */
void *aec_dump_raw(int secs, size_t *len);

/* JSON status (offset, late reference count, ERLE estimate, ...). */
int  aec_status_json(char *out, size_t cap);

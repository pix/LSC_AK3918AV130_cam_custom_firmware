/* ak_rtsp build config for the vendored SpeexDSP subset (echo canceller +
 * preprocessor only). Fixed point: the ARM926EJ-S has no FPU. */
#ifndef SPEEXDSP_AK_CONFIG_H
#define SPEEXDSP_AK_CONFIG_H
#define FIXED_POINT 1
#define USE_KISS_FFT 1
#define EXPORT
#define HAVE_STDINT_H 1
#endif

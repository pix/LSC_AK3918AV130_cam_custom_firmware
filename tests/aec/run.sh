#!/bin/sh
# Host simulation of the firmware echo canceller (aec.c + vendored SpeexDSP).
set -e
cd "$(dirname "$0")"
F=../../firmware/src/ak_rtsp; [ -d "$F" ] || F=../../src/ak_rtsp    # this repo or the public fork layout
S=$F/third_party/speexdsp
cc -O2 -Wall $XCFLAGS -DAEC_HOST_TEST -DHAVE_CONFIG_H -I$F -I$S -o /tmp/test_aec_$$ test_aec.c $F/aec.c \
   $S/mdf.c $S/preprocess.c $S/fftwrap.c $S/filterbank.c $S/kiss_fft.c $S/kiss_fftr.c -lm -lpthread
for d in ${DELAYS:-8 24 200}; do /tmp/test_aec_$$ $d ${GAIN:-0.8}; done
[ -n "$DELAYS" ] || /tmp/test_aec_$$ 24 2.0
rm -f /tmp/test_aec_$$

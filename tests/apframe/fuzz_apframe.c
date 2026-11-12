#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include "apframe.h"
int LLVMFuzzerTestOneInput(const uint8_t *d, size_t n) {
  ap_rx_t rx; ap_cfg_t c; memset(&c,0,sizeof c); strcpy(c.ssid,"LSC-Camera-CA01"); c.channel=6; c.beacon_int=100;
  if (ap_parse(d,n,&rx)==0) { uint8_t o[512]; (void)ap_probe_matches(&c,&rx); ap_build_probe_resp(&c,rx.sa,o,sizeof o); ap_build_assoc_resp(&c,rx.sa,0,0,1,o,sizeof o); }
  return 0; }

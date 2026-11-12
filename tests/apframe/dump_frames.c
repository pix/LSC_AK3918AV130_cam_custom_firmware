/* Writes our built frames as a pcap (LINKTYPE_IEEE802_11 = 105) for tshark. */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include "apframe.h"
static void rec(FILE *f, const uint8_t *d, uint32_t n){ uint32_t h[4]={0,0,n,n}; fwrite(h,4,4,f); fwrite(d,1,n,f); }
int main(int argc,char**argv){
  ap_cfg_t c; memset(&c,0,sizeof c); uint8_t me[6]={0x02,0x11,0x22,0x33,0x44,0x55}, sta[6]={0xaa,0xbb,0xcc,0xdd,0xee,0xff};
  memcpy(c.bssid,me,6); strcpy(c.ssid,"LSC-Camera-CA01"); c.channel=6; c.beacon_int=100;
  FILE *f=fopen(argv[1],"wb"); uint32_t gh[6]={0xa1b2c3d4,0x00040002,0,0,65535,105}; fwrite(gh,4,6,f);
  uint8_t b[512],t[64]; size_t h=ap_build_beacon_head(&c,b,sizeof b), tl=ap_build_beacon_tail(&c,t,sizeof t);
  uint8_t tim[]={5,4,0,1,0,0}; memcpy(b+h,tim,6); memcpy(b+h+6,t,tl); rec(f,b,(uint32_t)(h+6+tl));  /* as mac80211 would assemble it */
  rec(f,b,(uint32_t)ap_build_probe_resp(&c,sta,b,sizeof b));
  rec(f,b,(uint32_t)ap_build_auth_resp(&c,sta,0,b,sizeof b));
  rec(f,b,(uint32_t)ap_build_assoc_resp(&c,sta,0,0,1,b,sizeof b));
  rec(f,b,(uint32_t)ap_build_deauth(&c,sta,3,b,sizeof b));
  fclose(f); return 0; }

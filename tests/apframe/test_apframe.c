#include <stdio.h>
#include <string.h>
#include "apframe.h"
static int fails;
#define CHECK(c,n) do{ if(c) printf("PASS %s\n",n); else {printf("FAIL %s\n",n); fails++;} }while(0)
static const uint8_t ME[6]={0x02,0x11,0x22,0x33,0x44,0x55}, STA[6]={0xaa,0xbb,0xcc,0xdd,0xee,0xff};
static size_t mk_hdr(uint8_t *f, uint8_t fc){ memset(f,0,24); f[0]=fc; memset(f+4,0xff,6); memcpy(f+10,STA,6); memset(f+16,0xff,6); return 24; }
int main(void){
  ap_cfg_t c; memset(&c,0,sizeof c); memcpy(c.bssid,ME,6); strcpy(c.ssid,"LSC-Camera-CA01"); c.channel=6; c.beacon_int=100;
  uint8_t f[512], o[512]; ap_rx_t rx; size_t n;
  /* wildcard probe */
  n=mk_hdr(f,FC_PROBE_REQ); f[n++]=0; f[n++]=0; f[n++]=1; f[n++]=2; f[n++]=0x82; f[n++]=0x84;
  CHECK(ap_parse(f,n,&rx)==0 && rx.subtype_fc==FC_PROBE_REQ && ap_probe_matches(&c,&rx),"wildcard probe answered");
  CHECK(rx.n_rates==2 && !memcmp(rx.sa,STA,6),"probe rates+sa parsed");
  /* directed probe, other SSID */
  n=mk_hdr(f,FC_PROBE_REQ); f[n++]=0; f[n++]=4; memcpy(f+n,"Home",4); n+=4;
  CHECK(ap_parse(f,n,&rx)==0 && !ap_probe_matches(&c,&rx),"probe for other SSID ignored");
  n=mk_hdr(f,FC_PROBE_REQ); f[n++]=0; f[n++]=15; memcpy(f+n,"LSC-Camera-CA01",15); n+=15;
  CHECK(ap_parse(f,n,&rx)==0 && ap_probe_matches(&c,&rx),"probe for our SSID answered");
  /* malformed */
  n=mk_hdr(f,FC_PROBE_REQ); f[n++]=0; f[n++]=200; f[n++]='x';
  CHECK(ap_parse(f,n,&rx)==-1,"truncated IE rejected");
  n=mk_hdr(f,FC_PROBE_REQ); f[n++]=0; f[n++]=33; memset(f+n,'a',33); n+=33;
  CHECK(ap_parse(f,n,&rx)==-1,"33-byte SSID rejected");
  CHECK(ap_parse(f,23,&rx)==-1,"short frame rejected");
  n=mk_hdr(f,FC_AUTH); CHECK(ap_parse(f,n+5,&rx)==-1,"short auth rejected");
  n=mk_hdr(f,FC_AUTH); f[1]=0x40; memset(f+n,0,6); CHECK(ap_parse(f,n+6,&rx)==-1,"protected frame rejected");
  n=mk_hdr(f,0x08); CHECK(ap_parse(f,n+10,&rx)==-1,"data frame rejected");
  /* auth */
  n=mk_hdr(f,FC_AUTH); f[n++]=0; f[n++]=0; f[n++]=1; f[n++]=0; f[n++]=0; f[n++]=0;
  CHECK(ap_parse(f,n,&rx)==0 && rx.auth_alg==0 && rx.auth_seq==1,"auth parsed");
  /* assoc / reassoc with rates + ext rates, rate cap */
  n=mk_hdr(f,FC_ASSOC_REQ); f[n++]=0x21; f[n++]=0x04; f[n++]=10; f[n++]=0; f[n++]=1; f[n++]=8; memset(f+n,0x8c,8); n+=8; f[n++]=50; f[n++]=12; memset(f+n,0x6c,12); n+=12;
  CHECK(ap_parse(f,n,&rx)==0 && rx.listen_int==10 && rx.n_rates==AP_RATES_MAX,"assoc parsed, rates capped at 16");
  n=mk_hdr(f,FC_REASSOC_REQ); memset(f+n,0,10); n+=10; f[n++]=1; f[n++]=1; f[n++]=0x82;
  CHECK(ap_parse(f,n,&rx)==0 && rx.n_rates==1,"reassoc parsed");
  /* builders */
  size_t h=ap_build_beacon_head(&c,o,sizeof o);
  CHECK(h==24+12+2+15+2+8+2+1 && o[0]==0x80 && !memcmp(o+10,ME,6),"beacon head layout");
  CHECK(ap_build_beacon_tail(&c,o,sizeof o)==3+2+4,"beacon tail layout");
  size_t pr=ap_build_probe_resp(&c,STA,o,sizeof o);
  CHECK(pr>0 && o[0]==0x50 && !memcmp(o+4,STA,6),"probe resp to requester");
  CHECK(ap_parse(o,pr,&rx)==-1,"own probe resp not parsed as request");
  size_t ar=ap_build_auth_resp(&c,STA,0,o,sizeof o);
  CHECK(ar==30 && o[0]==0xb0 && o[26]==2,"auth resp seq 2");
  size_t as=ap_build_assoc_resp(&c,STA,0,0,1,o,sizeof o);
  CHECK(as>0 && o[0]==0x10 && o[28]==1 && o[29]==0xc0,"assoc resp AID|0xc000");
  CHECK(ap_build_probe_resp(&c,STA,o,20)==0,"builder refuses small buffer");
  printf(fails?"\n%d FAILURES\n":"\nALL PASS\n",fails); return fails!=0;
}

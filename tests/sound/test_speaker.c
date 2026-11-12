#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "speaker.h"
#include <unistd.h>
static int fails;
#define CHECK(c,n) do{ if(c) printf("PASS %s\n",n); else {printf("FAIL %s\n",n); fails++;} }while(0)
static size_t drain(int *peak){ int16_t b[256]; size_t tot=0,k; *peak=0; while((k=speaker_fill(b,256))>0){ for(size_t i=0;i<k;i++){int a=abs(b[i]); if(a>*peak)*peak=a;} tot+=k; } return tot; }
static unsigned char *rd(const char *p, size_t *n){ FILE*f=fopen(p,"rb"); if(!f) return NULL; static unsigned char b[2000000]; *n=fread(b,1,sizeof b,f); fclose(f); return b; }
int main(int argc, char **argv){
  const char *e; size_t n; int peak; speaker_set_volume(100);
  CHECK(speaker_play_builtin("chime")==0,"chime queued"); size_t c=drain(&peak); CHECK(c==16000 && peak>5000 && peak<=32767,"chime 2 s, audible, no clipping overflow");
  CHECK(speaker_play_builtin("nope")==-1,"unknown sound rejected");
  unsigned char *w=rd(argv[1],&n); CHECK(w && speaker_play_wav(w,n,&e)==0,"user WAV (8-bit 22 kHz) decoded");
  c=drain(&peak); CHECK(c>9900 && c<10200 && peak>1000,"user WAV ~1.26 s at 8 kHz");
  w=rd(argv[2],&n); CHECK(w && speaker_play_wav(w,n,&e)==0,"44.1 kHz stereo 16-bit decoded"); c=drain(&peak); CHECK(c>15900 && c<16100,"2 s stereo -> 16000 mono samples");
  CHECK(speaker_play_wav((const unsigned char*)"RIFF0000WAVE",12,&e)==-1,"no chunks rejected");
  unsigned char bad[64]={0}; memcpy(bad,"RIFF",4); memcpy(bad+8,"WAVEfmt ",8); bad[16]=16; bad[20]=3; /* float fmt */ memcpy(bad+36,"data",4); bad[40]=8;
  CHECK(speaker_play_wav(bad,52,&e)==-1 && strstr(e,"PCM"),"non-PCM rejected");
  w=rd(argv[1],&n); CHECK(speaker_play_wav(w,n/3,&e)==0,"truncated WAV handled"); drain(&peak);
  speaker_set_volume(50); speaker_play_builtin("tone"); drain(&peak); CHECK(peak>5500 && peak<6100,"volume 50% halves amplitude");
  /* talk-back jitter buffer */
  { int16_t chunk[341], out[256]; for(int i=0;i<341;i++) chunk[i]=1000; speaker_set_volume(100);
    speaker_queue_stream(chunk,341); CHECK(speaker_fill(out,256)==0,"talk-back: holds until 150 ms buffered");
    for(int i=0;i<3;i++) speaker_queue_stream(chunk,341);          /* 1364 samples, still < 1200? */
    size_t k=speaker_fill(out,256); CHECK(k==256,"talk-back: plays once >=150 ms buffered");
    drain(&peak); CHECK(speaker_fill(out,256)==0,"talk-back: underrun -> silence");
    speaker_queue_stream(chunk,100); CHECK(speaker_fill(out,256)==0,"talk-back: re-buffers after underrun");
    usleep(150000); CHECK(speaker_fill(out,256)==100,"talk-back: short tail plays after the sender pauses");
    speaker_stop(); for(int i=0;i<20;i++) speaker_queue_stream(chunk,341);   /* 6820 samples sent at once */
    CHECK(speaker_queued()<=4800 && speaker_queued()>=2400,"talk-back: latency capped by dropping oldest");
    speaker_stop(); }
  printf(fails?"\n%d FAILURES\n":"\nALL PASS\n",fails); return fails!=0; }

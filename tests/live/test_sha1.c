#include <stdio.h>
#include <string.h>
#include "sha1.h"
static int fails;
static void chk(const char *in, size_t n, const char *want){ uint8_t d[20]; char h[41]; sha1(in,n,d); for(int i=0;i<20;i++) sprintf(h+2*i,"%02x",d[i]); int ok=!strcmp(h,want); printf("%s sha1(%zu bytes)\n", ok?"PASS":"FAIL", n); if(!ok){printf("  %s\n  %s\n",h,want);fails++;} }
int main(void){
  chk("",0,"da39a3ee5e6b4b0d3255bfef95601890afd80709");
  chk("abc",3,"a9993e364706816aba3e25717850c26c9cd0d89d");
  const char *l="abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq"; chk(l,strlen(l),"84983e441c3bd26ebaae4aa1f95129e5e54670f1");
  static char m[1000000]; memset(m,'a',sizeof m); chk(m,sizeof m,"34aa973cd4c4daa4f61eeb2bdbad27316534016f");
  char b[64]; base64_encode((const uint8_t*)"foobar",6,b); printf("%s base64 foobar=%s\n", !strcmp(b,"Zm9vYmFy")?"PASS":"FAIL", b); if(strcmp(b,"Zm9vYmFy"))fails++;
  base64_encode((const uint8_t*)"fo",2,b); printf("%s base64 fo=%s\n", !strcmp(b,"Zm8=")?"PASS":"FAIL", b); if(strcmp(b,"Zm8="))fails++;
  char a[29]; ws_accept_key("dGhlIHNhbXBsZSBub25jZQ==",a); printf("%s RFC6455 accept=%s\n", !strcmp(a,"s3pPLMBiTxaQ9kYGzzhZRbK+xOo=")?"PASS":"FAIL", a); if(strcmp(a,"s3pPLMBiTxaQ9kYGzzhZRbK+xOo="))fails++;
  printf(fails?"%d FAILURES\n":"ALL PASS\n",fails); return fails!=0; }

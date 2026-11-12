#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "qrwifi.h"
static int fails;
#define CHECK(c,n) do{ if(c) printf("PASS %s\n",n); else {printf("FAIL %s\n",n); fails++;} }while(0)
static int P(const char *s, char *ss, char *pp, const char **e){ return qrwifi_parse(s, strlen(s), ss, pp, e); }
int main(int argc, char **argv){
  char s[33], p[64]; const char *e;
  CHECK(P("WIFI:T:WPA;S:Example Net 5;P:secretpass;H:false;;",s,p,&e)==0 && !strcmp(s,"Example Net 5") && !strcmp(p,"secretpass"),"Android format");
  CHECK(P("WIFI:S:Home;T:WPA;P:pass1234;;",s,p,&e)==0 && !strcmp(s,"Home"),"field order independent");
  CHECK(P("WIFI:T:WPA;S:a\\;b\\:c\\,d\\\\e;P:p\\;ss\\\\word;;",s,p,&e)==0 && !strcmp(s,"a;b:c,d\\e") && !strcmp(p,"p;ss\\word"),"escapes decoded");
  CHECK(P("WIFI:T:WPA;S:\"Quoted Net\";P:\"pass word 1\";;",s,p,&e)==0 && !strcmp(s,"Quoted Net") && !strcmp(p,"pass word 1"),"quoted values");
  CHECK(P("WIFI:T:nopass;S:Guest;P:;;",s,p,&e)==0 && p[0]==0,"open network");
  CHECK(P("WIFI:S:Guest;;",s,p,&e)==0 && p[0]==0,"no T means open");
  CHECK(P("WIFI:T:WPA;S:Home;P:short;;",s,p,&e)==-1,"7-char password rejected");
  CHECK(P("WIFI:T:WEP;S:Old;P:abcde;;",s,p,&e)==-1 && strstr(e,"WEP"),"WEP rejected with message");
  CHECK(P("WIFI:T:SAE;S:New;P:password1;;",s,p,&e)==-1 && strstr(e,"WPA3"),"WPA3 rejected with message");
  CHECK(P("WIFI:T:WPA;P:password1;;",s,p,&e)==-1,"missing SSID rejected");
  CHECK(P("WIFI:T:WPA;S:Home;P:password1",s,p,&e)==-1,"unterminated field rejected");
  CHECK(P("WIFI:T:WPA;S:123456789012345678901234567890123;P:password1;;",s,p,&e)==-1,"33-byte SSID rejected");
  CHECK(P("WIFI:T:WPA;S:x\\",s,p,&e)==-1,"trailing backslash rejected");
  CHECK(P("https://example.com",s,p,&e)==-1,"non-Wi-Fi QR ignored");
  CHECK(P("WIFI:T:WPA;S:Home;P:password1;X",s,p,&e)==-1,"garbage tail rejected");
  CHECK(qrwifi_parse_file("ssid=Example Net 5\r\npassword=secretpass\n",s,p,&e)==0 && !strcmp(s,"Example Net 5") && !strcmp(p,"secretpass"),"wifi.txt CRLF");
  CHECK(qrwifi_parse_file("password=secretpass\n",s,p,&e)==-1,"wifi.txt without ssid rejected");
  /* images: argv[i] = raw 640x360 gray, name encodes expectation: ok_* or no_* */
  for (int i = 1; i < argc; i++) {
    FILE *f = fopen(argv[i],"rb"); static unsigned char img[640*360];
    if (!f || fread(img,1,sizeof img,f)!=sizeof img) { printf("FAIL read %s\n", argv[i]); fails++; if(f) fclose(f); continue; }
    fclose(f);
    int got = qrwifi_scan_gray(img,640,360,s,p);
    const char *base = strrchr(argv[i],'/'); base = base?base+1:argv[i];
    int want = !strncmp(base,"ok_",3);
    char name[160]; snprintf(name,sizeof name,"image %s -> %s%s", base, got?"decoded ":"none", got?s:"");
    CHECK(got==want && (!got || !strcmp(s,"Example Net 5")), name);
  }
  printf(fails?"\n%d FAILURES\n":"\nALL PASS\n",fails); return fails!=0;
}

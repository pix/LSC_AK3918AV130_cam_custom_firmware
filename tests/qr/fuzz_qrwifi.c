#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include "qrwifi.h"
int LLVMFuzzerTestOneInput(const uint8_t *d, size_t n){ char s[33],p[64]; const char *e;
  char buf[600]; if (n > 590) n = 590; memcpy(buf,"WIFI:",5); memcpy(buf+5,d,n);
  if (qrwifi_parse(buf,n+5,s,p,&e)==0) { if (strlen(s)>32||strlen(p)>63) __builtin_trap(); }
  char t[700]; size_t m = n < 690 ? n : 690; memcpy(t,d,m); t[m]=0; qrwifi_parse_file(t,s,p,&e); return 0; }

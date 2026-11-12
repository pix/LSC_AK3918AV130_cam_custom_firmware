/* Host harness: runs auth + control (8091) + web (WEB_PORT) without hardware. */
#include <stdio.h>
#include <unistd.h>
#include "auth.h"
#include "control.h"
#include "web.h"
#include "resetkey.h"
void stub_set_frame(const char *p);
int main(int argc, char **argv)
{
    if (argc > 1) stub_set_frame(argv[1]);
    control_start();
    auth_init();
    control_load_settings();
    web_start();
    printf("host harness up\n");
    for (;;) pause();
}

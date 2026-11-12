#pragma once
/* Development terminal (root shell over WebSocket), setting dev.terminal. */
#include "conn.h"
void term_init(void);                     /* register dev.terminal */
int  term_enabled(void);
int  term_token_new(char out[33]);        /* -1 when disabled */
int  term_token_take(const char *tok);    /* 1 = valid (single use) */
int  term_busy(void);                     /* a session is open */
/* after the 101 response: 0 = handed to a session thread, -1 refused, -2 busy */
int  term_ws_start(conn_t *cn, const char *peer);

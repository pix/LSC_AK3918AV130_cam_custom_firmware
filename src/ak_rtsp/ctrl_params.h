#pragma once
/* Helpers to define registered settings (see control.h) with little code.
 * Each macro defines <fn>_get / <fn>_set for a static variable. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "control.h"

/* integer in [lo, hi] */
#define CTRL_INT(fn, var, lo, hi)                                                        \
    static int fn##_get(char *o, size_t c) { snprintf(o, c, "%d", (int)(var)); return 0; } \
    static int fn##_set(const char *v, const char **e) {                                  \
        char *end; long x = strtol(v, &end, 10);                                          \
        if (!*v || *end || x < (lo) || x > (hi)) { *e = "must be a number from " #lo " to " #hi; return -1; } \
        (var) = (int)x; return 0; }

/* on / off */
#define CTRL_BOOL(fn, var)                                                               \
    static int fn##_get(char *o, size_t c) { snprintf(o, c, "%s", (var) ? "on" : "off"); return 0; } \
    static int fn##_set(const char *v, const char **e) {                                  \
        if (!strcmp(v, "on") || !strcmp(v, "1") || !strcmp(v, "true")) (var) = 1;       \
        else if (!strcmp(v, "off") || !strcmp(v, "0") || !strcmp(v, "false")) (var) = 0; \
        else { *e = "must be on or off"; return -1; } return 0; }

/* string buffer, validated by ok(const char*) (may be NULL = any printable) */
#define CTRL_STR(fn, buf, ok, errmsg)                                                    \
    static int fn##_get(char *o, size_t c) { snprintf(o, c, "%s", buf); return 0; }     \
    static int fn##_set(const char *v, const char **e) {                                  \
        int (*okf)(const char *) = ok;                                                   \
        if (strlen(v) >= sizeof(buf) || (okf && !okf(v))) { *e = errmsg; return -1; }    \
        snprintf(buf, sizeof(buf), "%s", v); return 0; }

#define CTRL_REG(name, fn, flags) do { ctrl_param_t p_ = { name, fn##_get, fn##_set, flags }; control_register(&p_); } while (0)

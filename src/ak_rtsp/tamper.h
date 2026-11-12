#pragma once
/* Tamper detection (lens covered, camera moved). Setting tamper.enabled. */
#include <stddef.h>
void tamper_init(void);     /* register settings */
void tamper_start(void);    /* 1 Hz thread on the sub stream */
/* {"enabled":..,"tamper":..,"state":"none|covered|moved","corr":..,"texture":..,"spread":..} */
int  tamper_status_json(char *out, size_t cap);

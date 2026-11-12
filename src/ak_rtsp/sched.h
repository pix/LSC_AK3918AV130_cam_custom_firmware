#pragma once
/* Weekly schedules: sched.record, sched.events, sched.privacy (see sched.c). */
#include <stddef.h>
void sched_init(void);          /* register settings */
void sched_start(void);         /* applies privacy mode at hour changes */
int  sched_privacy(void);       /* privacy mode active now */
int  sched_record_ok(void);     /* recording allowed now (and no privacy) */
int  sched_events_ok(void);     /* detections reported now (and no privacy) */
/* {"privacy":..,"record":..,"events":..,"hour":0..167} */
int  sched_status_json(char *out, size_t cap);

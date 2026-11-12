#pragma once
/* Events (motion, sound): fan out to MQTT (state + snapshot), an optional
 * outgoing HTTP webhook and the SD recorder. Delivery runs in a worker
 * thread so detectors never block on the network.
 * Settings: events.webhook (http://host[:port]/path, empty = off),
 *           events.snapshot (on/off: publish a JPEG to MQTT on motion). */
#include <stddef.h>
void events_init(void);                 /* register settings */
void events_start(void);                /* worker thread; installs the MQTT command handler */
void events_motion(int on);
void events_sound(int on, int level_db);
void events_tamper(int state);        /* 0 cleared, 1 lens covered, 2 camera moved */
/* Recorder hook: called (from the worker) on every motion/sound start/stop. */
void events_set_hook(void (*fn)(const char *kind, int on));
/* {"motion":..,"sound":..,"last_motion":"..","last_sound":"..","webhook":"ok|error ..","mqtt":{...}} */
int  events_status_json(char *out, size_t cap);
int  events_motion_active(void);

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
/* md.line: on = a tracked object crossed (sent for every crossing; dir 1 toward
 * the left side of the line seen from its start, 2 toward the right; id = track);
 * off once no crossing for md.hold seconds. MQTT "line" + "line_direction". */
void events_line(int on, int dir, int id);
/* md.loiter: someone stays in the zone (0 = watched area) longer than set. */
void events_loiter(int on, int zone);
/* Recorder hook: called (from the worker) on every motion/sound start/stop. */
void events_set_hook(void (*fn)(const char *kind, int on));
/* {"motion":..,"sound":..,"last_motion":"..","last_sound":"..","webhook":"ok|error ..","mqtt":{...}} */
int  events_status_json(char *out, size_t cap);
int  events_motion_active(void);

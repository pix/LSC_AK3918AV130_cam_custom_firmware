#pragma once
/* Status LED, time (NTP + time zone) and health data for the web page. */
#include <stddef.h>

#define TIME_DEFAULT_NTP "pool.ntp.org"
#define TIME_DEFAULT_TZ  "Europe/Paris"

/* Status LED ("state_led" in the DTB, /sys/class/leds). */
int  sys_led_set(int on);              /* resting state (led.status) */
int  sys_led_get(void);
/* led.feedback = off|motion|recording|both: blink fast while motion is
 * active, slowly while a recording file is written. Starts the thread. */
void sys_led_start(void);

/* NTP server: hostname or IPv4, [A-Za-z0-9.-] only (it is passed to ntpd).
 * Returns 0, or -1 if invalid. Restarts ntpd. */
int         sys_time_set_server(const char *host);
const char *sys_time_server(void);

/* Time zone by name from the built-in list (see sys_tz_list_json).
 * Applies to this process (web page clock, future overlay). */
int         sys_time_set_tz(const char *name);
const char *sys_time_tz(void);
int         sys_tz_list_json(char *out, size_t cap);

/* Starts the ntpd keeper thread (waits for the network, restarts ntpd if
 * it dies). Call once at startup, after settings are restored. */
void sys_time_start(void);
int  sys_time_synced(void);
/* Call right before system()/popen(): marks every open descriptor above
 * stderr close-on-exec, so daemons we start (ntpd, udhcpc, wpa_cli) do not
 * inherit the camera's devices and listening sockets. */
void sys_cloexec_all(void);

/* {"uptime_s":..,"load":..,"mem_total_kb":..,"mem_avail_kb":..,"temp_c":..,
 *  "time":{...},"rtsp_client":..} — wifi details come from netcfg. */
int  sys_health_json(char *out, size_t cap);

/* Chip temperature in tenths of a degree C, or -9999 if unknown. */
int  sys_temp_c10(void);

/* Camera name (setting device.name; safe for JSON: [A-Za-z0-9 _.-]). */
const char *sys_device_name(void);
void sys_register_params(void);
const char *sys_device_name_cn(void);

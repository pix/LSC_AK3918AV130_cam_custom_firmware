#pragma once
/* Minimal MQTT 3.1.1 client (QoS 0) with Home Assistant discovery.
 *
 * Settings (registered, persisted): mqtt.enabled, mqtt.host, mqtt.port,
 * mqtt.user, mqtt.pass (secret), mqtt.topic (base topic, default
 * "lsc_camera/<mac>"), mqtt.discovery (HA discovery under "homeassistant/").
 *
 * Topics under <base>: availability (online/offline, retained, LWT),
 * motion, sound (ON/OFF), temperature, rssi, ir, night, led, volume (state),
 * snapshot (JPEG on motion), and commands on <base>/cmd/<name>:
 * sound (chime|tone|custom), ir (auto|on|off), night (auto|day|night),
 * led (on|off), volume (0-100), motion_detection (on|off), recording (...). */
#include <stddef.h>

void mqtt_init(void);                    /* register settings (before loading them) */
void mqtt_start(void);                   /* connection thread */
int  mqtt_connected(void);
/* Publish <base>/<sub>; 0 sent, -1 not connected/failed. */
int  mqtt_publish(const char *sub, const void *payload, size_t len, int retain);
int  mqtt_publish_str(const char *sub, const char *payload, int retain);
/* {"connected":..,"error":".."} for the web page */
int  mqtt_status_json(char *out, size_t cap);
/* Commands from MQTT are handed to this (set by events.c). */
void mqtt_set_command_handler(void (*fn)(const char *name, const char *payload));
/* Re-publish states soon (after a setting changed). */
void mqtt_kick(void);
/* Drop and re-establish the connection now (certificates changed). */
void mqtt_reconnect(void);
int  mqtt_open_tcp(const char *host, int port, char *err, size_t errcap);

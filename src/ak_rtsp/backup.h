#pragma once
/* Settings backup / restore (tar of whitelisted /etc/config files, see backup.c). */
#include <stddef.h>
#include <stdint.h>
/* malloc'd tar in *out (caller frees). with_wifi: include the Wi-Fi network. */
int backup_build(uint8_t **out, size_t *len, int with_wifi, const char *device, const char *version);
/* Validates the whole archive, then writes it. 0 ok (caller reboots). */
int backup_restore(const uint8_t *tar, size_t len, int *restored, char *err, size_t errcap);

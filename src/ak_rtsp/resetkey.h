#pragma once
/* Credential / settings reset paths (physical access only):
 *
 *  0. Reset button pressed 3 times within 3 s -> look for a Wi-Fi QR code, 3 min.
 *  1. Reset button held >= RESET_HOLD_S seconds -> auth_reset()
 *     (login back to admin/admin). The button is the gpio-keys device
 *     ("reset_key_detect" in the DTB, loaded by stock main.sh as
 *     ak_gpio_keys.ko); stock anyka_ipc used to watch it, so after the
 *     de-Tuyafication nothing did until now.
 *
 *  2. At boot, files on the SD card root:
 *       reset_auth      -> auth_reset()
 *       reset_settings  -> delete saved image/night settings
 *       wifi.txt        -> join that network (ssid=/password= lines, with
 *                          rollback); renamed wifi.txt.done, password blanked
 *     Each file is renamed to <name>.done afterwards so it fires once. */

#define RESET_HOLD_S 10

/* Call after auth_init() and BEFORE control_load_settings(). */
void resetkey_check_sd(void);
/* Starts the button watcher thread. */
int  resetkey_start(void);

/* SPDX-License-Identifier: 0BSD OR GPL-3.0-or-later */
/*
 * The settings a person chooses on the hotspot page: which mesh to join (name and password),
 * the hotspot's own name and password, and the page's root password (as a hash, auth.c).
 * Kept in NVS. Nothing about the network is built into the firmware: a board with nothing
 * saved is unconfigured and boots into setup mode. Passwords are never logged and never sent
 * back out.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "mesh.h"

#define SETTINGS_SSID_MAX 32
#define SETTINGS_AP_PW_MIN 8    /* WPA2-PSK passphrase: 8 to 63 characters (802.11i) */
#define SETTINGS_AP_PW_MAX 63
#define SETTINGS_ROOT_HASH_LEN 52   /* auth.c: iterations, salt, PBKDF2 key */

struct hop_settings
{
    char mesh_id[MESH_ID_MAX + 1];
    char mesh_password[MESH_PASSWORD_MAX + 1];   /* empty: open mesh */
    char ap_ssid[SETTINGS_SSID_MAX + 1];
    char ap_password[SETTINGS_AP_PW_MAX + 1];
    uint8_t root_hash[SETTINGS_ROOT_HASH_LEN];
    bool configured; /* all five were saved by the setup page; false: setup mode */
};

/* Fills s from NVS. Unconfigured: the mesh name suggestion from Kconfig, the default
 * hotspot name, empty passwords. */
void settings_load(struct hop_settings *s);

/* Checks s; on a problem returns false and a short reason for the person in why. */
bool settings_valid(const struct hop_settings *s, const char **why);

/* Writes all five values to NVS. */
esp_err_t settings_save(const struct hop_settings *s);

/* Writes only the root password hash (a password change, no restart). */
esp_err_t settings_save_root_hash(const uint8_t hash[SETTINGS_ROOT_HASH_LEN]);

/* Forgets everything saved; the next boot is in setup mode. */
esp_err_t settings_erase(void);

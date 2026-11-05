/* SPDX-License-Identifier: 0BSD OR GPL-3.0-or-later */
/*
 * The settings a person changes on the hotspot page: which mesh to join (name and password)
 * and the hotspot's own name and password. Kept in NVS; until the page saves anything, the
 * build's Kconfig values are used. Passwords are never logged and never sent back out.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"
#include "mesh.h"

#define SETTINGS_SSID_MAX 32
#define SETTINGS_AP_PW_MIN 8    /* WPA2-PSK passphrase: 8 to 63 characters (802.11i) */
#define SETTINGS_AP_PW_MAX 63

struct hop_settings
{
    char mesh_id[MESH_ID_MAX + 1];
    char mesh_password[MESH_PASSWORD_MAX + 1];   /* empty: open mesh */
    char ap_ssid[SETTINGS_SSID_MAX + 1];
    char ap_password[SETTINGS_AP_PW_MAX + 1];
    bool from_nvs;   /* false: these are the build defaults */
};

/* Fills s from NVS, or from the build defaults for anything not saved. */
void settings_load(struct hop_settings *s);

/* Checks s; on a problem returns false and a short reason for the person in why. */
bool settings_valid(const struct hop_settings *s, const char **why);

/* Writes all four values to NVS. */
esp_err_t settings_save(const struct hop_settings *s);

/* Forgets everything saved; the next boot uses the build defaults. */
esp_err_t settings_erase(void);

/* SPDX-License-Identifier: 0BSD OR GPL-3.0-or-later */
#include "settings.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_mac.h"
#include "nvs.h"
#include "sdkconfig.h"

static const char *TAG = "settings";
#define NS "hopcfg"

static void get_str(nvs_handle_t h, const char *key, char *out, size_t size)
{
    size_t len = size;
    if (nvs_get_str(h, key, out, &len) != ESP_OK)
    {
        return;   /* keep the default already in out */
    }
}

void settings_load(struct hop_settings *s)
{
    memset(s, 0, sizeof(*s));
    strlcpy(s->mesh_id, CONFIG_HOP_MESH_ID, sizeof(s->mesh_id));
    strlcpy(s->mesh_password, CONFIG_HOP_MESH_PASSWORD, sizeof(s->mesh_password));
    if (CONFIG_HOP_AP_SSID[0] != '\0')
    {
        strlcpy(s->ap_ssid, CONFIG_HOP_AP_SSID, sizeof(s->ap_ssid));
    }
    else
    {
        /* Default hotspot name: Meshpoint- and the last two bytes of the board's MAC, so two
         * boards in one room are told apart. */
        uint8_t mac[6] = { 0 };
        esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
        snprintf(s->ap_ssid, sizeof(s->ap_ssid), "Meshpoint-%02X%02X", mac[4], mac[5]);
    }
    strlcpy(s->ap_password, CONFIG_HOP_AP_PASSWORD, sizeof(s->ap_password));

    nvs_handle_t h;
    if (nvs_open(NS, NVS_READONLY, &h) != ESP_OK)
    {
        ESP_LOGI(TAG, "No saved settings: using the build defaults");
        return;
    }
    s->from_nvs = true;
    get_str(h, "mesh_id", s->mesh_id, sizeof(s->mesh_id));
    get_str(h, "mesh_pw", s->mesh_password, sizeof(s->mesh_password));
    get_str(h, "ap_ssid", s->ap_ssid, sizeof(s->ap_ssid));
    get_str(h, "ap_pw", s->ap_password, sizeof(s->ap_password));
    nvs_close(h);
    ESP_LOGI(TAG, "Saved settings loaded");
}

static bool printable(const char *s)
{
    for (; *s; s++)
    {
        if ((unsigned char)*s < 0x20 || (unsigned char)*s == 0x7f)
        {
            return false;
        }
    }
    return true;
}

bool settings_valid(const struct hop_settings *s, const char **why)
{
    size_t id = strlen(s->mesh_id), mpw = strlen(s->mesh_password);
    size_t ssid = strlen(s->ap_ssid), apw = strlen(s->ap_password);
    if (id == 0 || id > MESH_ID_MAX || !printable(s->mesh_id))
    {
        *why = "Mesh name must be 1 to 32 characters";
        return false;
    }
    /* OpenWrt refuses an SAE key shorter than 8 characters, so a shorter one could never
     * match an OpenMANET node. Empty means an open mesh. */
    if (mpw != 0 && (mpw < 8 || mpw > MESH_PASSWORD_MAX || !printable(s->mesh_password)))
    {
        *why = "Mesh password must be 8 to 63 characters";
        return false;
    }
    if (ssid == 0 || ssid > SETTINGS_SSID_MAX || !printable(s->ap_ssid))
    {
        *why = "Hotspot name must be 1 to 32 characters";
        return false;
    }
    /* esp_wifi refuses to start a WPA2 access point outside 8 to 63, and the hotspot is the
     * only way back into this page, so this is checked before saving, never at boot. */
    if (apw < SETTINGS_AP_PW_MIN || apw > SETTINGS_AP_PW_MAX || !printable(s->ap_password))
    {
        *why = "Hotspot password must be 8 to 63 characters";
        return false;
    }
    return true;
}

esp_err_t settings_save(const struct hop_settings *s)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READWRITE, &h);
    if (err != ESP_OK)
    {
        return err;
    }
    err = nvs_set_str(h, "mesh_id", s->mesh_id);
    if (err == ESP_OK) err = nvs_set_str(h, "mesh_pw", s->mesh_password);
    if (err == ESP_OK) err = nvs_set_str(h, "ap_ssid", s->ap_ssid);
    if (err == ESP_OK) err = nvs_set_str(h, "ap_pw", s->ap_password);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    ESP_LOGI(TAG, "Settings saved (mesh \"%s\", hotspot \"%s\")", s->mesh_id, s->ap_ssid);
    return err;
}

esp_err_t settings_erase(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READWRITE, &h);
    if (err != ESP_OK)
    {
        return err;
    }
    err = nvs_erase_all(h);
    if (err == ESP_OK)
    {
        err = nvs_commit(h);
    }
    nvs_close(h);
    ESP_LOGI(TAG, "Saved settings erased: build defaults on the next boot");
    return err;
}

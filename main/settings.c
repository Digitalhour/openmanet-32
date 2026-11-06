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

/* True if the key was there; otherwise out keeps its default. */
static bool get_str(nvs_handle_t h, const char *key, char *out, size_t size)
{
    char tmp[72];
    size_t len = sizeof(tmp);
    if (size > sizeof(tmp) || nvs_get_str(h, key, tmp, &len) != ESP_OK || len > size)
    {
        return false;
    }
    memcpy(out, tmp, len);
    memset(tmp, 0, sizeof(tmp));
    return true;
}

/* The radio settings in NVS: a version byte, then the fields. */
#define RADIO_BLOB_VERSION 1
#define RADIO_BLOB_LEN 8

static void radio_pack(const struct hop_radio *r, uint8_t b[RADIO_BLOB_LEN])
{
    b[0] = RADIO_BLOB_VERSION;
    b[1] = r->s1g_chan;
    b[2] = r->prim_bw_mhz;
    b[3] = (uint8_t)r->prim_idx;
    b[4] = r->beacon_tus & 0xff;
    b[5] = r->beacon_tus >> 8;
    b[6] = r->tx_dbm;
    b[7] = r->ap_chan;
}

static bool radio_unpack(const uint8_t b[RADIO_BLOB_LEN], struct hop_radio *r)
{
    if (b[0] != RADIO_BLOB_VERSION)
    {
        return false;
    }
    r->s1g_chan = b[1];
    r->prim_bw_mhz = b[2];
    r->prim_idx = (int8_t)b[3];
    r->beacon_tus = (uint16_t)(b[4] | b[5] << 8);
    r->tx_dbm = b[6];
    r->ap_chan = b[7];
    return true;
}

void settings_load(struct hop_settings *s)
{
    memset(s, 0, sizeof(*s));
    radio_defaults(&s->radio);
    strlcpy(s->mesh_id, CONFIG_HOP_MESH_ID, sizeof(s->mesh_id));
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

    nvs_handle_t h;
    if (nvs_open(NS, NVS_READONLY, &h) == ESP_OK)
    {
        /* Configured only if all five are there: settings_save writes all five, and
         * settings_erase leaves the namespace itself behind, empty. A board saved before the
         * root password existed has four, so it goes back to setup to choose one. */
        struct hop_settings got = *s;
        bool all = get_str(h, "mesh_id", got.mesh_id, sizeof(got.mesh_id));
        all &= get_str(h, "mesh_pw", got.mesh_password, sizeof(got.mesh_password));
        all &= get_str(h, "ap_ssid", got.ap_ssid, sizeof(got.ap_ssid));
        all &= get_str(h, "ap_pw", got.ap_password, sizeof(got.ap_password));
        size_t hl = sizeof(got.root_hash);
        all &= nvs_get_blob(h, "root_pw", got.root_hash, &hl) == ESP_OK && hl == sizeof(got.root_hash);
        if (all)
        {
            *s = got;
            s->configured = true;
        }
        memset(&got, 0, sizeof(got));
        /* The radio settings are optional: a board saved before they existed keeps the build's
         * defaults, which are what it ran with. Saved ones that no longer check out (say, a
         * channel the table lost) are ignored the same way. */
        uint8_t b[RADIO_BLOB_LEN];
        size_t bl = sizeof(b);
        struct hop_radio r;
        const char *why;
        if (nvs_get_blob(h, "radio", b, &bl) == ESP_OK && bl == sizeof(b) && radio_unpack(b, &r) && radio_valid(&r, &why))
        {
            s->radio = r;
        }
        nvs_close(h);
    }
    ESP_LOGI(TAG, "%s", s->configured ? "Settings loaded" : "Nothing saved: setup mode");
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
    if (err == ESP_OK) err = nvs_set_blob(h, "root_pw", s->root_hash, sizeof(s->root_hash));
    uint8_t b[RADIO_BLOB_LEN];
    radio_pack(&s->radio, b);
    if (err == ESP_OK) err = nvs_set_blob(h, "radio", b, sizeof(b));
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    ESP_LOGI(TAG, "Settings saved (mesh \"%s\", hotspot \"%s\", HaLow channel %u)", s->mesh_id, s->ap_ssid,
             s->radio.s1g_chan);
    return err;
}

esp_err_t settings_save_root_hash(const uint8_t hash[SETTINGS_ROOT_HASH_LEN])
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READWRITE, &h);
    if (err != ESP_OK)
    {
        return err;
    }
    err = nvs_set_blob(h, "root_pw", hash, SETTINGS_ROOT_HASH_LEN);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    ESP_LOGI(TAG, "Root password changed");
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

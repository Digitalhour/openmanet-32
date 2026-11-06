/* SPDX-License-Identifier: 0BSD OR GPL-3.0-or-later */
#include "hotspot.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "cJSON.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"
#include "sdkconfig.h"

#include "auth.h"
#include "batadv.h"
#include "captive_dns.h"
#include "mesh.h"
#include "radio.h"

static const char *TAG = "hotspot";

extern const char page_html_start[] asm("_binary_page_html_start");
extern const char page_html_end[] asm("_binary_page_html_end");
extern const char setup_html_start[] asm("_binary_setup_html_start");
extern const char setup_html_end[] asm("_binary_setup_html_end");
extern const char board_webp_start[] asm("_binary_board_webp_start");
extern const char board_webp_end[] asm("_binary_board_webp_end");
extern const char radio_js_start[] asm("_binary_radio_js_start");
extern const char radio_js_end[] asm("_binary_radio_js_end");

static struct hop_settings s_running;   /* what this boot runs with */
static bool s_setup;                    /* nothing saved yet: open setup hotspot, no mesh */
static char s_ap_name[SETTINGS_SSID_MAX + 1];
static esp_netif_t *s_ap_netif;
static esp_netif_t *s_mesh_netif;
static bool s_ready;

/* DHCP option 114 (RFC 8910): newer phones read the setup page's address straight from the
 * lease. esp_netif keeps the pointer, not a copy (esp_netif.h, ESP_NETIF_CAPTIVEPORTAL_URI),
 * so it must stay alive. */
static const char s_portal_uri[] = "http://" CONFIG_HOP_AP_IP "/";

/* ---------- The access point ---------- */

/* Clients reach the mesh through NAT. Per ESP-IDF's softap_sta example and the
 * openmanet-gateway notes: NAPT goes on the access point's netif (esp_netif sets it on exactly
 * the netif handed over and clears every other), and the uplink must be the default netif so
 * translated packets leave on bat0. No DNS is offered: the mesh has no resolver to point at.
 * NAPT can only be switched on while the AP netif is up (esp_netif_lwip.c,
 * esp_netif_napt_control_api, v5.4.4 line 2599), which happens on WIFI_EVENT_AP_START after
 * esp_wifi_start() has returned, so this runs from that event. */
static void nat_enable(void)
{
    esp_netif_set_default_netif(s_mesh_netif);
    esp_err_t err = esp_netif_napt_enable(s_ap_netif);
    if (err == ESP_OK)
    {
        ESP_LOGI(TAG, "Hotspot clients reach the mesh through NAT on bat0");
    }
    else
    {
        ESP_LOGE(TAG, "NAT into the mesh failed: %s", esp_err_to_name(err));
    }
}

/* Registered for WIFI_EVENT_AP_START itself, after esp_netif_create_default_wifi_ap()
 * registered ESP-IDF's own handler for it: handlers on one event id run in registration
 * order, so the netif is up by now. (A handler on ESP_EVENT_ANY_ID would run before every
 * id-level one: esp_event.c, esp_event_loop_run, base-level handlers first.) */
static void ap_started(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)base;
    (void)id;
    (void)data;
    if (s_mesh_netif != NULL)
    {
        nat_enable();
    }
}

static void wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)base;
    if (id == WIFI_EVENT_AP_STACONNECTED)
    {
        const wifi_event_ap_staconnected_t *e = data;
        ESP_LOGI(TAG, "Client %02x:%02x:%02x:%02x:%02x:%02x joined", e->mac[0], e->mac[1], e->mac[2], e->mac[3], e->mac[4],
                 e->mac[5]);
    }
    else if (id == WIFI_EVENT_AP_STADISCONNECTED)
    {
        const wifi_event_ap_stadisconnected_t *e = data;
        ESP_LOGI(TAG, "Client %02x:%02x:%02x:%02x:%02x:%02x left", e->mac[0], e->mac[1], e->mac[2], e->mac[3], e->mac[4],
                 e->mac[5]);
    }
}

void hotspot_prepare(const struct hop_settings *running)
{
    s_running = *running;
    s_setup = !running->configured;
    /* Setup mode signs in with the default root password, after that only the chosen one. */
    auth_init(s_setup ? NULL : running->root_hash);
    if (s_setup)
    {
        /* An open network, so the first person can get in, named so nobody mistakes it for
         * a working meshpoint. Setup insists on a hotspot password before leaving it. */
        snprintf(s_ap_name, sizeof(s_ap_name), "%.26s-Setup", s_running.ap_ssid);
    }
    else
    {
        strlcpy(s_ap_name, s_running.ap_ssid, sizeof(s_ap_name));
    }
    s_ap_netif = esp_netif_create_default_wifi_ap();
    esp_netif_ip_info_t ip = { 0 };
    ip.ip.addr = esp_ip4addr_aton(CONFIG_HOP_AP_IP);
    ip.gw.addr = ip.ip.addr;
    ip.netmask.addr = esp_ip4addr_aton("255.255.255.0");
    esp_netif_dhcps_stop(s_ap_netif);
    ESP_ERROR_CHECK(esp_netif_set_ip_info(s_ap_netif, &ip));
    if (s_setup)
    {
        esp_netif_dhcps_option(s_ap_netif, ESP_NETIF_OP_SET, ESP_NETIF_CAPTIVEPORTAL_URI, (void *)s_portal_uri,
                               strlen(s_portal_uri));
    }
    esp_netif_dhcps_start(s_ap_netif);

    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, WIFI_EVENT_AP_START, &ap_started, NULL, NULL));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));

    wifi_config_t cfg = { 0 };
    strlcpy((char *)cfg.ap.ssid, s_ap_name, sizeof(cfg.ap.ssid));
    cfg.ap.ssid_len = strlen(s_ap_name);
    if (s_setup)
    {
        cfg.ap.authmode = WIFI_AUTH_OPEN;
    }
    else
    {
        strlcpy((char *)cfg.ap.password, s_running.ap_password, sizeof(cfg.ap.password));
        cfg.ap.authmode = WIFI_AUTH_WPA2_PSK;
    }
    cfg.ap.channel = s_running.radio.ap_chan;
    cfg.ap.max_connection = 4;
    cfg.ap.pmf_cfg.capable = true;
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &cfg));
    /* Start once so the RF calibration happens now, then stay off until HaLow is up: the
     * halowchat firmware found the two radios starting together too much for USB power. */
    ESP_ERROR_CHECK(esp_wifi_start());
    esp_wifi_stop();
}

/* ---------- The page ---------- */

/* The client's IPv4 address (network order), or 0. */
static uint32_t peer_ip(httpd_req_t *req)
{
    struct sockaddr_storage ss;
    socklen_t len = sizeof(ss);
    if (getpeername(httpd_req_to_sockfd(req), (struct sockaddr *)&ss, &len) != 0)
    {
        return 0;
    }
    uint32_t addr = 0;
    if (ss.ss_family == AF_INET)
    {
        addr = ((struct sockaddr_in *)&ss)->sin_addr.s_addr;
    }
    else if (ss.ss_family == AF_INET6)
    {
        /* esp_http_server listens on an IPv6 socket when lwIP has IPv6; IPv4 clients then
         * appear as ::ffff:a.b.c.d. */
        const uint8_t *b = (const uint8_t *)&((struct sockaddr_in6 *)&ss)->sin6_addr;
        static const uint8_t mapped[12] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xff };
        if (memcmp(b, mapped, 12) != 0)
        {
            return 0;
        }
        memcpy(&addr, b + 12, 4);
    }
    return addr;
}

/* Changes are taken only from hotspot clients. Over the mesh, root can sign in and see the
 * status, but anyone on the mesh (including other nodes' hotspot users) is not trusted to
 * change this node. */
static bool from_hotspot(httpd_req_t *req)
{
    uint32_t addr = peer_ip(req);
    esp_netif_ip_info_t ap;
    if (addr == 0 || esp_netif_get_ip_info(s_ap_netif, &ap) != ESP_OK)
    {
        return false;
    }
    return (addr & ap.netmask.addr) == (ap.ip.addr & ap.netmask.addr);
}

static void mac_str(const uint8_t *m, char *out)
{
    snprintf(out, 18, "%02x:%02x:%02x:%02x:%02x:%02x", m[0], m[1], m[2], m[3], m[4], m[5]);
}

static esp_err_t send_json(httpd_req_t *req, cJSON *root)
{
    char *body = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (body == NULL)
    {
        return httpd_resp_send_500(req);
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    esp_err_t err = httpd_resp_sendstr(req, body);
    free(body);
    return err;
}

static esp_err_t send_result(httpd_req_t *req, const char *status, bool ok, const char *message)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ok", ok);
    cJSON_AddStringToObject(root, "message", message);
    httpd_resp_set_status(req, status);
    return send_json(req, root);
}

/* ---------- Sign-in ---------- */

#define COOKIE "sid"

/* The session token from the Cookie header. Not httpd_req_get_cookie_val: on some requests
 * (seen with a POST whose next header has no space after its colon) ESP-IDF 5.4's parser
 * leaves the Cookie value unterminated, so it runs on into the next header line and the
 * value comes back too long. Here the token is the AUTH_TOKEN_LEN hex characters after
 * "sid=", whatever follows them. */
static bool cookie_token(httpd_req_t *req, char token[AUTH_TOKEN_LEN + 1])
{
    char raw[256];
    esp_err_t err = httpd_req_get_hdr_value_str(req, "Cookie", raw, sizeof(raw));
    if (err != ESP_OK && err != ESP_ERR_HTTPD_RESULT_TRUNC)
    {
        return false;
    }
    bool found = false;
    for (const char *p = raw; !found && (p = strstr(p, COOKIE "=")) != NULL; p++)
    {
        if (p != raw && p[-1] != ' ' && p[-1] != ';')
        {
            continue;   /* the end of some other cookie's name */
        }
        const char *v = p + strlen(COOKIE "=");
        size_t n = 0;
        while (n <= AUTH_TOKEN_LEN && isxdigit((unsigned char)v[n]))
        {
            n++;
        }
        if (n == AUTH_TOKEN_LEN)
        {
            memcpy(token, v, AUTH_TOKEN_LEN);
            token[AUTH_TOKEN_LEN] = '\0';
            found = true;
        }
    }
    memset(raw, 0, sizeof(raw));
    return found;
}

static bool signed_in(httpd_req_t *req)
{
    char token[AUTH_TOKEN_LEN + 1];
    bool ok = cookie_token(req, token) && auth_session_valid(token);
    memset(token, 0, sizeof(token));
    return ok;
}

/* Every API call but sign-in answers this until the browser has signed in; the pages then
 * show their sign-in form. */
static esp_err_t need_login(httpd_req_t *req)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ok", false);
    cJSON_AddBoolToObject(root, "login", true);
    cJSON_AddStringToObject(root, "message", "Sign in first");
    httpd_resp_set_status(req, "401 Unauthorized");
    return send_json(req, root);
}

static void log_peer(const char *what, httpd_req_t *req)
{
    uint32_t a = peer_ip(req);
    ESP_LOGI(TAG, "%s from " IPSTR, what, IP2STR((esp_ip4_addr_t *)&a));
}

static esp_err_t page_get(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    if (s_setup)
    {
        return httpd_resp_send(req, setup_html_start, setup_html_end - setup_html_start - 1);
    }
    return httpd_resp_send(req, page_html_start, page_html_end - page_html_start - 1);
}

/* The picture on the sign-in screens. Public, like the pages themselves, and cached. */
static esp_err_t board_get(httpd_req_t *req)
{
    httpd_resp_set_type(req, "image/webp");
    httpd_resp_set_hdr(req, "Cache-Control", "public, max-age=86400");
    return httpd_resp_send(req, board_webp_start, board_webp_end - board_webp_start);
}

/* The radio form's script, shared by the setup and settings pages. */
static esp_err_t radio_js_get(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/javascript");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, radio_js_start, radio_js_end - radio_js_start - 1);
}

/* Setup mode: every other address (a phone's captive-portal check, any page a person types)
 * is sent to the setup page. iOS needs a body with the redirect to treat it as a portal
 * (ESP-IDF captive_portal example, http_404_error_handler). */
static esp_err_t redirect_to_setup(httpd_req_t *req, httpd_err_code_t err)
{
    (void)err;
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", s_portal_uri);
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_sendstr(req, "Meshpoint setup");
}

static esp_err_t status_get(httpd_req_t *req)
{
    if (!signed_in(req))
    {
        return need_login(req);
    }
    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "can_edit", from_hotspot(req));
    cJSON_AddBoolToObject(root, "setup", s_setup);
    cJSON_AddStringToObject(root, "mesh_id", s_running.mesh_id);
    cJSON_AddBoolToObject(root, "mesh_secure", s_running.mesh_password[0] != '\0');
    cJSON_AddStringToObject(root, "ap_ssid", s_running.ap_ssid);
    cJSON_AddBoolToObject(root, "saved", s_running.configured);
    cJSON_AddNumberToObject(root, "uptime_s", (double)(esp_timer_get_time() / 1000000));

    char buf[18];
    esp_netif_ip_info_t ip;
    if (s_mesh_netif != NULL && esp_netif_get_ip_info(s_mesh_netif, &ip) == ESP_OK)
    {
        snprintf(buf, sizeof(buf), IPSTR, IP2STR(&ip.ip));
        cJSON_AddStringToObject(root, "mesh_ip", buf);
    }
    if (esp_netif_get_ip_info(s_ap_netif, &ip) == ESP_OK)
    {
        snprintf(buf, sizeof(buf), IPSTR, IP2STR(&ip.ip));
        cJSON_AddStringToObject(root, "ap_ip", buf);
    }
    wifi_sta_list_t stas;
    cJSON_AddNumberToObject(root, "ap_clients", esp_wifi_ap_get_sta_list(&stas) == ESP_OK ? stas.num : 0);

    /* The radio this boot runs with, auto settings worked out. */
    const struct mmwlan_s1g_channel *ch = radio_find(s_running.radio.s1g_chan);
    uint8_t pbw, pidx;
    radio_resolve(&s_running.radio, &pbw, &pidx);
    cJSON *rd = cJSON_AddObjectToObject(root, "radio");
    cJSON_AddNumberToObject(rd, "chan", s_running.radio.s1g_chan);
    cJSON_AddNumberToObject(rd, "mhz", ch ? ch->centre_freq_hz / 1e6 : 0);
    cJSON_AddNumberToObject(rd, "bw", ch ? ch->bw_mhz : 0);
    cJSON_AddNumberToObject(rd, "prim_bw", pbw);
    cJSON_AddNumberToObject(rd, "prim_idx", pidx);
    cJSON_AddNumberToObject(rd, "ap_chan", s_running.radio.ap_chan);

    /* In setup mode the mesh and batman never start, and their getters take locks that only
     * exist once they have. */
    struct mesh_neighbour nb[8];
    int nn = s_setup ? 0 : mesh_get_neighbours(nb, 8);
    cJSON *peers = cJSON_AddArrayToObject(root, "peers");
    for (int i = 0; i < nn; i++)
    {
        cJSON *p = cJSON_CreateObject();
        mac_str(nb[i].addr, buf);
        cJSON_AddStringToObject(p, "mac", buf);
        cJSON_AddNumberToObject(p, "rssi", nb[i].rssi);
        cJSON_AddStringToObject(p, "state", mesh_plink_state_name(nb[i].plink));
        cJSON_AddBoolToObject(p, "peered", nb[i].plink == MESH_PLINK_ESTAB);
        cJSON_AddItemToArray(peers, p);
    }

    struct batadv_originator og[16];
    int no = s_setup ? 0 : batadv_get_originators(og, 16);
    cJSON *nodes = cJSON_AddArrayToObject(root, "nodes");
    for (int i = 0; i < no; i++)
    {
        cJSON *o = cJSON_CreateObject();
        mac_str(og[i].addr, buf);
        cJSON_AddStringToObject(o, "mac", buf);
        mac_str(og[i].router, buf);
        cJSON_AddStringToObject(o, "via", buf);
        cJSON_AddNumberToObject(o, "mbps", og[i].throughput / 10.0);
        cJSON_AddItemToArray(nodes, o);
    }
    return send_json(req, root);
}

/* The radio form's data: the country's channel table (the same rows as OpenMANET's
 * channels.csv) and the saved choices, with auto kept as auto. */
static esp_err_t radio_get(httpd_req_t *req)
{
    if (!signed_in(req))
    {
        return need_login(req);
    }
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "country", CONFIG_HALOW_COUNTRY_CODE);
    cJSON_AddNumberToObject(root, "ap_max_chan", radio_ap_max_chan());
    cJSON *chans = cJSON_AddArrayToObject(root, "channels");
    const struct mmwlan_s1g_channel_list *list = radio_channels();
    for (unsigned i = 0; list != NULL && i < list->num_channels; i++)
    {
        const struct mmwlan_s1g_channel *c = &list->channels[i];
        cJSON *o = cJSON_CreateObject();
        cJSON_AddNumberToObject(o, "chan", c->s1g_chan_num);
        cJSON_AddNumberToObject(o, "mhz", c->centre_freq_hz / 1e6);
        cJSON_AddNumberToObject(o, "bw", c->bw_mhz);
        cJSON_AddNumberToObject(o, "op", c->global_operating_class);
        cJSON_AddItemToArray(chans, o);
    }
    const struct hop_radio *r = &s_running.radio;
    cJSON *saved = cJSON_AddObjectToObject(root, "saved");
    cJSON_AddNumberToObject(saved, "chan", r->s1g_chan);
    cJSON_AddNumberToObject(saved, "prim_bw", r->prim_bw_mhz);
    cJSON_AddNumberToObject(saved, "prim_idx", r->prim_idx);
    cJSON_AddNumberToObject(saved, "beacon", r->beacon_tus);
    cJSON_AddNumberToObject(saved, "tx_dbm", r->tx_dbm);
    cJSON_AddNumberToObject(saved, "ap_chan", r->ap_chan);
    return send_json(req, root);
}

/* Reads the optional "radio" object of a settings request into r. False if it is there but
 * malformed. */
static bool take_radio(const cJSON *root, struct hop_radio *r)
{
    const cJSON *o = cJSON_GetObjectItemCaseSensitive(root, "radio");
    if (o == NULL)
    {
        return true;
    }
    if (!cJSON_IsObject(o))
    {
        return false;
    }
    struct
    {
        const char *key;
        int lo, hi;
        int value;
    } f[] = {
        { "chan", 1, 255, r->s1g_chan },     { "prim_bw", 0, 2, r->prim_bw_mhz }, { "prim_idx", -1, 7, r->prim_idx },
        { "beacon", 1, 65535, r->beacon_tus }, { "tx_dbm", 0, 255, r->tx_dbm },  { "ap_chan", 1, 14, r->ap_chan },
    };
    for (size_t i = 0; i < sizeof(f) / sizeof(f[0]); i++)
    {
        const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, f[i].key);
        if (v == NULL)
        {
            continue;
        }
        if (!cJSON_IsNumber(v) || v->valuedouble != (double)v->valueint || v->valueint < f[i].lo || v->valueint > f[i].hi)
        {
            return false;
        }
        f[i].value = v->valueint;
    }
    r->s1g_chan = (uint8_t)f[0].value;
    r->prim_bw_mhz = (uint8_t)f[1].value;
    r->prim_idx = (int8_t)f[2].value;
    r->beacon_tus = (uint16_t)f[3].value;
    r->tx_dbm = (uint8_t)f[4].value;
    r->ap_chan = (uint8_t)f[5].value;
    return true;
}

static void restart_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(1500));   /* let the reply reach the browser first */
    esp_restart();
}

static int read_body(httpd_req_t *req, char *body, size_t size)
{
    if (req->content_len == 0 || req->content_len >= size)
    {
        return -1;
    }
    size_t got = 0;
    while (got < req->content_len)
    {
        int n = httpd_req_recv(req, body + got, req->content_len - got);
        if (n == HTTPD_SOCK_ERR_TIMEOUT)
        {
            continue;
        }
        if (n <= 0)
        {
            return -1;
        }
        got += (size_t)n;
    }
    body[got] = '\0';
    return (int)got;
}

/* Copies a string field when present; an empty or missing password field keeps the
 * current one, so the page never needs to know it. False if the value is too long to
 * keep whole (it is refused rather than cut short). */
static bool take(const cJSON *root, const char *key, char *out, size_t size, bool empty_keeps)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(root, key);
    if (!cJSON_IsString(v) || v->valuestring == NULL)
    {
        return true;
    }
    if (empty_keeps && v->valuestring[0] == '\0')
    {
        return true;
    }
    if (strlen(v->valuestring) >= size)
    {
        return false;
    }
    strlcpy(out, v->valuestring, size);
    return true;
}

static esp_err_t settings_post(httpd_req_t *req)
{
    if (!signed_in(req))
    {
        return need_login(req);
    }
    if (!from_hotspot(req))
    {
        return send_result(req, "403 Forbidden", false, "Settings can only be changed from this meshpoint's hotspot");
    }
    char body[768];
    if (read_body(req, body, sizeof(body)) < 0)
    {
        return send_result(req, "400 Bad Request", false, "Request too long or empty");
    }
    cJSON *root = cJSON_Parse(body);
    memset(body, 0, sizeof(body));
    if (root == NULL)
    {
        return send_result(req, "400 Bad Request", false, "Not JSON");
    }
    struct hop_settings next = s_running;
    bool fits = take(root, "mesh_id", next.mesh_id, sizeof(next.mesh_id), false);
    fits &= take(root, "mesh_password", next.mesh_password, sizeof(next.mesh_password), true);
    if (cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(root, "mesh_open")))
    {
        next.mesh_password[0] = '\0';
    }
    fits &= take(root, "ap_ssid", next.ap_ssid, sizeof(next.ap_ssid), false);
    fits &= take(root, "ap_password", next.ap_password, sizeof(next.ap_password), true);
    bool open_mesh = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(root, "mesh_open"));
    bool radio_ok = take_radio(root, &next.radio);
    /* Setup replaces the default root password; later changes go through /api/password. */
    char root_pw[64] = "";
    if (s_setup)
    {
        fits &= take(root, "root_password", root_pw, sizeof(root_pw), false);
    }
    cJSON_Delete(root);

    const char *why = fits ? NULL : "A value is too long";
    if (fits && s_setup && !auth_password_acceptable(root_pw, &why))
    {
        fits = false;
    }
    if (fits && s_setup && next.mesh_password[0] == '\0' && !open_mesh)
    {
        /* In setup there is no current password to keep: blank means the person forgot. */
        fits = false;
        why = "Enter the mesh password, or tick Open network";
    }
    if (fits && !radio_ok)
    {
        fits = false;
        why = "The radio settings are malformed";
    }
    if (fits && !settings_valid(&next, &why))
    {
        fits = false;
    }
    if (fits && !radio_valid(&next.radio, &why))
    {
        fits = false;
    }
    if (fits && s_setup && !auth_hash(root_pw, next.root_hash))
    {
        fits = false;
        why = "Could not hash the root password";
    }
    memset(root_pw, 0, sizeof(root_pw));
    if (!fits)
    {
        memset(&next, 0, sizeof(next));
        return send_result(req, "400 Bad Request", false, why);
    }
    esp_err_t err = settings_save(&next);
    memset(&next, 0, sizeof(next));
    if (err != ESP_OK)
    {
        return send_result(req, "500 Internal Server Error", false, "Could not save the settings");
    }
    send_result(req, "200 OK", true, "Saved. The meshpoint restarts now.");
    xTaskCreate(restart_task, "restart", 2048, NULL, 5, NULL);
    return ESP_OK;
}

static esp_err_t reset_post(httpd_req_t *req)
{
    if (!signed_in(req))
    {
        return need_login(req);
    }
    if (!from_hotspot(req))
    {
        return send_result(req, "403 Forbidden", false, "Settings can only be changed from this meshpoint's hotspot");
    }
    if (settings_erase() != ESP_OK)
    {
        return send_result(req, "500 Internal Server Error", false, "Could not erase the settings");
    }
    send_result(req, "200 OK", true, "Settings erased. The meshpoint restarts in setup mode.");
    xTaskCreate(restart_task, "restart", 2048, NULL, 5, NULL);
    return ESP_OK;
}

/* Reads the named string fields of a small JSON body into the given buffers. False if the
 * body is not JSON or a field is too long. */
static bool read_fields(httpd_req_t *req, const char *const *keys, char *const *outs, size_t size, int n)
{
    char body[384];
    if (read_body(req, body, sizeof(body)) < 0)
    {
        return false;
    }
    cJSON *root = cJSON_Parse(body);
    memset(body, 0, sizeof(body));
    if (root == NULL)
    {
        return false;
    }
    bool fits = true;
    for (int i = 0; i < n; i++)
    {
        outs[i][0] = '\0';
        fits &= take(root, keys[i], outs[i], size, false);
    }
    cJSON_Delete(root);
    return fits;
}

static esp_err_t login_post(httpd_req_t *req)
{
    char user[64], pw[64];
    const char *const keys[] = { "username", "password" };
    char *const outs[] = { user, pw };
    if (!read_fields(req, keys, outs, sizeof(user), 2))
    {
        memset(pw, 0, sizeof(pw));
        return send_result(req, "400 Bad Request", false, "Bad request");
    }
    int wait_s = 0;
    enum auth_result r = auth_check(user, pw, &wait_s);
    memset(pw, 0, sizeof(pw));
    if (r == AUTH_LOCKED)
    {
        char msg[64];
        snprintf(msg, sizeof(msg), "Too many wrong tries. Wait %d seconds.", wait_s);
        return send_result(req, "429 Too Many Requests", false, msg);
    }
    if (r != AUTH_OK)
    {
        log_peer("Wrong sign-in", req);
        return send_result(req, "401 Unauthorized", false, "Wrong user name or password");
    }
    char token[AUTH_TOKEN_LEN + 1];
    auth_session_open(token);
    char cookie[AUTH_TOKEN_LEN + 48];
    snprintf(cookie, sizeof(cookie), COOKIE "=%s; Path=/; HttpOnly; SameSite=Strict", token);
    memset(token, 0, sizeof(token));
    httpd_resp_set_hdr(req, "Set-Cookie", cookie);
    log_peer("Signed in", req);
    esp_err_t err = send_result(req, "200 OK", true, "Signed in");
    memset(cookie, 0, sizeof(cookie));
    return err;
}

static esp_err_t logout_post(httpd_req_t *req)
{
    char token[AUTH_TOKEN_LEN + 1];
    if (cookie_token(req, token))
    {
        auth_session_close(token);
    }
    memset(token, 0, sizeof(token));
    httpd_resp_set_hdr(req, "Set-Cookie", COOKIE "=; Path=/; Max-Age=0; HttpOnly; SameSite=Strict");
    return send_result(req, "200 OK", true, "Signed out");
}

/* After setup: root changes its own password. The current one is asked again, so a page
 * left signed in is not enough to take the board over. Every session ends. */
static esp_err_t password_post(httpd_req_t *req)
{
    if (!signed_in(req))
    {
        return need_login(req);
    }
    if (s_setup)
    {
        return send_result(req, "400 Bad Request", false, "Finish setup first");
    }
    if (!from_hotspot(req))
    {
        return send_result(req, "403 Forbidden", false, "Settings can only be changed from this meshpoint's hotspot");
    }
    char cur[64], pw[64];
    const char *const keys[] = { "current", "password" };
    char *const outs[] = { cur, pw };
    bool fits = read_fields(req, keys, outs, sizeof(cur), 2);
    const char *why = "Bad request";
    int wait_s = 0;
    enum auth_result r = fits ? auth_check(AUTH_USER, cur, &wait_s) : AUTH_WRONG;
    memset(cur, 0, sizeof(cur));
    if (!fits || r != AUTH_OK || !auth_password_acceptable(pw, &why))
    {
        memset(pw, 0, sizeof(pw));
        if (fits && r == AUTH_LOCKED)
        {
            return send_result(req, "429 Too Many Requests", false, "Too many wrong tries. Wait and try again.");
        }
        if (fits && r != AUTH_OK)
        {
            return send_result(req, "403 Forbidden", false, "The current password is wrong");
        }
        return send_result(req, "400 Bad Request", false, why);
    }
    uint8_t hash[SETTINGS_ROOT_HASH_LEN];
    bool hashed = auth_hash(pw, hash);
    memset(pw, 0, sizeof(pw));
    if (!hashed || settings_save_root_hash(hash) != ESP_OK)
    {
        return send_result(req, "500 Internal Server Error", false, "Could not save the new password");
    }
    memcpy(s_running.root_hash, hash, sizeof(hash));
    auth_set_hash(hash);
    auth_session_close_all();
    httpd_resp_set_hdr(req, "Set-Cookie", COOKIE "=; Path=/; Max-Age=0; HttpOnly; SameSite=Strict");
    log_peer("Root password changed", req);
    return send_result(req, "200 OK", true, "Password changed. Sign in with the new one.");
}

static void web_start(void)
{
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.stack_size = 8192;
    cfg.lru_purge_enable = true;
    cfg.max_open_sockets = 5;
    cfg.max_uri_handlers = 12;
    httpd_handle_t server = NULL;
    if (httpd_start(&server, &cfg) != ESP_OK)
    {
        ESP_LOGE(TAG, "Settings page failed to start");
        return;
    }
    const httpd_uri_t routes[] = {
        { .uri = "/", .method = HTTP_GET, .handler = page_get },
        { .uri = "/board.webp", .method = HTTP_GET, .handler = board_get },
        { .uri = "/radio.js", .method = HTTP_GET, .handler = radio_js_get },
        { .uri = "/api/radio", .method = HTTP_GET, .handler = radio_get },
        { .uri = "/api/status", .method = HTTP_GET, .handler = status_get },
        { .uri = "/api/settings", .method = HTTP_POST, .handler = settings_post },
        { .uri = "/api/reset", .method = HTTP_POST, .handler = reset_post },
        { .uri = "/api/login", .method = HTTP_POST, .handler = login_post },
        { .uri = "/api/logout", .method = HTTP_POST, .handler = logout_post },
        { .uri = "/api/password", .method = HTTP_POST, .handler = password_post },
    };
    for (size_t i = 0; i < sizeof(routes) / sizeof(routes[0]); i++)
    {
        httpd_register_uri_handler(server, &routes[i]);
    }
    if (s_setup)
    {
        httpd_register_err_handler(server, HTTPD_404_NOT_FOUND, redirect_to_setup);
    }
}

/* ---------- Bring-up ---------- */

void hotspot_start(esp_netif_t *mesh_netif)
{
    if (s_ap_netif == NULL || s_ready)
    {
        return;
    }
    s_mesh_netif = mesh_netif;   /* set first: the AP start event turns on NAT with it */
    ESP_ERROR_CHECK(esp_wifi_start());
    /* Quarter-dBm units; set after start (esp_wifi.h). A low cap keeps the 2.4 GHz radio's
     * current peaks down next to the HaLow radio on USB power. */
    esp_wifi_set_max_tx_power((int8_t)(CONFIG_HOP_AP_TX_DBM * 4));
    web_start();
    if (s_setup)
    {
        captive_dns_start(esp_ip4addr_aton(CONFIG_HOP_AP_IP));
    }
    s_ready = true;
    ESP_LOGI(TAG, "%s \"%s\" on channel %d, page at http://%s/", s_setup ? "Setup hotspot (open)" : "Hotspot", s_ap_name,
             s_running.radio.ap_chan, CONFIG_HOP_AP_IP);
}

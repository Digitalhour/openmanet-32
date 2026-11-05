/* SPDX-License-Identifier: 0BSD OR GPL-3.0-or-later */
#include "hotspot.h"

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

#include "batadv.h"
#include "mesh.h"

static const char *TAG = "hotspot";

extern const char page_html_start[] asm("_binary_page_html_start");
extern const char page_html_end[] asm("_binary_page_html_end");

static struct hop_settings s_running;   /* what this boot runs with */
static esp_netif_t *s_ap_netif;
static esp_netif_t *s_mesh_netif;
static bool s_ready;

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
    s_ap_netif = esp_netif_create_default_wifi_ap();
    esp_netif_ip_info_t ip = { 0 };
    ip.ip.addr = esp_ip4addr_aton(CONFIG_HOP_AP_IP);
    ip.gw.addr = ip.ip.addr;
    ip.netmask.addr = esp_ip4addr_aton("255.255.255.0");
    esp_netif_dhcps_stop(s_ap_netif);
    ESP_ERROR_CHECK(esp_netif_set_ip_info(s_ap_netif, &ip));
    esp_netif_dhcps_start(s_ap_netif);

    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, WIFI_EVENT_AP_START, &ap_started, NULL, NULL));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));

    wifi_config_t cfg = { 0 };
    strlcpy((char *)cfg.ap.ssid, s_running.ap_ssid, sizeof(cfg.ap.ssid));
    cfg.ap.ssid_len = strlen(s_running.ap_ssid);
    strlcpy((char *)cfg.ap.password, s_running.ap_password, sizeof(cfg.ap.password));
    cfg.ap.authmode = WIFI_AUTH_WPA2_PSK;
    cfg.ap.channel = CONFIG_HOP_AP_CHANNEL;
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

/* Changes are taken only from hotspot clients. The mesh can see the status, but anyone on
 * the mesh (including other nodes' hotspot users) is not trusted to change this node. */
static bool from_hotspot(httpd_req_t *req)
{
    struct sockaddr_storage ss;
    socklen_t len = sizeof(ss);
    if (getpeername(httpd_req_to_sockfd(req), (struct sockaddr *)&ss, &len) != 0)
    {
        return false;
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
            return false;
        }
        memcpy(&addr, b + 12, 4);
    }
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

static esp_err_t page_get(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, page_html_start, page_html_end - page_html_start - 1);
}

static esp_err_t status_get(httpd_req_t *req)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "can_edit", from_hotspot(req));
    cJSON_AddStringToObject(root, "mesh_id", s_running.mesh_id);
    cJSON_AddBoolToObject(root, "mesh_secure", s_running.mesh_password[0] != '\0');
    cJSON_AddStringToObject(root, "ap_ssid", s_running.ap_ssid);
    cJSON_AddBoolToObject(root, "saved", s_running.from_nvs);
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

    struct mesh_neighbour nb[8];
    int nn = mesh_get_neighbours(nb, 8);
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
    int no = batadv_get_originators(og, 16);
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
    if (!from_hotspot(req))
    {
        return send_result(req, "403 Forbidden", false, "Settings can only be changed from this meshpoint's hotspot");
    }
    char body[512];
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
    cJSON_Delete(root);

    const char *why = fits ? NULL : "A value is too long";
    if (!fits || !settings_valid(&next, &why))
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
    if (!from_hotspot(req))
    {
        return send_result(req, "403 Forbidden", false, "Settings can only be changed from this meshpoint's hotspot");
    }
    if (settings_erase() != ESP_OK)
    {
        return send_result(req, "500 Internal Server Error", false, "Could not erase the settings");
    }
    send_result(req, "200 OK", true, "Back to the build defaults. The meshpoint restarts now.");
    xTaskCreate(restart_task, "restart", 2048, NULL, 5, NULL);
    return ESP_OK;
}

static void web_start(void)
{
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.stack_size = 8192;
    cfg.lru_purge_enable = true;
    cfg.max_open_sockets = 5;
    httpd_handle_t server = NULL;
    if (httpd_start(&server, &cfg) != ESP_OK)
    {
        ESP_LOGE(TAG, "Settings page failed to start");
        return;
    }
    const httpd_uri_t routes[] = {
        { .uri = "/", .method = HTTP_GET, .handler = page_get },
        { .uri = "/api/status", .method = HTTP_GET, .handler = status_get },
        { .uri = "/api/settings", .method = HTTP_POST, .handler = settings_post },
        { .uri = "/api/reset", .method = HTTP_POST, .handler = reset_post },
    };
    for (size_t i = 0; i < sizeof(routes) / sizeof(routes[0]); i++)
    {
        httpd_register_uri_handler(server, &routes[i]);
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
    s_ready = true;
    ESP_LOGI(TAG, "Hotspot \"%s\" on channel %d, page at http://%s/", s_running.ap_ssid, CONFIG_HOP_AP_CHANNEL,
             CONFIG_HOP_AP_IP);
}

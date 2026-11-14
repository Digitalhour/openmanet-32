/* SPDX-License-Identifier: 0BSD OR GPL-3.0-or-later */
#include "gateway.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "lwip/sockets.h"

#include "batadv.h"
#include "gateway_protocol.h"
#include "hotspot.h"

#if CONFIG_HOP_AUTO_GATEWAY

#define PRIMARY_COUNT 8
#define PRIMARY_TIMEOUT_US (60 * 1000000LL)
#define REQUEST_INTERVAL_US (10 * 1000000LL)
#define RECORD_TIMEOUT_US (180 * 1000000LL)

static const char *TAG = "gateway";
static esp_netif_t *s_netif;

struct primary
{
    struct sockaddr_in6 addr;
    int64_t seen_us, requested_us;
    uint16_t transaction;
};

static bool set_gateway(uint32_t address)
{
    esp_netif_ip_info_t info;
    ESP_ERROR_CHECK(esp_netif_get_ip_info(s_netif, &info));
    if (address != 0 &&
        ((address & info.netmask.addr) != (info.ip.addr & info.netmask.addr) ||
         address == info.ip.addr || (address & ~info.netmask.addr) == 0 ||
         (address | info.netmask.addr) == UINT32_MAX))
    {
        esp_ip4_addr_t invalid = { .addr = address };
        ESP_LOGE(TAG, "Advertised gateway " IPSTR " is not another host in the static bat0 subnet", IP2STR(&invalid));
        return false;
    }
    info.gw.addr = address;
    ESP_ERROR_CHECK(esp_netif_set_ip_info(s_netif, &info));
    if (address != 0)
    {
        esp_netif_dns_info_t dns = { 0 };
        dns.ip.type = ESP_IPADDR_TYPE_V4;
        dns.ip.u_addr.ip4.addr = address;
        ESP_ERROR_CHECK(esp_netif_set_dns_info(s_netif, ESP_NETIF_DNS_MAIN, &dns));
        hotspot_set_upstream_dns(address);
    }
    if (address != 0)
    {
        ESP_LOGI(TAG, "Static bat0 " IPSTR ", discovered gateway/DNS " IPSTR,
                 IP2STR(&info.ip), IP2STR(&info.gw));
    }
    else
    {
        ESP_LOGW(TAG, "Internet route removed; static bat0 " IPSTR " retained", IP2STR(&info.ip));
    }
    return true;
}

static int open_socket(void)
{
    int sock = socket(AF_INET6, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0)
    {
        ESP_LOGE(TAG, "ALFRED socket failed: errno %d", errno);
        return -1;
    }
    struct ifreq iface = { 0 };
    ESP_ERROR_CHECK(esp_netif_get_netif_impl_name(s_netif, iface.ifr_name));
    int v6_only = 1;
    struct timeval timeout = { .tv_sec = 1 };
    struct sockaddr_in6 local = { .sin6_family = AF_INET6, .sin6_port = htons(GATEWAY_ALFRED_PORT) };
    if (setsockopt(sock, SOL_SOCKET, SO_BINDTODEVICE, &iface, sizeof(iface)) != 0 ||
        setsockopt(sock, IPPROTO_IPV6, IPV6_V6ONLY, &v6_only, sizeof(v6_only)) != 0 ||
        setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) != 0 ||
        bind(sock, (struct sockaddr *)&local, sizeof(local)) != 0)
    {
        ESP_LOGE(TAG, "ALFRED socket configuration failed: errno %d", errno);
        close(sock);
        return -1;
    }
    /* ff02::1 is the ALFRED primary-announcement group. Binding to bat0 also
     * confines unicast replies and prevents listening on the phone hotspot. */
    struct ipv6_mreq group = { .ipv6mr_interface = esp_netif_get_netif_impl_index(s_netif) };
    inet_pton(AF_INET6, "ff02::1", &group.ipv6mr_multiaddr);
    if (setsockopt(sock, IPPROTO_IPV6, IPV6_JOIN_GROUP, &group, sizeof(group)) != 0)
    {
        ESP_LOGE(TAG, "ALFRED multicast membership failed: errno %d", errno);
        close(sock);
        return -1;
    }
    return sock;
}

static int primary_find(struct primary *primaries, const struct sockaddr_in6 *addr)
{
    for (int i = 0; i < PRIMARY_COUNT; i++)
    {
        if (primaries[i].seen_us != 0 &&
            memcmp(&primaries[i].addr.sin6_addr, &addr->sin6_addr, sizeof(addr->sin6_addr)) == 0)
        {
            return i;
        }
    }
    return -1;
}

static void primary_announce(struct primary *primaries, const struct sockaddr_in6 *addr, int64_t now)
{
    int i = primary_find(primaries, addr);
    if (i < 0)
    {
        i = 0;
        for (int k = 1; k < PRIMARY_COUNT; k++)
        {
            if (primaries[k].seen_us < primaries[i].seen_us)
            {
                i = k;
            }
        }
        memset(&primaries[i], 0, sizeof(primaries[i]));
        primaries[i].addr = *addr;
        primaries[i].addr.sin6_scope_id = esp_netif_get_netif_impl_index(s_netif);
        ESP_LOGI(TAG, "Discovered ALFRED primary on bat0");
    }
    primaries[i].seen_us = now;
}

static void request_records(int sock, struct primary *primaries, int64_t now)
{
    for (int i = 0; i < PRIMARY_COUNT; i++)
    {
        struct primary *p = &primaries[i];
        if (p->seen_us == 0 || now - p->seen_us > PRIMARY_TIMEOUT_US ||
            (p->requested_us != 0 && now - p->requested_us < REQUEST_INTERVAL_US))
        {
            continue;
        }
        p->transaction = (uint16_t)esp_random();
        const uint8_t request[] = {
            2, 0, 0, 3, GATEWAY_ALFRED_TYPE,
            (uint8_t)(p->transaction >> 8), (uint8_t)p->transaction,
        };
        p->requested_us = now;
        if (sendto(sock, request, sizeof(request), 0, (struct sockaddr *)&p->addr, sizeof(p->addr)) != sizeof(request))
        {
            ESP_LOGW(TAG, "ALFRED gateway request failed: errno %d", errno);
        }
    }
}

static void gateway_task(void *arg)
{
    (void)arg;
    struct primary primaries[PRIMARY_COUNT] = { 0 };
    uint8_t selected[6] = { 0 };
    uint32_t installed = 0;
    int64_t record_seen = 0, last_warning = -30000000LL;
    int sock = -1;
    for (;;)
    {
        int64_t now = esp_timer_get_time();
        struct batadv_gateway gw;
        bool have_gateway = batadv_get_best_gateway(selected, &gw);
        if (!have_gateway || memcmp(selected, gw.addr, 6) != 0)
        {
            if (installed != 0)
            {
                set_gateway(0);
                installed = 0;
            }
            memset(selected, 0, sizeof(selected));
            record_seen = 0;
            if (have_gateway)
            {
                memcpy(selected, gw.addr, 6);
                ESP_LOGI(TAG, "Selected BATMAN gateway %02x:%02x:%02x:%02x:%02x:%02x, down %lu, path %lu (100 kbit/s)",
                         selected[0], selected[1], selected[2], selected[3], selected[4], selected[5],
                         (unsigned long)gw.bandwidth_down, (unsigned long)gw.throughput);
                for (int i = 0; i < PRIMARY_COUNT; i++)
                {
                    primaries[i].requested_us = 0;
                }
            }
        }
        if (installed != 0 && now - record_seen > RECORD_TIMEOUT_US)
        {
            ESP_LOGW(TAG, "ALFRED gateway address expired; removing internet route");
            set_gateway(0);
            installed = 0;
        }
        if (installed == 0 && now - last_warning >= 30000000LL)
        {
            ESP_LOGW(TAG, "%s; mesh/static IP and hotspot dashboard remain available",
                     have_gateway ? "Waiting for selected gateway's ALFRED IPv4 record" : "No active BATMAN gateway advertisement");
            last_warning = now;
        }
        if (sock < 0)
        {
            sock = open_socket();
            if (sock < 0)
            {
                vTaskDelay(pdMS_TO_TICKS(5000));
                continue;
            }
        }
        request_records(sock, primaries, now);
        uint8_t header[4];
        struct sockaddr_in6 from;
        socklen_t from_len = sizeof(from);
        int len = recvfrom(sock, header, sizeof(header), MSG_PEEK, (struct sockaddr *)&from, &from_len);
        if (len < 0)
        {
            if (errno != EAGAIN && errno != EWOULDBLOCK)
            {
                ESP_LOGE(TAG, "ALFRED receive failed: errno %d; reopening socket", errno);
                close(sock);
                sock = -1;
            }
            continue;
        }
        size_t expected = len == 4 ? 4 + ((size_t)header[2] << 8 | header[3]) : 0;
        if (expected < 4 || expected > 65527 || header[1] != 0 ||
            from.sin6_port != htons(GATEWAY_ALFRED_PORT) ||
            !IN6_IS_ADDR_LINKLOCAL(&from.sin6_addr))
        {
            recvfrom(sock, header, sizeof(header), 0, NULL, NULL);
            ESP_LOGD(TAG, "Ignoring invalid ALFRED header/source");
            continue;
        }
        uint8_t *packet = malloc(expected + 1);
        if (packet == NULL)
        {
            recvfrom(sock, header, sizeof(header), 0, NULL, NULL);
            ESP_LOGE(TAG, "Cannot allocate %u bytes for ALFRED datagram", (unsigned)expected + 1);
            continue;
        }
        len = recvfrom(sock, packet, expected + 1, 0, NULL, NULL);
        now = esp_timer_get_time();
        if (len == (int)expected)
        {
            if (packet[0] == 1 && len == 4)
            {
                primary_announce(primaries, &from, now);
            }
            else if (packet[0] == 0 && have_gateway)
            {
                int i = primary_find(primaries, &from);
                if (i >= 0 && primaries[i].requested_us != 0 &&
                    now - primaries[i].requested_us <= REQUEST_INTERVAL_US)
                {
                    struct gateway_record record;
                    int result = gateway_parse_alfred(packet, len, primaries[i].transaction, selected, &record);
                    if (result > 0)
                    {
                        uint32_t address;
                        memcpy(&address, record.ip, sizeof(address));
                        if (address == installed || set_gateway(address))
                        {
                            installed = address;
                            record_seen = now;
                        }
                    }
                    else if (result < 0)
                    {
                        ESP_LOGD(TAG, "Ignoring malformed/stale ALFRED gateway response");
                    }
                }
            }
            else if (packet[0] == 4)
            {
                ESP_LOGW(TAG, "ALFRED primary reported a transaction error");
            }
        }
        else
        {
            ESP_LOGW(TAG, "ALFRED datagram length does not match its header");
        }
        free(packet);
    }
}

void gateway_start(esp_netif_t *netif)
{
    s_netif = netif;
    ESP_ERROR_CHECK(esp_netif_create_ip6_linklocal(netif));
    if (xTaskCreate(gateway_task, "gateway", 6144, NULL, 3, NULL) != pdPASS)
    {
        ESP_LOGE(TAG, "Could not start native gateway discovery");
        ESP_ERROR_CHECK(ESP_ERR_NO_MEM);
    }
    ESP_LOGI(TAG, "Native BATMAN/ALFRED gateway discovery enabled; static mesh address retained");
}

#endif

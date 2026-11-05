/* SPDX-License-Identifier: 0BSD OR GPL-3.0-or-later */
/*
 * Setup mode only: a DNS server that answers every name with the hotspot's own address.
 *
 * Phones check for a captive portal by fetching a known page (captive.apple.com,
 * connectivitycheck.gstatic.com, msftconnecttest.com, ...). The hotspot's DHCP server names
 * itself as the DNS server (CONFIG_LWIP_DHCPS_ADD_DNS, dhcpserver.c), so with every lookup
 * answered here those checks land on our web server, get redirected to the setup page, and
 * the phone opens it on its own. Same approach as ESP-IDF's
 * examples/protocols/http_server/captive_portal (dns_server.c), cut down to what that needs.
 */
#include "captive_dns.h"

#include <stdbool.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "lwip/sockets.h"

static const char *TAG = "captive_dns";
static uint32_t s_answer_ip;   /* network order */

#define DNS_PORT 53
#define DNS_MAX 512

/* Builds the reply in place: the header and the first question are kept, an A record for
 * that question is appended when it asks for one (type A, class IN), and every other type
 * gets an empty answer, so a phone asking for AAAA falls back to A. Returns the reply length
 * or -1 to send nothing. */
static int answer(uint8_t *buf, int len)
{
    if (len < 12 || len > DNS_MAX - 16)
    {
        return -1;
    }
    uint16_t flags = (uint16_t)(buf[2] << 8 | buf[3]);
    uint16_t qdcount = (uint16_t)(buf[4] << 8 | buf[5]);
    if ((flags & 0x8000) || ((flags >> 11) & 0xf) != 0 || qdcount == 0)
    {
        return -1;   /* a response, not a standard query, or no question */
    }
    /* Walk the first question's name (labels only; a query does not compress it). */
    int i = 12;
    while (i < len && buf[i] != 0)
    {
        if (buf[i] & 0xc0)
        {
            return -1;
        }
        i += buf[i] + 1;
    }
    if (i + 5 > len)
    {
        return -1;
    }
    i++;   /* the root label */
    uint16_t qtype = (uint16_t)(buf[i] << 8 | buf[i + 1]);
    uint16_t qclass = (uint16_t)(buf[i + 2] << 8 | buf[i + 3]);
    int qend = i + 4;
    bool give_a = qtype == 1 && qclass == 1;

    buf[2] = 0x84 | (buf[2] & 0x01);   /* response, authoritative, keep RD */
    buf[3] = 0x00;                      /* no recursion available, no error */
    buf[4] = 0;
    buf[5] = 1;                         /* one question: the first */
    buf[6] = 0;
    buf[7] = give_a ? 1 : 0;            /* answers */
    memset(&buf[8], 0, 4);              /* no authority or additional records */
    int n = qend;
    if (give_a)
    {
        const uint8_t rr[] = {
            0xc0, 0x0c,             /* name: the question's, by pointer */
            0x00, 0x01, 0x00, 0x01, /* type A, class IN */
            0x00, 0x00, 0x00, 0x3c, /* TTL 60 s */
            0x00, 0x04,             /* four bytes of address */
        };
        memcpy(&buf[n], rr, sizeof(rr));
        n += sizeof(rr);
        memcpy(&buf[n], &s_answer_ip, 4);
        n += 4;
    }
    return n;
}

static void dns_task(void *arg)
{
    (void)arg;
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    struct sockaddr_in addr = { .sin_family = AF_INET, .sin_port = htons(DNS_PORT), .sin_addr.s_addr = htonl(INADDR_ANY) };
    if (sock < 0 || bind(sock, (struct sockaddr *)&addr, sizeof(addr)) != 0)
    {
        ESP_LOGE(TAG, "Could not open the DNS port");
        if (sock >= 0)
        {
            close(sock);
        }
        vTaskDelete(NULL);
        return;
    }
    static uint8_t buf[DNS_MAX];
    for (;;)
    {
        struct sockaddr_in from;
        socklen_t from_len = sizeof(from);
        int len = recvfrom(sock, buf, sizeof(buf), 0, (struct sockaddr *)&from, &from_len);
        if (len <= 0)
        {
            continue;
        }
        int n = answer(buf, len);
        if (n > 0)
        {
            sendto(sock, buf, n, 0, (struct sockaddr *)&from, from_len);
        }
    }
}

void captive_dns_start(uint32_t answer_ip)
{
    s_answer_ip = answer_ip;
    xTaskCreate(dns_task, "captive_dns", 3072, NULL, 4, NULL);
    ESP_LOGI(TAG, "Answering every name with the setup page's address");
}

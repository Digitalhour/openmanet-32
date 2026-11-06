/* SPDX-License-Identifier: 0BSD OR GPL-3.0-or-later */
#include "speedtest.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "lwip/inet.h"

static const char *TAG = "speedtest";

#define READ_BUF 4096

static struct speedtest_status s_st;
static SemaphoreHandle_t s_lock;

static void lock(void)
{
    if (s_lock == NULL)
    {
        s_lock = xSemaphoreCreateMutex();
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
}

static void unlock(void)
{
    xSemaphoreGive(s_lock);
}

static void finish(bool ok, const char *why)
{
    lock();
    s_st.state = ok ? SPEEDTEST_DONE : SPEEDTEST_FAILED;
    strlcpy(s_st.message, why ? why : "", sizeof(s_st.message));
    struct speedtest_status st = s_st;
    unlock();
    double mbps = st.elapsed_ms ? (double)st.bytes * 8 / st.elapsed_ms / 1000 : 0;
    if (ok)
    {
        ESP_LOGI(TAG, "%s: %llu bytes in %lu ms = %.2f Mbit/s (%lu downloads)", st.url, (unsigned long long)st.bytes,
                 (unsigned long)st.elapsed_ms, mbps, (unsigned long)st.requests);
    }
    else
    {
        ESP_LOGW(TAG, "%s failed after %llu bytes: %s", st.url, (unsigned long long)st.bytes, st.message);
    }
}

/* Downloads the file over and over until the time is up, counting every byte. A new
 * connection per download keeps it simple; the file is big enough (OpenMANET's default is
 * 221 KB) that the TCP handshake hardly shows. */
static void run_task(void *arg)
{
    (void)arg;
    char url[sizeof(s_st.url)];
    lock();
    strlcpy(url, s_st.url, sizeof(url));
    uint32_t seconds = s_st.seconds;
    unlock();

    char *buf = malloc(READ_BUF);
    if (buf == NULL)
    {
        finish(false, "Out of memory");
        vTaskDelete(NULL);
        return;
    }
    const int64_t start = esp_timer_get_time();
    const int64_t end = start + (int64_t)seconds * 1000000;
    const char *why = NULL;
    while (why == NULL && esp_timer_get_time() < end)
    {
        esp_http_client_config_t cfg = {
            .url = url,
            .timeout_ms = 8000,
            .buffer_size = 2048,
            .disable_auto_redirect = true,
        };
        esp_http_client_handle_t c = esp_http_client_init(&cfg);
        if (c == NULL)
        {
            why = "Out of memory";
            break;
        }
        if (esp_http_client_open(c, 0) != ESP_OK)
        {
            why = "Could not connect to the node";
        }
        else
        {
            esp_http_client_fetch_headers(c);
            int status = esp_http_client_get_status_code(c);
            if (status != 200)
            {
                why = status == 404 ? "The node has no such file (404)" : "The node did not send the file";
            }
            else
            {
                int n;
                while ((n = esp_http_client_read(c, buf, READ_BUF)) > 0)
                {
                    int64_t now = esp_timer_get_time();
                    lock();
                    s_st.bytes += (uint64_t)n;
                    s_st.elapsed_ms = (uint32_t)((now - start) / 1000);
                    unlock();
                    if (now >= end)
                    {
                        break;
                    }
                }
                if (n < 0)
                {
                    why = "The download broke off";
                }
                else
                {
                    lock();
                    s_st.requests++;
                    unlock();
                }
            }
            esp_http_client_close(c);
        }
        esp_http_client_cleanup(c);
    }
    free(buf);
    lock();
    bool got = s_st.bytes > 0;
    unlock();
    /* A download that broke off near the end still measured something; only fail outright
     * when nothing arrived. */
    finish(got || why == NULL, got ? NULL : why);
    vTaskDelete(NULL);
}

static bool path_ok(const char *p)
{
    if (p == NULL || p[0] != '/' || strlen(p) > 100)
    {
        return false;
    }
    for (; *p; p++)
    {
        if ((unsigned char)*p <= 0x20 || (unsigned char)*p >= 0x7f || *p == '"' || *p == '\\')
        {
            return false;
        }
    }
    return true;
}

esp_err_t speedtest_start(const char *ip, const char *path, uint32_t seconds)
{
    struct in_addr a;
    if (ip == NULL || inet_aton(ip, &a) == 0 || !path_ok(path) || seconds < 2 || seconds > 60)
    {
        return ESP_ERR_INVALID_ARG;
    }
    lock();
    if (s_st.state == SPEEDTEST_RUNNING)
    {
        unlock();
        return ESP_ERR_INVALID_STATE;
    }
    memset(&s_st, 0, sizeof(s_st));
    snprintf(s_st.url, sizeof(s_st.url), "http://%s%s", inet_ntoa(a), path);
    s_st.seconds = seconds;
    s_st.state = SPEEDTEST_RUNNING;
    unlock();
    if (xTaskCreate(run_task, "speedtest", 6144, NULL, 4, NULL) != pdPASS)
    {
        finish(false, "Could not start the test");
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "Downloading %s for %lu s", s_st.url, (unsigned long)seconds);
    return ESP_OK;
}

void speedtest_get(struct speedtest_status *out)
{
    lock();
    *out = s_st;
    unlock();
}

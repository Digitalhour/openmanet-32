/* SPDX-License-Identifier: 0BSD OR GPL-3.0-or-later */
/*
 * Speed test: the meshpoint downloads a file from another node's web server over the mesh
 * for a few seconds and times it. Every OpenMANET node serves its web page (uhttpd), so this
 * needs nothing installed on the node. It measures the HaLow path from that node to this
 * meshpoint, through any nodes batman-adv routes it over.
 *
 * Runs in its own task; the dashboard starts it and polls the progress.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

enum speedtest_state
{
    SPEEDTEST_IDLE,
    SPEEDTEST_RUNNING,
    SPEEDTEST_DONE,
    SPEEDTEST_FAILED,
};

struct speedtest_status
{
    enum speedtest_state state;
    char url[160];
    uint64_t bytes;       /* received so far */
    uint32_t elapsed_ms;  /* from the first request to the last byte so far */
    uint32_t seconds;     /* how long the test runs */
    uint32_t requests;    /* downloads completed */
    char message[80];     /* why it failed */
};

/* Starts a test of the given length against http://<ip><path>. ESP_ERR_INVALID_STATE if one
 * is already running, ESP_ERR_INVALID_ARG for a bad address or path. */
esp_err_t speedtest_start(const char *ip, const char *path, uint32_t seconds);

void speedtest_get(struct speedtest_status *out);

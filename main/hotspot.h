/* SPDX-License-Identifier: 0BSD OR GPL-3.0-or-later */
/*
 * The 2.4 GHz hotspot and its settings page.
 *
 * Phones and laptops join the hotspot, get an address from it, and reach the mesh through
 * NAT: their traffic leaves on bat0 with this node's mesh address. The page at the hotspot's
 * address shows the mesh and changes the settings (settings.h); a save restarts the node.
 */
#pragma once

#include "esp_netif.h"
#include "settings.h"

/* Before the HaLow radio starts: sets up and calibrates the 2.4 GHz radio, then leaves it
 * off so the two radios do not start up at the same moment on USB power. */
void hotspot_prepare(const struct hop_settings *running);

/* Once bat0 is up: turns the hotspot on, routes its clients into the mesh through NAT, and
 * starts the page. */
void hotspot_start(esp_netif_t *mesh_netif);

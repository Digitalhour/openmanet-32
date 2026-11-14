/* SPDX-License-Identifier: 0BSD OR GPL-3.0-or-later */
#pragma once

#include "esp_netif.h"

/* After BATMAN and the hotspot start, discover native OpenMANET gateways without
 * changing the static mesh address. */
void gateway_start(esp_netif_t *netif);

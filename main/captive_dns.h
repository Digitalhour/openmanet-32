/* SPDX-License-Identifier: 0BSD OR GPL-3.0-or-later */
#pragma once

#include <stdint.h>

/* Starts a DNS server on port 53 that answers every A query with answer_ip (network byte
 * order). For setup mode, so phones open the setup page by themselves. */
void captive_dns_start(uint32_t answer_ip);

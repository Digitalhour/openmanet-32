/* SPDX-License-Identifier: 0BSD OR GPL-3.0-or-later */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define GATEWAY_ALFRED_PORT 16962
#define GATEWAY_ALFRED_TYPE 100

struct gateway_record
{
    uint8_t mac[6];
    uint8_t ip[4];
};

/* Missing/withdrawn gateway TVLVs return zero bandwidth. Malformed TLVs return false. */
bool gateway_parse_tvlvs(const uint8_t *data, size_t len, uint32_t *down, uint32_t *up);

/* ALFRED PUSH_DATA v0, containing OpenMANET Gateway protobuf v1 records.
 * Returns -1 for malformed data, 0 for no matching record, 1 for a matching record.
 * Output is only written after the entire datagram has been validated. */
int gateway_parse_alfred(const uint8_t *data, size_t len, uint16_t transaction,
                        const uint8_t wanted_mac[6], struct gateway_record *out);

/* SPDX-License-Identifier: 0BSD OR GPL-3.0-or-later */
/*
 * The HaLow radio settings a person can choose on the page, following OpenMANET's own
 * choices (LuCI's HaLow channel widget and netifd's morse.sh on OpenMANET 1.8.0):
 *
 * - the channel, from the country's channel table. The firmware's table (Morse Micro's
 *   regulatory database) holds the same rows as OpenMANET's /usr/share/morse-regdb/channels.csv:
 *   channel number, centre frequency, width (1, 2, 4 or 8 MHz) and operating class. The width
 *   and operating class follow from the channel, as on OpenMANET.
 * - the primary channel width: auto, 1 MHz or 2 MHz. Auto is 2 MHz on 4 and 8 MHz channels,
 *   1 MHz otherwise (morse.sh).
 * - the primary 1 MHz channel index: auto or 0 to width - 1. Auto is (width - 1) / 2
 *   (morse.sh). Only indices whose primary channel is a channel of the country are allowed
 *   (LuCI's allowedS1gChanIndex).
 * - the beacon interval (OpenMANET's mesh default is 1000 TU) and a transmit power cap.
 *
 * Plus the 2.4 GHz hotspot channel. The country stays a build setting.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "mmwlan.h"

#define RADIO_AUTO 0          /* prim_bw_mhz: auto */
#define RADIO_IDX_AUTO (-1)   /* prim_idx: auto */

struct hop_radio
{
    uint8_t s1g_chan;     /* S1G channel number */
    uint8_t prim_bw_mhz;  /* RADIO_AUTO, 1 or 2 */
    int8_t prim_idx;      /* RADIO_IDX_AUTO or 0 to width - 1 */
    uint16_t beacon_tus;  /* beacon interval */
    uint8_t tx_dbm;       /* HaLow transmit power cap; 0 = the driver's (regulatory) maximum */
    uint8_t ap_chan;      /* 2.4 GHz hotspot channel */
};

/* The build's defaults (Kconfig, sdkconfig.openmanet). */
void radio_defaults(struct hop_radio *r);

/* The channel table for the build's country, or NULL. */
const struct mmwlan_s1g_channel_list *radio_channels(void);

/* The table row for a channel number, or NULL. */
const struct mmwlan_s1g_channel *radio_find(uint8_t s1g_chan);

/* True if r can be used; otherwise a short reason for the person in why. */
bool radio_valid(const struct hop_radio *r, const char **why);

/* Whether a primary 1 MHz index is allowed for a channel and primary width (1 or 2 MHz). */
bool radio_prim_idx_allowed(const struct mmwlan_s1g_channel *ch, uint8_t prim_bw_mhz, int idx);

/* The primary width and index actually used, with auto worked out as OpenMANET does. */
void radio_resolve(const struct hop_radio *r, uint8_t *prim_bw_mhz, uint8_t *prim_idx);

/* The highest 2.4 GHz channel for the build's country (11 in the US, else 13). */
int radio_ap_max_chan(void);

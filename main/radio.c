/* SPDX-License-Identifier: 0BSD OR GPL-3.0-or-later */
#include "radio.h"

#include <string.h>

#include "mmregdb.h"
#include "sdkconfig.h"

void radio_defaults(struct hop_radio *r)
{
    memset(r, 0, sizeof(*r));
    r->s1g_chan = CONFIG_HOP_S1G_CHANNEL;
    r->prim_bw_mhz = CONFIG_HOP_PRI_BW_MHZ;
    r->prim_idx = CONFIG_HOP_PRI_1MHZ_IDX;
    r->beacon_tus = CONFIG_HOP_BEACON_TUS;
    r->tx_dbm = CONFIG_HOP_TX_DBM;
    r->ap_chan = CONFIG_HOP_AP_CHANNEL;
}

const struct mmwlan_s1g_channel_list *radio_channels(void)
{
    return mmwlan_lookup_regulatory_domain(get_regulatory_db(), CONFIG_HALOW_COUNTRY_CODE);
}

const struct mmwlan_s1g_channel *radio_find(uint8_t s1g_chan)
{
    const struct mmwlan_s1g_channel_list *list = radio_channels();
    for (unsigned i = 0; list != NULL && i < list->num_channels; i++)
    {
        if (list->channels[i].s1g_chan_num == s1g_chan)
        {
            return &list->channels[i];
        }
    }
    return NULL;
}

/* LuCI's allowedS1gChanIndex: the primary channel's centre must be the centre of a channel in
 * the country's table. A 1 MHz channel has only index 0. */
bool radio_prim_idx_allowed(const struct mmwlan_s1g_channel *ch, uint8_t prim_bw_mhz, int idx)
{
    if (ch == NULL || idx < 0 || idx >= ch->bw_mhz)
    {
        return false;
    }
    if (ch->bw_mhz == 1)
    {
        return true;
    }
    /* Offsets in kHz from the channel centre, as LuCI computes them in MHz. */
    int32_t half = ch->bw_mhz * 500;
    int32_t offset_khz;
    if (prim_bw_mhz == 2)
    {
        offset_khz = -half + 1000 + (idx / 2) * 2000;
    }
    else if (prim_bw_mhz == 1)
    {
        offset_khz = -half + 500 + idx * 1000;
    }
    else
    {
        return false;
    }
    uint32_t want_khz = ch->centre_freq_hz / 1000 + offset_khz;
    const struct mmwlan_s1g_channel_list *list = radio_channels();
    for (unsigned i = 0; list != NULL && i < list->num_channels; i++)
    {
        if (list->channels[i].centre_freq_hz / 1000 == want_khz)
        {
            return true;
        }
    }
    return false;
}

void radio_resolve(const struct hop_radio *r, uint8_t *prim_bw_mhz, uint8_t *prim_idx)
{
    const struct mmwlan_s1g_channel *ch = radio_find(r->s1g_chan);
    uint8_t bw = ch != NULL ? ch->bw_mhz : 1;
    /* morse.sh: an unset primary width is 2 MHz on 4 and 8 MHz channels, else 1 MHz, and an
     * unset (auto) index is (width - 1) / 2. */
    *prim_bw_mhz = r->prim_bw_mhz != RADIO_AUTO ? r->prim_bw_mhz : (bw == 4 || bw == 8) ? 2 : 1;
    *prim_idx = r->prim_idx >= 0 ? (uint8_t)r->prim_idx : (uint8_t)((bw - 1) / 2);
}

int radio_ap_max_chan(void)
{
    return strcmp(CONFIG_HALOW_COUNTRY_CODE, "US") == 0 ? 11 : 13;
}

bool radio_valid(const struct hop_radio *r, const char **why)
{
    const struct mmwlan_s1g_channel *ch = radio_find(r->s1g_chan);
    if (ch == NULL)
    {
        *why = "That HaLow channel is not in this country's channel list";
        return false;
    }
    if (r->prim_bw_mhz != RADIO_AUTO && (r->prim_bw_mhz > 2 || r->prim_bw_mhz > ch->bw_mhz))
    {
        *why = "The primary channel width must be auto, 1 MHz or 2 MHz, and no wider than the channel";
        return false;
    }
    if (r->prim_idx != RADIO_IDX_AUTO)
    {
        uint8_t pbw, pidx;
        radio_resolve(r, &pbw, &pidx);
        if (!radio_prim_idx_allowed(ch, pbw, r->prim_idx))
        {
            *why = "That primary channel index is not allowed for this channel and primary width";
            return false;
        }
    }
    if (r->beacon_tus < 15 || r->beacon_tus > 10000)
    {
        *why = "The beacon interval must be 15 to 10000 TU";
        return false;
    }
    if (r->tx_dbm > 30)
    {
        *why = "The transmit power cap must be 30 dBm or less";
        return false;
    }
    if (r->ap_chan < 1 || r->ap_chan > radio_ap_max_chan())
    {
        *why = "That hotspot channel is not allowed";
        return false;
    }
    return true;
}

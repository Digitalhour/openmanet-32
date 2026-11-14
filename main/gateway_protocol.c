/* SPDX-License-Identifier: 0BSD OR GPL-3.0-or-later */
#include "gateway_protocol.h"

static uint16_t read16(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[0] << 8 | p[1]);
}

static uint32_t read32(const uint8_t *p)
{
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

bool gateway_parse_tvlvs(const uint8_t *data, size_t len, uint32_t *down, uint32_t *up)
{
    uint32_t d = 0, u = 0;
    while (len != 0)
    {
        if (len < 4)
        {
            return false;
        }
        size_t size = read16(data + 2);
        if (size > len - 4)
        {
            return false;
        }
        if (data[0] == 1 && data[1] == 1 && size >= 8)
        {
            d = read32(data + 4);
            u = read32(data + 8);
            if (d == 0 || u == 0)
            {
                d = u = 0;
            }
        }
        data += size + 4;
        len -= size + 4;
    }
    *down = d;
    *up = u;
    return true;
}

static bool varint(const uint8_t **data, size_t *len, uint64_t *value)
{
    *value = 0;
    for (unsigned shift = 0; shift < 64 && *len != 0; shift += 7)
    {
        uint8_t byte = **data;
        (*data)++;
        (*len)--;
        if (shift == 63 && byte > 1)
        {
            return false;
        }
        *value |= (uint64_t)(byte & 0x7f) << shift;
        if ((byte & 0x80) == 0)
        {
            return true;
        }
    }
    return false;
}

static int hex_digit(uint8_t c)
{
    if (c >= '0' && c <= '9')
    {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f')
    {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F')
    {
        return c - 'A' + 10;
    }
    return -1;
}

static bool parse_mac(const uint8_t *data, size_t len, uint8_t mac[6])
{
    if (len != 17)
    {
        return false;
    }
    unsigned nonzero = 0;
    for (int i = 0; i < 6; i++)
    {
        int hi = hex_digit(data[i * 3]), lo = hex_digit(data[i * 3 + 1]);
        if (hi < 0 || lo < 0 || (i < 5 && data[i * 3 + 2] != ':'))
        {
            return false;
        }
        mac[i] = (uint8_t)(hi * 16 + lo);
        nonzero |= mac[i];
    }
    return nonzero != 0 && (mac[0] & 1) == 0;
}

static bool parse_ip(const uint8_t *data, size_t len, uint8_t ip[4])
{
    for (int i = 0; i < 4; i++)
    {
        unsigned value = 0, digits = 0;
        while (len != 0 && *data >= '0' && *data <= '9')
        {
            value = value * 10 + *data++ - '0';
            len--;
            if (++digits > 3 || value > 255)
            {
                return false;
            }
        }
        if (digits == 0 || (i < 3 && (len == 0 || *data++ != '.')))
        {
            return false;
        }
        if (i < 3)
        {
            len--;
        }
        ip[i] = (uint8_t)value;
    }
    return len == 0 && ip[0] != 0 && ip[0] != 127 && ip[0] < 224;
}

static bool parse_record(const uint8_t *data, size_t len, struct gateway_record *out)
{
    bool mac = false, ip = false;
    while (len != 0)
    {
        uint64_t tag, value;
        if (!varint(&data, &len, &tag) || tag >> 3 == 0 || tag >> 3 > 0x1fffffff)
        {
            return false;
        }
        unsigned wire = tag & 7;
        uint64_t field = tag >> 3;
        if ((field == 1 || field == 3) && wire != 2)
        {
            return false;
        }
        if (wire == 0)
        {
            if (!varint(&data, &len, &value))
            {
                return false;
            }
            continue;
        }
        size_t size;
        if (wire == 2)
        {
            if (!varint(&data, &len, &value) || value > len)
            {
                return false;
            }
            size = (size_t)value;
        }
        else if (wire == 1 || wire == 5)
        {
            size = wire == 1 ? 8 : 4;
            if (size > len)
            {
                return false;
            }
        }
        else
        {
            return false;
        }
        if (field == 1)
        {
            mac = parse_mac(data, size, out->mac);
            if (!mac)
            {
                return false;
            }
        }
        else if (field == 3)
        {
            ip = parse_ip(data, size, out->ip);
            if (!ip)
            {
                return false;
            }
        }
        data += size;
        len -= size;
    }
    return mac && ip;
}

int gateway_parse_alfred(const uint8_t *data, size_t len, uint16_t transaction,
                        const uint8_t wanted_mac[6], struct gateway_record *out)
{
    if (len < 8 || data[0] != 0 || data[1] != 0 || read16(data + 2) != len - 4 ||
        read16(data + 4) != transaction)
    {
        return -1;
    }
    data += 8;
    len -= 8;
    int found = 0;
    struct gateway_record matched;
    while (len != 0)
    {
        if (len < 10)
        {
            return -1;
        }
        size_t size = read16(data + 8);
        if (size > len - 10)
        {
            return -1;
        }
        if (data[6] == GATEWAY_ALFRED_TYPE && data[7] == 1)
        {
            struct gateway_record record;
            if (!parse_record(data + 10, size, &record))
            {
                return -1;
            }
            bool equal = true;
            for (int i = 0; i < 6; i++)
            {
                equal = equal && record.mac[i] == wanted_mac[i];
            }
            if (equal)
            {
                matched = record;
                found = 1;
            }
        }
        data += size + 10;
        len -= size + 10;
    }
    if (found)
    {
        *out = matched;
    }
    return found;
}

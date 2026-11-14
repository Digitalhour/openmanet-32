/* SPDX-License-Identifier: 0BSD OR GPL-3.0-or-later */
#include <stdio.h>
#include <string.h>

#include "gateway_protocol.h"

#define CHECK(expr) do { if (!(expr)) { fprintf(stderr, "Line %d: %s\n", __LINE__, #expr); return 1; } } while (0)

static const uint8_t wanted[6] = { 0x9c, 0x04, 0xb6, 0x46, 0x0a, 0x36 };

static size_t string_field(uint8_t *buf, uint8_t tag, const char *text)
{
    size_t len = strlen(text);
    buf[0] = tag;
    buf[1] = (uint8_t)len;
    memcpy(buf + 2, text, len);
    return len + 2;
}

static size_t record(uint8_t *buf, const char *mac, const char *ip)
{
    size_t len = string_field(buf, 10, mac);
    len += string_field(buf + len, 18, "gateway");
    len += string_field(buf + len, 26, ip);
    return len;
}

static size_t packet(uint8_t *buf, const char *mac, const char *ip)
{
    memset(buf, 0, 18);
    buf[4] = 0x12;
    buf[5] = 0x34;
    buf[14] = GATEWAY_ALFRED_TYPE;
    buf[15] = 1;
    size_t size = record(buf + 18, mac, ip);
    buf[16] = (uint8_t)(size >> 8);
    buf[17] = (uint8_t)size;
    buf[2] = (uint8_t)((size + 14) >> 8);
    buf[3] = (uint8_t)(size + 14);
    return size + 18;
}

static int tvlv_tests(void)
{
    uint32_t down = 123, up = 456;
    CHECK(gateway_parse_tvlvs(NULL, 0, &down, &up) && down == 0 && up == 0);
    uint8_t data[] = { 1, 1, 0, 8, 0, 1, 2, 3, 0, 0, 1, 44 };
    CHECK(gateway_parse_tvlvs(data, sizeof(data), &down, &up));
    CHECK(down == 66051 && up == 300);
    data[8] = data[9] = data[10] = data[11] = 0;
    CHECK(gateway_parse_tvlvs(data, sizeof(data), &down, &up) && down == 0 && up == 0);
    data[11] = 10;
    data[1] = 2;
    CHECK(gateway_parse_tvlvs(data, sizeof(data), &down, &up) && down == 0 && up == 0);
    data[1] = 1;
    down = 123;
    up = 456;
    for (size_t len = 1; len < sizeof(data); len++)
    {
        CHECK(!gateway_parse_tvlvs(data, len, &down, &up) && down == 123 && up == 456);
    }
    const uint8_t short_gw[] = { 1, 1, 0, 1, 0 };
    CHECK(gateway_parse_tvlvs(short_gw, sizeof(short_gw), &down, &up) && down == 0 && up == 0);
    uint8_t other_then_gw[16] = { 4, 1, 0, 0 };
    memcpy(other_then_gw + 4, data, sizeof(data));
    CHECK(gateway_parse_tvlvs(other_then_gw, sizeof(other_then_gw), &down, &up) && down == 66051 && up == 10);
    uint8_t trailing[13];
    memcpy(trailing, data, sizeof(data));
    trailing[12] = 1;
    CHECK(!gateway_parse_tvlvs(trailing, sizeof(trailing), &down, &up));
    puts("PASS: gateway TVLV endian decoding, withdrawal, versions and truncation");
    return 0;
}

static int alfred_tests(void)
{
    uint8_t buf[512];
    size_t len = packet(buf, "9c:04:b6:46:0a:36", "10.41.78.134");
    struct gateway_record out;
    CHECK(gateway_parse_alfred(buf, len, 0x1234, wanted, &out) == 1);
    CHECK(memcmp(out.mac, wanted, 6) == 0);
    const uint8_t expected[] = { 10, 41, 78, 134 };
    CHECK(memcmp(out.ip, expected, 4) == 0);
    CHECK(gateway_parse_alfred(buf, len, 0xabcd, wanted, &out) == -1);
    buf[1] = 1;
    CHECK(gateway_parse_alfred(buf, len, 0x1234, wanted, &out) == -1);
    buf[1] = 0;
    for (size_t n = 0; n < len; n++)
    {
        memset(&out, 0xa5, sizeof(out));
        CHECK(gateway_parse_alfred(buf, n, 0x1234, wanted, &out) == -1);
        CHECK(out.mac[0] == 0xa5 && out.ip[0] == 0xa5);
    }
    buf[15] = 2;
    CHECK(gateway_parse_alfred(buf, len, 0x1234, wanted, &out) == 0);
    buf[15] = 1;
    buf[14] = 102;
    CHECK(gateway_parse_alfred(buf, len, 0x1234, wanted, &out) == 0);
    len = packet(buf, "9c:04:b6:46:0a:37", "10.41.78.134");
    CHECK(gateway_parse_alfred(buf, len, 0x1234, wanted, &out) == 0);
    const char *invalid_ips[] = { "10.41.78.256", "10.41.78", "10..78.134", "10.41.78.134x",
                                  "0.0.0.0", "127.0.0.1", "224.0.0.1", "255.255.255.255", "fe80::1" };
    for (size_t i = 0; i < sizeof(invalid_ips) / sizeof(invalid_ips[0]); i++)
    {
        len = packet(buf, "9c:04:b6:46:0a:36", invalid_ips[i]);
        CHECK(gateway_parse_alfred(buf, len, 0x1234, wanted, &out) == -1);
    }
    const char *invalid_macs[] = { "", "9c:04:b6:46:0a", "9c:04:b6:46:0a:zz", "9d:04:b6:46:0a:36", "00:00:00:00:00:00" };
    for (size_t i = 0; i < sizeof(invalid_macs) / sizeof(invalid_macs[0]); i++)
    {
        len = packet(buf, invalid_macs[i], "10.41.78.134");
        CHECK(gateway_parse_alfred(buf, len, 0x1234, wanted, &out) == -1);
    }
    len = packet(buf, "9C:04:B6:46:0A:36", "10.41.78.135");
    CHECK(gateway_parse_alfred(buf, len, 0x1234, wanted, &out) == 1 && out.ip[3] == 135);

    /* A future protobuf field must not prevent decoding the current fields. */
    const uint8_t extra[] = { 0x20, 0x81, 0x01, 0x29, 0, 0, 0, 0, 0, 0, 0, 0,
                              0x35, 0, 0, 0, 0, 0x3a, 2, 1, 2 };
    memcpy(buf + len, extra, sizeof(extra));
    len += sizeof(extra);
    buf[3] = (uint8_t)(len - 4);
    buf[17] = (uint8_t)(len - 18);
    CHECK(gateway_parse_alfred(buf, len, 0x1234, wanted, &out) == 1);
    buf[len++] = 0xff;
    buf[3] = (uint8_t)(len - 4);
    buf[17] = (uint8_t)(len - 18);
    CHECK(gateway_parse_alfred(buf, len, 0x1234, wanted, &out) == -1);

    len = packet(buf, "9c:04:b6:46:0a:36", "10.41.78.134");
    /* A malformed trailing ALFRED block invalidates the entire packet, atomically. */
    buf[len++] = 0;
    buf[3] = (uint8_t)(len - 4);
    memset(&out, 0xa5, sizeof(out));
    CHECK(gateway_parse_alfred(buf, len, 0x1234, wanted, &out) == -1 && out.ip[0] == 0xa5);

    len = packet(buf, "9c:04:b6:46:0a:37", "10.41.78.134");
    for (int i = 0; i < 5; i++)
    {
        memset(buf + len, 0, 10);
        buf[len + 6] = GATEWAY_ALFRED_TYPE;
        buf[len + 7] = 1;
        size_t size = record(buf + len + 10, "9c:04:b6:46:0a:36", "10.41.78.135");
        buf[len + 9] = (uint8_t)size;
        len += size + 10;
    }
    buf[2] = (uint8_t)((len - 4) >> 8);
    buf[3] = (uint8_t)(len - 4);
    CHECK(len > 255);
    CHECK(gateway_parse_alfred(buf, len, 0x1234, wanted, &out) == 1 && out.ip[3] == 135);
    puts("PASS: ALFRED type 100/version 1, protobuf, MAC matching, changing IP and malformed packets");
    return 0;
}

static uint32_t random_value(uint32_t *state)
{
    *state ^= *state << 13;
    *state ^= *state >> 17;
    *state ^= *state << 5;
    return *state;
}

static int mutation_tests(void)
{
    uint32_t state = 0x12345678;
    uint8_t buf[512];
    struct gateway_record out;
    for (unsigned trial = 0; trial < 50000; trial++)
    {
        size_t len = packet(buf, "9c:04:b6:46:0a:36", "10.41.78.134");
        unsigned changes = random_value(&state) % 8 + 1;
        for (unsigned i = 0; i < changes; i++)
        {
            buf[random_value(&state) % len] ^= (uint8_t)random_value(&state);
        }
        memset(&out, 0xa5, sizeof(out));
        int result = gateway_parse_alfred(buf, len, 0x1234, wanted, &out);
        CHECK(result >= -1 && result <= 1);
        if (result != 1)
        {
            CHECK(out.mac[0] == 0xa5 && out.ip[0] == 0xa5);
        }
        uint32_t down, up;
        gateway_parse_tvlvs(buf, random_value(&state) % len, &down, &up);
    }
    puts("PASS: 50,000 deterministic mutated packets; failed parses leave output unchanged");
    return 0;
}

int main(void)
{
    return tvlv_tests() || alfred_tests() || mutation_tests();
}

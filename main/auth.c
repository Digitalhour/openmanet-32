/* SPDX-License-Identifier: 0BSD OR GPL-3.0-or-later */
#include "auth.h"

#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "mbedtls/md.h"
#include "mbedtls/pkcs5.h"
#include "sdkconfig.h"

static const char *TAG = "auth";

/* The stored form: iteration count (4 bytes, little endian), salt, derived key. Keeping the
 * count lets it change later without locking out boards that saved the old one. */
#define SALT_LEN 16
#define KEY_LEN 32
#define ITERATIONS 4000   /* about 0.5 s on the S3; 10000 made sign-in take 1.6 s */
_Static_assert(4 + SALT_LEN + KEY_LEN == SETTINGS_ROOT_HASH_LEN, "root hash layout");

#define MAX_SESSIONS 4
#define IDLE_US (30LL * 60 * 1000000)
#define MAX_FAILS 5
#define LOCK_US (30LL * 1000000)

static uint8_t s_hash[SETTINGS_ROOT_HASH_LEN];
static bool s_have_hash;   /* false: setup mode, the default password signs in */

struct session
{
    char token[AUTH_TOKEN_LEN + 1];
    int64_t last_us;
};
static struct session s_sessions[MAX_SESSIONS];
static int s_fails;
static int64_t s_locked_until_us;
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

/* Compares without stopping at the first difference, so the time taken says nothing about
 * how much of a guess was right. */
static bool same(const uint8_t *a, const uint8_t *b, size_t n)
{
    uint8_t d = 0;
    for (size_t i = 0; i < n; i++)
    {
        d |= a[i] ^ b[i];
    }
    return d == 0;
}

static bool derive(const char *password, const uint8_t *salt, uint32_t iterations, uint8_t key[KEY_LEN])
{
    return mbedtls_pkcs5_pbkdf2_hmac_ext(MBEDTLS_MD_SHA256, (const unsigned char *)password, strlen(password), salt,
                                         SALT_LEN, iterations, KEY_LEN, key) == 0;
}

void auth_init(const uint8_t *root_hash)
{
    lock();
    s_have_hash = root_hash != NULL;
    if (s_have_hash)
    {
        memcpy(s_hash, root_hash, sizeof(s_hash));
    }
    unlock();
}

void auth_set_hash(const uint8_t root_hash[SETTINGS_ROOT_HASH_LEN])
{
    auth_init(root_hash);
}

bool auth_hash(const char *password, uint8_t out[SETTINGS_ROOT_HASH_LEN])
{
    uint32_t it = ITERATIONS;
    out[0] = it & 0xff;
    out[1] = (it >> 8) & 0xff;
    out[2] = (it >> 16) & 0xff;
    out[3] = (it >> 24) & 0xff;
    esp_fill_random(out + 4, SALT_LEN);
    int64_t t0 = esp_timer_get_time();
    bool ok = derive(password, out + 4, it, out + 4 + SALT_LEN);
    ESP_LOGI(TAG, "Root password hashed in %d ms", (int)((esp_timer_get_time() - t0) / 1000));
    return ok;
}

static bool password_matches(const char *password)
{
    if (!s_have_hash)
    {
        const char *def = CONFIG_HOP_ROOT_DEFAULT_PASSWORD;
        size_t n = strlen(def);
        return strlen(password) == n && same((const uint8_t *)password, (const uint8_t *)def, n);
    }
    uint32_t it = s_hash[0] | s_hash[1] << 8 | s_hash[2] << 16 | (uint32_t)s_hash[3] << 24;
    if (it == 0 || it > 1000000)
    {
        return false;
    }
    uint8_t key[KEY_LEN];
    bool ok = derive(password, s_hash + 4, it, key) && same(key, s_hash + 4 + SALT_LEN, KEY_LEN);
    memset(key, 0, sizeof(key));
    return ok;
}

enum auth_result auth_check(const char *user, const char *password, int *wait_s)
{
    lock();
    int64_t now = esp_timer_get_time();
    if (now < s_locked_until_us)
    {
        *wait_s = (int)((s_locked_until_us - now) / 1000000) + 1;
        unlock();
        return AUTH_LOCKED;
    }
    /* The hash is worked out even for a wrong user name, so the time taken does not say
     * whether the name was right. */
    bool ok = password_matches(password);
    ok = strcmp(user, AUTH_USER) == 0 && ok;
    if (ok)
    {
        s_fails = 0;
    }
    else if (++s_fails >= MAX_FAILS)
    {
        s_fails = 0;
        s_locked_until_us = now + LOCK_US;
        ESP_LOGW(TAG, "%d wrong sign-ins: locked for %d s", MAX_FAILS, (int)(LOCK_US / 1000000));
    }
    unlock();
    return ok ? AUTH_OK : AUTH_WRONG;
}

static bool printable(const char *s)
{
    for (; *s; s++)
    {
        if ((unsigned char)*s < 0x20 || (unsigned char)*s == 0x7f)
        {
            return false;
        }
    }
    return true;
}

bool auth_password_acceptable(const char *password, const char **why)
{
    size_t n = strlen(password);
    if (n < 8 || n > 63 || !printable(password))
    {
        *why = "The root password must be 8 to 63 characters";
        return false;
    }
    if (strcmp(password, CONFIG_HOP_ROOT_DEFAULT_PASSWORD) == 0)
    {
        *why = "Choose a root password other than the default one";
        return false;
    }
    return true;
}

void auth_session_open(char token[AUTH_TOKEN_LEN + 1])
{
    uint8_t raw[AUTH_TOKEN_LEN / 2];
    esp_fill_random(raw, sizeof(raw));
    for (size_t i = 0; i < sizeof(raw); i++)
    {
        snprintf(token + 2 * i, 3, "%02x", raw[i]);
    }
    memset(raw, 0, sizeof(raw));

    lock();
    /* An empty or expired slot, else the one idle longest. */
    int64_t now = esp_timer_get_time();
    int pick = 0;
    for (int i = 0; i < MAX_SESSIONS; i++)
    {
        if (s_sessions[i].token[0] == '\0' || now - s_sessions[i].last_us > IDLE_US)
        {
            pick = i;
            break;
        }
        if (s_sessions[i].last_us < s_sessions[pick].last_us)
        {
            pick = i;
        }
    }
    strlcpy(s_sessions[pick].token, token, sizeof(s_sessions[pick].token));
    s_sessions[pick].last_us = now;
    unlock();
}

static struct session *find(const char *token)
{
    if (token == NULL || strlen(token) != AUTH_TOKEN_LEN)
    {
        return NULL;
    }
    for (int i = 0; i < MAX_SESSIONS; i++)
    {
        if (s_sessions[i].token[0] != '\0' &&
            same((const uint8_t *)s_sessions[i].token, (const uint8_t *)token, AUTH_TOKEN_LEN))
        {
            return &s_sessions[i];
        }
    }
    return NULL;
}

bool auth_session_valid(const char *token)
{
    lock();
    struct session *s = find(token);
    int64_t now = esp_timer_get_time();
    bool ok = s != NULL && now - s->last_us <= IDLE_US;
    if (ok)
    {
        s->last_us = now;
    }
    else if (s != NULL)
    {
        memset(s, 0, sizeof(*s));
    }
    unlock();
    return ok;
}

void auth_session_close(const char *token)
{
    lock();
    struct session *s = find(token);
    if (s != NULL)
    {
        memset(s, 0, sizeof(*s));
    }
    unlock();
}

void auth_session_close_all(void)
{
    lock();
    memset(s_sessions, 0, sizeof(s_sessions));
    unlock();
}

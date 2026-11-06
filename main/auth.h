/* SPDX-License-Identifier: 0BSD OR GPL-3.0-or-later */
/*
 * Sign-in for the hotspot page. One account, root. Until setup is finished its password is
 * the default (CONFIG_HOP_ROOT_DEFAULT_PASSWORD, "openmanet"); setup makes the owner choose a
 * new one, kept in NVS only as a salted PBKDF2-HMAC-SHA256 hash. A successful sign-in gets a
 * random session token in a cookie; sessions live in RAM and end after 30 idle minutes or a
 * restart. Five wrong passwords in a row lock sign-in for 30 seconds. Passwords are never
 * logged.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "settings.h"

#define AUTH_USER "root"
#define AUTH_TOKEN_LEN 32   /* hex characters */

/* Which password signs in: the saved hash, or NULL for the default (setup mode). */
void auth_init(const uint8_t *root_hash);

/* Result of a sign-in attempt. */
enum auth_result
{
    AUTH_OK,
    AUTH_WRONG,
    AUTH_LOCKED,
};

/* Checks a user name and password; on AUTH_LOCKED, *wait_s says for how long. */
enum auth_result auth_check(const char *user, const char *password, int *wait_s);

/* True if password would do as a new root password; otherwise a short reason in why. */
bool auth_password_acceptable(const char *password, const char **why);

/* Salted hash of password into out, in the form settings_save keeps. */
bool auth_hash(const char *password, uint8_t out[SETTINGS_ROOT_HASH_LEN]);

/* Switches sign-in to a new hash (after a password change). */
void auth_set_hash(const uint8_t root_hash[SETTINGS_ROOT_HASH_LEN]);

/* Opens a session; token gets AUTH_TOKEN_LEN hex characters and a terminator. */
void auth_session_open(char token[AUTH_TOKEN_LEN + 1]);

/* True if token is a live session (and refreshes its idle timer). */
bool auth_session_valid(const char *token);

void auth_session_close(const char *token);

/* Ends every session, for a password change. */
void auth_session_close_all(void);

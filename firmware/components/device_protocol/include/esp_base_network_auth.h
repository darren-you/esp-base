// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define EBASE_MANAGEMENT_KEY_BYTES 32u
#define EBASE_MANAGEMENT_TAG_BYTES 32u

/* The transport supplies a separate tag and the exact received UTF-8 request
 * bytes. Verification must precede JSON parsing, request admission, or any
 * response containing a request ID. PSA owns the HMAC implementation. */
bool ebase_management_authenticate(
    const uint8_t key[EBASE_MANAGEMENT_KEY_BYTES],
    const uint8_t tag[EBASE_MANAGEMENT_TAG_BYTES],
    const uint8_t *request, size_t request_length);

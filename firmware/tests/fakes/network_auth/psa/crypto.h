// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <stddef.h>
#include <stdint.h>

typedef int psa_status_t;
typedef uint32_t psa_key_id_t;
typedef struct {
    uint32_t type;
    uint32_t bits;
    uint32_t usage;
    uint32_t algorithm;
} psa_key_attributes_t;

#define PSA_SUCCESS 0
#define PSA_ERROR_INVALID_SIGNATURE (-149)
#define PSA_ERROR_INSUFFICIENT_MEMORY (-141)
#define PSA_KEY_ATTRIBUTES_INIT {0}
#define PSA_KEY_TYPE_HMAC 0x1100u
#define PSA_KEY_USAGE_VERIFY_MESSAGE 0x1000u
#define PSA_ALG_SHA_256 0x02000009u
#define PSA_ALG_HMAC(algorithm) (0x03800000u | (algorithm))

void psa_set_key_type(psa_key_attributes_t *attributes, uint32_t type);
void psa_set_key_bits(psa_key_attributes_t *attributes, uint32_t bits);
void psa_set_key_usage_flags(psa_key_attributes_t *attributes, uint32_t usage);
void psa_set_key_algorithm(psa_key_attributes_t *attributes, uint32_t algorithm);
void psa_reset_key_attributes(psa_key_attributes_t *attributes);
psa_status_t psa_import_key(const psa_key_attributes_t *attributes, const uint8_t *data,
                            size_t length, psa_key_id_t *key_id);
psa_status_t psa_mac_verify(psa_key_id_t key_id, uint32_t algorithm,
                            const uint8_t *input, size_t input_length,
                            const uint8_t *tag, size_t tag_length);
psa_status_t psa_destroy_key(psa_key_id_t key_id);

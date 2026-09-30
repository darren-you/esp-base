// SPDX-License-Identifier: Apache-2.0
#include "esp_base_network_auth.h"

#include "psa/crypto.h"

static void wipe_tag(uint8_t tag[EBASE_MANAGEMENT_TAG_BYTES])
{
    volatile uint8_t *bytes = tag;
    for (size_t i = 0; i < EBASE_MANAGEMENT_TAG_BYTES; ++i) bytes[i] = 0;
}

bool ebase_management_sign(
    const uint8_t key[EBASE_MANAGEMENT_KEY_BYTES],
    const uint8_t *message, size_t message_length,
    uint8_t tag[EBASE_MANAGEMENT_TAG_BYTES])
{
    if (!tag) return false;
    wipe_tag(tag);
    if (!key || !message || !message_length || message_length > 4096) return false;

    psa_key_attributes_t attributes = PSA_KEY_ATTRIBUTES_INIT;
    psa_set_key_type(&attributes, PSA_KEY_TYPE_HMAC);
    psa_set_key_bits(&attributes, EBASE_MANAGEMENT_KEY_BYTES * 8);
    psa_set_key_usage_flags(&attributes, PSA_KEY_USAGE_SIGN_MESSAGE);
    psa_set_key_algorithm(&attributes, PSA_ALG_HMAC(PSA_ALG_SHA_256));

    psa_key_id_t key_id = 0;
    const psa_status_t imported = psa_import_key(&attributes, key, EBASE_MANAGEMENT_KEY_BYTES, &key_id);
    psa_reset_key_attributes(&attributes);
    if (imported != PSA_SUCCESS) return false;

    size_t tag_length = 0;
    const psa_status_t computed = psa_mac_compute(
        key_id, PSA_ALG_HMAC(PSA_ALG_SHA_256), message, message_length,
        tag, EBASE_MANAGEMENT_TAG_BYTES, &tag_length);
    const psa_status_t destroyed = psa_destroy_key(key_id);
    if (computed != PSA_SUCCESS || tag_length != EBASE_MANAGEMENT_TAG_BYTES ||
        destroyed != PSA_SUCCESS) {
        wipe_tag(tag);
        return false;
    }
    return true;
}

bool ebase_management_authenticate(
    const uint8_t key[EBASE_MANAGEMENT_KEY_BYTES],
    const uint8_t tag[EBASE_MANAGEMENT_TAG_BYTES],
    const uint8_t *request, size_t request_length)
{
    if (!key || !tag || !request || !request_length || request_length > 4096) return false;

    psa_key_attributes_t attributes = PSA_KEY_ATTRIBUTES_INIT;
    psa_set_key_type(&attributes, PSA_KEY_TYPE_HMAC);
    psa_set_key_bits(&attributes, EBASE_MANAGEMENT_KEY_BYTES * 8);
    psa_set_key_usage_flags(&attributes, PSA_KEY_USAGE_VERIFY_MESSAGE);
    psa_set_key_algorithm(&attributes, PSA_ALG_HMAC(PSA_ALG_SHA_256));

    psa_key_id_t key_id = 0;
    const psa_status_t imported = psa_import_key(&attributes, key, EBASE_MANAGEMENT_KEY_BYTES, &key_id);
    psa_reset_key_attributes(&attributes);
    if (imported != PSA_SUCCESS) return false;

    const psa_status_t verified = psa_mac_verify(
        key_id, PSA_ALG_HMAC(PSA_ALG_SHA_256), request, request_length,
        tag, EBASE_MANAGEMENT_TAG_BYTES);
    const psa_status_t destroyed = psa_destroy_key(key_id);
    return verified == PSA_SUCCESS && destroyed == PSA_SUCCESS;
}

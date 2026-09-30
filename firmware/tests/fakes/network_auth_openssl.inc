// SPDX-License-Identifier: Apache-2.0
/* Test-only PSA HMAC port shared by the two explicit OpenSSL probes. */
static uint8_t imported_key[32];
static bool key_present;
static uint32_t imported_usage;

void psa_set_key_type(psa_key_attributes_t *a, uint32_t v) { a->type = v; }
void psa_set_key_bits(psa_key_attributes_t *a, uint32_t v) { a->bits = v; }
void psa_set_key_usage_flags(psa_key_attributes_t *a, uint32_t v) { a->usage = v; }
void psa_set_key_algorithm(psa_key_attributes_t *a, uint32_t v) { a->algorithm = v; }
void psa_reset_key_attributes(psa_key_attributes_t *a) { memset(a, 0, sizeof *a); }

psa_status_t psa_import_key(const psa_key_attributes_t *a, const uint8_t *data,
                          size_t length, psa_key_id_t *id)
{
    if (key_present || length != sizeof imported_key ||
        a->type != PSA_KEY_TYPE_HMAC || a->bits != 256U ||
        (a->usage != PSA_KEY_USAGE_VERIFY_MESSAGE && a->usage != PSA_KEY_USAGE_SIGN_MESSAGE) ||
        a->algorithm != PSA_ALG_HMAC(PSA_ALG_SHA_256))
        return PSA_ERROR_INSUFFICIENT_MEMORY;
    memcpy(imported_key, data, length);
    key_present = true;
    imported_usage = a->usage;
    *id = 1;
    return PSA_SUCCESS;
}

psa_status_t psa_mac_verify(psa_key_id_t id, uint32_t algorithm,
                          const uint8_t *input, size_t input_length,
                          const uint8_t *tag, size_t tag_length)
{
    if (!key_present || id != 1U || algorithm != PSA_ALG_HMAC(PSA_ALG_SHA_256) ||
        tag_length != 32U || imported_usage != PSA_KEY_USAGE_VERIFY_MESSAGE) return PSA_ERROR_INVALID_SIGNATURE;
    uint8_t digest[EVP_MAX_MD_SIZE];
    unsigned length = 0;
    const bool valid = HMAC(EVP_sha256(), imported_key, sizeof imported_key,
                           input, input_length, digest, &length) != NULL &&
        length == tag_length && CRYPTO_memcmp(digest, tag, tag_length) == 0;
    OPENSSL_cleanse(digest, sizeof digest);
    return valid ? PSA_SUCCESS : PSA_ERROR_INVALID_SIGNATURE;
}

psa_status_t psa_mac_compute(psa_key_id_t id, uint32_t algorithm,
                           const uint8_t *input, size_t input_length,
                           uint8_t *tag, size_t tag_capacity, size_t *tag_length)
{
    if (!key_present || id != 1U || algorithm != PSA_ALG_HMAC(PSA_ALG_SHA_256) ||
        imported_usage != PSA_KEY_USAGE_SIGN_MESSAGE || tag_capacity < 32U)
        return PSA_ERROR_INSUFFICIENT_MEMORY;
    unsigned length = 0;
    if (!HMAC(EVP_sha256(), imported_key, sizeof imported_key,
              input, input_length, tag, &length)) return PSA_ERROR_INSUFFICIENT_MEMORY;
    *tag_length = length;
    return PSA_SUCCESS;
}

psa_status_t psa_destroy_key(psa_key_id_t id)
{
    if (!key_present || id != 1U) return PSA_ERROR_INSUFFICIENT_MEMORY;
    OPENSSL_cleanse(imported_key, sizeof imported_key);
    key_present = false;
    imported_usage = 0;
    return PSA_SUCCESS;
}

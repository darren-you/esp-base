// SPDX-License-Identifier: Apache-2.0
/* Host transport probe: production wire parser and PSA wrapper, with OpenSSL
 * providing the test PSA MAC implementation. This is not the IDF crypto port,
 * MQTT owner, Container queue, guest execution, or product health proof. */
#include "esp_base_mqtt_event.h"
#include "psa/crypto.h"

#include <assert.h>
#include <openssl/crypto.h>
#include <openssl/hmac.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint8_t imported_key[32];
static bool key_present;

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
        a->usage != PSA_KEY_USAGE_VERIFY_MESSAGE ||
        a->algorithm != PSA_ALG_HMAC(PSA_ALG_SHA_256))
        return PSA_ERROR_INSUFFICIENT_MEMORY;
    memcpy(imported_key, data, length);
    key_present = true;
    *id = 1;
    return PSA_SUCCESS;
}

psa_status_t psa_mac_verify(psa_key_id_t id, uint32_t algorithm,
                          const uint8_t *input, size_t input_length,
                          const uint8_t *tag, size_t tag_length)
{
    if (!key_present || id != 1U || algorithm != PSA_ALG_HMAC(PSA_ALG_SHA_256) ||
        tag_length != 32U) return PSA_ERROR_INVALID_SIGNATURE;
    uint8_t digest[EVP_MAX_MD_SIZE];
    unsigned length = 0;
    const bool valid = HMAC(EVP_sha256(), imported_key, sizeof imported_key,
                           input, input_length, digest, &length) != NULL &&
        length == tag_length && CRYPTO_memcmp(digest, tag, tag_length) == 0;
    OPENSSL_cleanse(digest, sizeof digest);
    return valid ? PSA_SUCCESS : PSA_ERROR_INVALID_SIGNATURE;
}

psa_status_t psa_destroy_key(psa_key_id_t id)
{
    if (!key_present || id != 1U) return PSA_ERROR_INSUFFICIENT_MEMORY;
    OPENSSL_cleanse(imported_key, sizeof imported_key);
    key_present = false;
    return PSA_SUCCESS;
}

int main(int argc, char **argv)
{
    if (argc != 7 || strlen(argv[1]) != 36U || strlen(argv[2]) != 36U ||
        (strcmp(argv[4], "0") != 0 && strcmp(argv[4], "1") != 0) ||
        (strcmp(argv[5], "0") != 0 && strcmp(argv[5], "1") != 0)) return 2;
    FILE *file = fopen(argv[6], "rb");
    if (file == NULL) return 2;
    uint8_t frame[4097];
    const size_t size_bytes = fread(frame, 1, sizeof frame, file);
    const bool io_ok = !ferror(file);
    fclose(file);
    if (!io_ok) return 2;
    uint8_t key[32];
    for (unsigned i = 0; i < sizeof key; ++i) key[i] = (uint8_t)i;
    ebase_mqtt_event_view_t view;
    const bool accepted = ebase_mqtt_verified_event(
        key, argv[1], argv[2], argv[3], (uint8_t)(argv[4][0] - '0'),
        argv[5][0] == '1', frame, size_bytes, &view);
    assert(!key_present);  // Every imported MAC key must be destroyed.
    if (!accepted) {
        puts("{\"accepted\":false}");
        return 0;
    }
    printf("{\"accepted\":true,\"event_sequence\":%llu,\"event_size_bytes\":%zu,"
           "\"package_sha256\":\"", (unsigned long long)view.event_sequence,
           view.event_size_bytes);
    for (unsigned i = 0; i < sizeof view.package_sha256; ++i)
        printf("%02x", view.package_sha256[i]);
    printf("\",\"event_sha256\":\"");
    uint8_t digest[32];
    unsigned digest_size = 0;
    assert(EVP_Digest(view.event, view.event_size_bytes, digest, &digest_size,
                      EVP_sha256(), NULL) == 1 && digest_size == sizeof digest);
    for (unsigned i = 0; i < sizeof digest; ++i) printf("%02x", digest[i]);
    puts("\"}");
    return 0;
}

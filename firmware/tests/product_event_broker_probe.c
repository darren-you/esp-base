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

#include "fakes/network_auth_openssl.inc"

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

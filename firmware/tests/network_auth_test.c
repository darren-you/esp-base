// SPDX-License-Identifier: Apache-2.0
#include "esp_base_network_auth.h"
#include "esp_base_mqtt_command.h"
#include "esp_base_mqtt_event.h"
#include "psa/crypto.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

/* The frozen tag is HMAC-SHA256 for key 00..1f and these exact UTF-8 bytes.
 * The SDK supplies PSA; this fake checks the wrapper's inputs and cleanup. */
static const uint8_t key[32] = {
    0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
    0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f,
    0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17,
    0x18, 0x19, 0x1a, 0x1b, 0x1c, 0x1d, 0x1e, 0x1f
};
static const uint8_t tag[32] = {
    0x57, 0xd8, 0xe9, 0x8b, 0x33, 0xe6, 0x9b, 0x07,
    0x5c, 0xd1, 0x38, 0x71, 0x28, 0x13, 0x41, 0x1c,
    0x03, 0x6f, 0x61, 0x5a, 0x24, 0x0e, 0x04, 0xa5,
    0x4e, 0x8c, 0x54, 0xf2, 0xfa, 0x3f, 0x38, 0xca
};
static const uint8_t request[] =
    "{\"protocol_version\":1,\"request_id\":\"11111111-1111-4111-8111-111111111111\",\"command\":\"status\"}";
static const uint8_t event_tag[32] = {
    0x82, 0x5d, 0x3c, 0xe7, 0xac, 0x4f, 0x69, 0x13,
    0x5b, 0xd4, 0x09, 0xf4, 0x75, 0x16, 0x84, 0x00,
    0x47, 0xe0, 0xf6, 0x47, 0x0c, 0xd4, 0x92, 0x79,
    0xec, 0x1a, 0x2e, 0x65, 0x47, 0x5a, 0xae, 0xdd
};
static uint8_t event_body[141];
static const char boot_id[] = "33333333-3333-4333-8333-333333333333";

static unsigned imports, verifies, destroys, resets;
static bool fail_import, fail_destroy;
static const char device_id[] = "22222222-2222-4222-8222-222222222222";

void psa_set_key_type(psa_key_attributes_t *a, uint32_t v) { a->type = v; }
void psa_set_key_bits(psa_key_attributes_t *a, uint32_t v) { a->bits = v; }
void psa_set_key_usage_flags(psa_key_attributes_t *a, uint32_t v) { a->usage = v; }
void psa_set_key_algorithm(psa_key_attributes_t *a, uint32_t v) { a->algorithm = v; }
void psa_reset_key_attributes(psa_key_attributes_t *a) { memset(a, 0, sizeof *a); ++resets; }
psa_status_t psa_import_key(const psa_key_attributes_t *a, const uint8_t *data,
                            size_t length, psa_key_id_t *key_id)
{
    ++imports;
    assert(a->type == PSA_KEY_TYPE_HMAC && a->bits == 256 &&
           a->usage == PSA_KEY_USAGE_VERIFY_MESSAGE &&
           a->algorithm == PSA_ALG_HMAC(PSA_ALG_SHA_256));
    assert(length == sizeof key && !memcmp(data, key, sizeof key));
    if (fail_import) return PSA_ERROR_INSUFFICIENT_MEMORY;
    *key_id = 42;
    return PSA_SUCCESS;
}
psa_status_t psa_mac_verify(psa_key_id_t key_id, uint32_t algorithm,
                            const uint8_t *input, size_t input_length,
                            const uint8_t *mac, size_t mac_length)
{
    ++verifies;
    assert(key_id == 42 && algorithm == PSA_ALG_HMAC(PSA_ALG_SHA_256));
    if (input_length == sizeof request - 1 &&
        !memcmp(input, request, input_length) &&
        mac_length == sizeof tag && !memcmp(mac, tag, sizeof tag)) return PSA_SUCCESS;
    if (input_length == sizeof event_body &&
        !memcmp(input, event_body, input_length) &&
        mac_length == sizeof event_tag &&
        !memcmp(mac, event_tag, sizeof event_tag)) return PSA_SUCCESS;
    return PSA_ERROR_INVALID_SIGNATURE;
}
psa_status_t psa_destroy_key(psa_key_id_t key_id)
{
    ++destroys;
    assert(key_id == 42);
    return fail_destroy ? PSA_ERROR_INSUFFICIENT_MEMORY : PSA_SUCCESS;
}

int main(void)
{
    assert(ebase_management_authenticate(key, tag, request, sizeof request - 1));
    assert(imports == 1 && verifies == 1 && destroys == 1 && resets == 1);

    uint8_t altered[sizeof request];
    memcpy(altered, request, sizeof request);
    altered[1] = 'X';
    assert(!ebase_management_authenticate(key, tag, altered, sizeof request - 1));
    uint8_t bad_tag[sizeof tag];
    memcpy(bad_tag, tag, sizeof tag);
    bad_tag[31] ^= 1;
    assert(!ebase_management_authenticate(key, bad_tag, request, sizeof request - 1));
    assert(imports == 3 && verifies == 3 && destroys == 3 && resets == 3);

    fail_import = true;
    assert(!ebase_management_authenticate(key, tag, request, sizeof request - 1));
    assert(imports == 4 && verifies == 3 && destroys == 3 && resets == 4);
    fail_import = false;
    fail_destroy = true;
    assert(!ebase_management_authenticate(key, tag, request, sizeof request - 1));
    assert(imports == 5 && verifies == 4 && destroys == 4 && resets == 5);
    fail_destroy = false;

    assert(!ebase_management_authenticate(NULL, tag, request, sizeof request - 1));
    assert(!ebase_management_authenticate(key, NULL, request, sizeof request - 1));
    assert(!ebase_management_authenticate(key, tag, NULL, sizeof request - 1));
    assert(!ebase_management_authenticate(key, tag, request, 0));
    assert(!ebase_management_authenticate(key, tag, request, 4097));
    assert(imports == 5 && verifies == 4 && destroys == 4 && resets == 5);

    char topic[EBASE_MQTT_TOPIC_BYTES];
    assert(ebase_mqtt_topic(topic, device_id, EBASE_MQTT_COMMAND));
    assert(!strcmp(topic, "esp-base/22222222-2222-4222-8222-222222222222/command"));
    assert(ebase_mqtt_topic(topic, device_id, EBASE_MQTT_RESULT));
    assert(!strcmp(topic, "esp-base/22222222-2222-4222-8222-222222222222/result"));
    assert(ebase_mqtt_topic(topic, device_id, EBASE_MQTT_REPORTED));
    assert(!strcmp(topic, "esp-base/22222222-2222-4222-8222-222222222222/reported"));
    assert(ebase_mqtt_topic(topic, device_id, EBASE_MQTT_STATUS));
    assert(!strcmp(topic, "esp-base/22222222-2222-4222-8222-222222222222/status"));
    assert(ebase_mqtt_topic(topic, device_id, EBASE_MQTT_EVENT));
    assert(!strcmp(topic, "esp-base/22222222-2222-4222-8222-222222222222/event"));
    assert(!ebase_mqtt_topic(topic, "invalid-device", EBASE_MQTT_COMMAND));
    assert(!ebase_mqtt_topic(topic, device_id, (ebase_mqtt_channel_t)5));

    assert(ebase_mqtt_topic(topic, device_id, EBASE_MQTT_COMMAND));
    static const char hex[] = "57d8e98b33e69b075cd138712813411c036f615a240e04a54e8c54f2fa3f38ca";
    uint8_t frame[65 + sizeof request];
    memcpy(frame, hex, 64);
    frame[64] = '\n';
    memcpy(frame + 65, request, sizeof request - 1);
    const size_t frame_length = 65 + sizeof request - 1;
    ebase_mqtt_request_view_t view = {0};
    assert(ebase_mqtt_verified_request(key, device_id, topic, 1, false,
                                       frame, frame_length, &view));
    assert(view.request == frame + 65 && view.request_length == sizeof request - 1);
    assert(imports == 6 && verifies == 5 && destroys == 5);

    assert(!ebase_mqtt_verified_request(key, device_id, topic, 0, false,
                                        frame, frame_length, &view));
    assert(!ebase_mqtt_verified_request(key, device_id, topic, 1, true,
                                        frame, frame_length, &view));
    assert(!ebase_mqtt_verified_request(key, device_id,
        "esp-base/22222222-2222-4222-8222-222222222222/result", 1, false,
        frame, frame_length, &view));
    assert(!ebase_mqtt_verified_request(key, "33333333-3333-4333-8333-333333333333",
                                        topic, 1, false, frame, frame_length, &view));
    assert(!ebase_mqtt_verified_request(key, device_id, topic, 1, false,
                                        frame, 65, &view));
    assert(!ebase_mqtt_verified_request(key, device_id, topic, 1, false,
                                        frame, 4097, &view));
    assert(view.request == NULL && view.request_length == 0);
    assert(imports == 6 && verifies == 5 && destroys == 5);

    frame[0] = 'F'; /* Only canonical lowercase hex is allowed. */
    assert(!ebase_mqtt_verified_request(key, device_id, topic, 1, false,
                                        frame, frame_length, &view));
    frame[0] = '5';
    frame[64] = '\r';
    assert(!ebase_mqtt_verified_request(key, device_id, topic, 1, false,
                                        frame, frame_length, &view));
    frame[64] = '\n';
    frame[65] ^= 1;
    assert(!ebase_mqtt_verified_request(key, device_id, topic, 1, false,
                                        frame, frame_length, &view));
    assert(view.request == NULL && view.request_length == 0);
    assert(imports == 7 && verifies == 6 && destroys == 6);

    static const char event_domain[] = "esp-base-product-event-v1\n";
    size_t offset = 0;
    memcpy(event_body + offset, event_domain, sizeof event_domain - 1);
    offset += sizeof event_domain - 1;
    memcpy(event_body + offset, device_id, 36);
    offset += 36;
    memcpy(event_body + offset, boot_id, 36);
    offset += 36;
    memset(event_body + offset, 0x11, 32);
    offset += 32;
    memset(event_body + offset, 0, 8);
    event_body[offset + 7] = 1;
    offset += 8;
    memcpy(event_body + offset, "\x01\x02\x03", 3);
    offset += 3;
    assert(offset == sizeof event_body);
    static const char event_hex[] =
        "825d3ce7ac4f69135bd409f47516840047e0f6470cd49279ec1a2e65475aaedd";
    uint8_t event_frame[65 + sizeof event_body];
    memcpy(event_frame, event_hex, 64);
    event_frame[64] = '\n';
    memcpy(event_frame + 65, event_body, sizeof event_body);
    assert(ebase_mqtt_topic(topic, device_id, EBASE_MQTT_EVENT));
    ebase_mqtt_event_view_t event_view = {0};
    assert(ebase_mqtt_verified_event(key, device_id, boot_id, topic, 1, false,
                                      event_frame, sizeof event_frame, &event_view));
    assert(event_view.event_sequence == 1U && event_view.event_size_bytes == 3U &&
           event_view.event == event_frame + sizeof event_frame - 3U &&
           !memcmp(event_view.event, "\x01\x02\x03", 3));
    for (unsigned index = 0; index < 32U; ++index)
        assert(event_view.package_sha256[index] == 0x11);
    assert(!ebase_mqtt_verified_event(key, device_id, boot_id, topic, 0, false,
                                       event_frame, sizeof event_frame, &event_view));
    assert(!ebase_mqtt_verified_event(key, device_id, boot_id, topic, 1, true,
                                       event_frame, sizeof event_frame, &event_view));
    assert(!ebase_mqtt_verified_event(key, device_id, boot_id,
        "esp-base/22222222-2222-4222-8222-222222222222/command", 1, false,
        event_frame, sizeof event_frame, &event_view));
    assert(!ebase_mqtt_verified_event(key, device_id, device_id, topic, 1, false,
                                       event_frame, sizeof event_frame, &event_view));
    assert(!ebase_mqtt_verified_event(key, device_id, boot_id, topic, 1, false,
                                       event_frame, sizeof event_frame - 3U, &event_view));
    event_frame[0] = 'A';
    assert(!ebase_mqtt_verified_event(key, device_id, boot_id, topic, 1, false,
                                       event_frame, sizeof event_frame, &event_view));
    event_frame[0] = event_hex[0];
    event_frame[65] ^= 1U;
    assert(!ebase_mqtt_verified_event(key, device_id, boot_id, topic, 1, false,
                                       event_frame, sizeof event_frame, &event_view));
    assert(event_view.event == NULL && event_view.event_size_bytes == 0U);
    puts("  network_auth     passed (exact request bytes, PSA failure cleanup)");
    puts("  mqtt_command     passed (Topic, QoS1, retained and frame rejection)");
    puts("  mqtt_event       passed (signed boot/package/sequence and payload boundary)");
}

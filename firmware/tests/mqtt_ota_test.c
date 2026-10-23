// SPDX-License-Identifier: Apache-2.0
#include "esp_base_mqtt_ota.h"
#include "esp_base_mqtt_ota_stream.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <openssl/crypto.h>
#include <openssl/hmac.h>
#include "psa/crypto.h"
#include "network_auth_openssl.inc"

static const char device[] = "11111111-1111-4111-8111-111111111111";
static const char boot[] = "22222222-2222-4222-8222-222222222222";
static const char operation[] = "33333333-3333-4333-8333-333333333333";
static const char data_topic[] = "esp-base/11111111-1111-4111-8111-111111111111/ota/data";
static const char request_topic[] = "esp-base/11111111-1111-4111-8111-111111111111/ota/request";
static uint8_t key[32];

static void resign(uint8_t *frame, size_t length)
{
    assert(ebase_management_sign(key, frame + 32, length - 32, frame));
}

static bool decode(const uint8_t *frame, size_t length, bool from_device)
{
    ebase_mqtt_ota_frame_t view = {.offset = 99};
    const bool valid = ebase_mqtt_ota_decode(key, device, boot, operation, from_device,
        from_device ? request_topic : data_topic, 1, false, frame, length, &view);
    if (!valid) assert(view.offset == 0 && view.data == NULL && view.length == 0);
    return valid;
}

static void codec_vectors(const char *path)
{
    FILE *file = fopen(path, "r"); assert(file);
    char name[16], hex[8193];
    uint8_t frame[EBASE_MQTT_OTA_FRAME_BYTES + 1], rebuilt[EBASE_MQTT_OTA_FRAME_BYTES];
    unsigned vectors = 0;
    while (fscanf(file, "%15s %8192s", name, hex) == 2) {
        const size_t length = strlen(hex) / 2;
        assert(length >= EBASE_MQTT_OTA_HEADER_BYTES && length <= EBASE_MQTT_OTA_FRAME_BYTES);
        for (size_t i = 0; i < length; ++i) {
            unsigned value; assert(sscanf(hex + i * 2, "%2x", &value) == 1);
            frame[i] = (uint8_t)value;
        }
        const bool from_device = !strcmp(name, "request");
        ebase_mqtt_ota_frame_t view;
        assert(ebase_mqtt_ota_decode(key, device, boot, operation, from_device,
            from_device ? request_topic : data_topic, 1, false, frame, length, &view));
        assert(view.offset == 1024 && view.written_offset == 1024 && view.expires_at_uptime_ms == 31000);
        size_t written;
        assert(ebase_mqtt_ota_encode(key, device, boot, operation, &view,
            rebuilt, sizeof rebuilt, &written));
        assert(written == length && !memcmp(frame, rebuilt, length));
        for (size_t i = 0; i < length; ++i) {
            frame[i] ^= 1; assert(!decode(frame, length, from_device)); frame[i] ^= 1;
        }
        for (size_t i = 0; i < length; ++i) assert(!decode(frame, i, from_device));
        frame[length] = 0; assert(!decode(frame, length + 1, from_device));
        assert(!decode(frame, length, !from_device));
        const char *topic = from_device ? request_topic : data_topic;
        assert(!ebase_mqtt_ota_decode(key, device, boot, operation, from_device, topic, 0, false, frame, length, &view));
        assert(!ebase_mqtt_ota_decode(key, device, boot, operation, from_device, topic, 1, true, frame, length, &view));
        assert(!ebase_mqtt_ota_decode(key, boot, boot, operation, from_device, topic, 1, false, frame, length, &view));
        assert(!ebase_mqtt_ota_decode(key, device, operation, operation, from_device, topic, 1, false, frame, length, &view));
        assert(!ebase_mqtt_ota_decode(key, device, boot, boot, from_device, topic, 1, false, frame, length, &view));
        /* Valid MAC over wrong domain/identity/length/kind must still reject. */
        const size_t bad_positions[] = {32, 32 + sizeof(EBASE_MQTT_OTA_DOMAIN) - 1,
            32 + sizeof(EBASE_MQTT_OTA_DOMAIN), EBASE_MQTT_OTA_HEADER_BYTES - 1};
        for (size_t i = 0; i < sizeof bad_positions / sizeof bad_positions[0]; ++i) {
            memcpy(rebuilt, frame, length); rebuilt[bad_positions[i]] = 0xff;
            resign(rebuilt, length); assert(!decode(rebuilt, length, from_device));
        }
        ++vectors;
    }
    assert(fclose(file) == 0 && vectors == 5);
}

static ebase_mqtt_ota_frame_t response(ebase_mqtt_ota_frame_t request,
    ebase_mqtt_ota_kind_t kind, const uint8_t *data, size_t length)
{
    request.kind = kind; request.data = data; request.length = length;
    request.tag[0] = (uint8_t)(request.offset / 1024 + 1);
    return request;
}

static void stream_tests(void)
{
    ebase_mqtt_ota_stream_t s = {0};
    uint8_t bytes[1024], output[1024];
    memset(bytes, 0x5a, sizeof bytes);
    ebase_mqtt_ota_frame_t request, retry;
    assert(!ebase_mqtt_ota_stream_begin(&s, 1, UINT64_MAX - 10, 100, 30));
    assert(!ebase_mqtt_ota_stream_begin(&s, 1, 100, 0, 30));
    assert(ebase_mqtt_ota_stream_begin(&s, 1027, 100, 1000, 200));
    assert(!ebase_mqtt_ota_stream_begin(&s, 1027, 100, 1000, 200));
    assert(ebase_mqtt_ota_stream_read(&s, 101, output, sizeof output) == -2);
    assert(ebase_mqtt_ota_stream_request(&s, 102, &request));
    assert(request.offset == 0 && request.written_offset == 0 && request.expires_at_uptime_ms == 300);
    assert(ebase_mqtt_ota_stream_request(&s, 103, &retry));
    assert(retry.expires_at_uptime_ms == request.expires_at_uptime_ms);
    ebase_mqtt_ota_frame_t block = response(request, EBASE_MQTT_OTA_DATA, bytes, sizeof bytes);
    block.offset = 1024;
    assert(ebase_mqtt_ota_stream_offer(&s, 104, &block) == EBASE_MQTT_OTA_IGNORED);
    block.offset = 0; block.length--;
    assert(ebase_mqtt_ota_stream_offer(&s, 105, &block) == EBASE_MQTT_OTA_IGNORED);
    block.length++;
    assert(ebase_mqtt_ota_stream_offer(&s, 106, &block) == EBASE_MQTT_OTA_ACCEPTED);
    assert(s.previous.data == NULL);
    memset(bytes, 0, sizeof bytes); /* Message released: the mailbox owns its copy. */
    assert(ebase_mqtt_ota_stream_offer(&s, 107, &block) == EBASE_MQTT_OTA_DUPLICATE);
    assert(!ebase_mqtt_ota_stream_request(&s, 108, &retry));
    assert(ebase_mqtt_ota_stream_read(&s, 109, output, 64) == 64);
    assert(output[0] == 0x5a && output[63] == 0x5a && s.written_offset == 0);
    assert(ebase_mqtt_ota_stream_read(&s, 110, output, sizeof output) == 960);
    assert(!ebase_mqtt_ota_stream_request(&s, 111, &retry));
    assert(ebase_mqtt_ota_stream_written(&s, 112, 1024));
    assert(ebase_mqtt_ota_stream_request(&s, 113, &request));
    assert(request.offset == 1024 && request.written_offset == 1024);
    assert(ebase_mqtt_ota_stream_offer(&s, 114, &block) == EBASE_MQTT_OTA_DUPLICATE);
    block = response(request, EBASE_MQTT_OTA_DATA, bytes, 3);
    assert(ebase_mqtt_ota_stream_offer(&s, 115, &block) == EBASE_MQTT_OTA_ACCEPTED);
    assert(ebase_mqtt_ota_stream_read(&s, 116, output, sizeof output) == 3);
    assert(ebase_mqtt_ota_stream_request(&s, 117, &request));
    assert(request.offset == 1027 && request.written_offset == 1024);
    assert(ebase_mqtt_ota_stream_read(&s, 118, output, 1) == -2); /* Length is not EOF. */
    block = response(request, EBASE_MQTT_OTA_END, NULL, 0);
    assert(ebase_mqtt_ota_stream_offer(&s, 119, &block) == EBASE_MQTT_OTA_ACCEPTED);
    assert(ebase_mqtt_ota_stream_read(&s, 120, output, 1) == 0);
    assert(ebase_mqtt_ota_stream_offer(&s, 121, &block) == EBASE_MQTT_OTA_DUPLICATE);
    assert(ebase_mqtt_ota_stream_written(&s, 122, 1027));
    assert(!ebase_mqtt_ota_stream_request(&s, 123, &request));
    ebase_mqtt_ota_stream_close(&s);
    assert(!s.active && !s.previous_valid && s.block[0] == 0);

    /* Conflicting duplicate, even after consuming bytes, latches a failure. */
    assert(ebase_mqtt_ota_stream_begin(&s, 1024, 100, 1000, 200));
    assert(ebase_mqtt_ota_stream_request(&s, 100, &request));
    block = response(request, EBASE_MQTT_OTA_DATA, bytes, sizeof bytes);
    assert(ebase_mqtt_ota_stream_offer(&s, 101, &block) == EBASE_MQTT_OTA_ACCEPTED);
    block.tag[1] ^= 1;
    assert(ebase_mqtt_ota_stream_offer(&s, 102, &block) == EBASE_MQTT_OTA_REJECTED);
    assert(s.error == EBASE_MQTT_OTA_STREAM_CONFLICT);
    assert(ebase_mqtt_ota_stream_read(&s, 103, output, 1) == -1);
    ebase_mqtt_ota_stream_close(&s);

    /* Cancel both before data and while a copied block is in the worker. */
    for (unsigned after_data = 0; after_data < 2; ++after_data) {
        assert(ebase_mqtt_ota_stream_begin(&s, 1024, 100, 1000, 200));
        assert(ebase_mqtt_ota_stream_request(&s, 100, &request));
        block = response(request, EBASE_MQTT_OTA_DATA, bytes, sizeof bytes);
        if (after_data) assert(ebase_mqtt_ota_stream_offer(&s, 101, &block) == EBASE_MQTT_OTA_ACCEPTED);
        block = response(request, EBASE_MQTT_OTA_CANCEL, NULL, 0);
        assert(ebase_mqtt_ota_stream_offer(&s, 102, &block) == EBASE_MQTT_OTA_ACCEPTED);
        assert(s.error == EBASE_MQTT_OTA_STREAM_CANCELLED);
        ebase_mqtt_ota_stream_close(&s);
    }
    /* Retries, duplicate writes, backwards time and late packets cannot
     * extend the finite idle/total budget or fabricate a written byte. */
    assert(ebase_mqtt_ota_stream_begin(&s, 1024, 100, 1000, 200));
    assert(ebase_mqtt_ota_stream_request(&s, 100, &request));
    assert(ebase_mqtt_ota_stream_written(&s, 299, 0));
    assert(!ebase_mqtt_ota_stream_request(&s, 300, &retry));
    assert(s.error == EBASE_MQTT_OTA_STREAM_TIMEOUT);
    ebase_mqtt_ota_stream_close(&s);
    assert(ebase_mqtt_ota_stream_begin(&s, 1024, 100, 1000, 200));
    assert(!ebase_mqtt_ota_stream_written(&s, 101, 1));
    assert(s.error == EBASE_MQTT_OTA_STREAM_BAD_WRITE);
    ebase_mqtt_ota_stream_close(&s);
    assert(ebase_mqtt_ota_stream_begin(&s, 1024, 100, 1000, 200));
    assert(!ebase_mqtt_ota_stream_request(&s, 99, &request));
    assert(s.error == EBASE_MQTT_OTA_STREAM_TIMEOUT);
    ebase_mqtt_ota_stream_close(&s);
}

static void stream_many_blocks(void)
{
    ebase_mqtt_ota_stream_t s = {0};
    uint8_t data[1024] = {0}, copied[64];
    ebase_mqtt_ota_frame_t request, block;
    assert(ebase_mqtt_ota_stream_begin(&s, 40 * sizeof data, 0, 4100, 200));
    for (uint32_t i = 0; i < 40; ++i) {
        const uint64_t now = i * 100;
        assert(ebase_mqtt_ota_stream_request(&s, now, &request));
        assert(request.offset == i * sizeof data && request.written_offset == request.offset);
        block = response(request, EBASE_MQTT_OTA_DATA, data, sizeof data);
        assert(ebase_mqtt_ota_stream_offer(&s, now + 1, &block) == EBASE_MQTT_OTA_ACCEPTED);
        for (unsigned part = 0; part < 16; ++part)
            assert(ebase_mqtt_ota_stream_read(&s, now + 2, copied, sizeof copied) == 64);
        assert(ebase_mqtt_ota_stream_written(&s, now + 3, (i + 1) * sizeof data));
    }
    assert(ebase_mqtt_ota_stream_request(&s, 4000, &request));
    /* Continuous progress can neither extend the original session deadline
     * nor grant a new block/EOF after its final idle deadline. */
    block = response(request, EBASE_MQTT_OTA_END, NULL, 0);
    assert(ebase_mqtt_ota_stream_offer(&s, 4100, &block) == EBASE_MQTT_OTA_REJECTED);
    assert(s.error == EBASE_MQTT_OTA_STREAM_TIMEOUT && s.written_offset == 40960);
    ebase_mqtt_ota_stream_close(&s);
}

static void selection_boundary(void)
{
    uint8_t byte = 7, copied;
    for (unsigned scenario = 0; scenario < 4; ++scenario) {
        ebase_mqtt_ota_stream_t s = {0};
        ebase_mqtt_ota_frame_t request;
        assert(ebase_mqtt_ota_stream_begin(&s, 1, 100, 1000, 200));
        assert(!ebase_mqtt_ota_stream_select(&s, 101));
        assert(ebase_mqtt_ota_stream_request(&s, 102, &request));
        ebase_mqtt_ota_frame_t frame = response(request, EBASE_MQTT_OTA_DATA, &byte, 1);
        assert(ebase_mqtt_ota_stream_offer(&s, 103, &frame) == EBASE_MQTT_OTA_ACCEPTED);
        assert(ebase_mqtt_ota_stream_read(&s, 104, &copied, 1) == 1 && copied == byte);
        assert(!ebase_mqtt_ota_stream_select(&s, 105));
        assert(ebase_mqtt_ota_stream_request(&s, 106, &request));
        frame = response(request, EBASE_MQTT_OTA_END, NULL, 0);
        assert(ebase_mqtt_ota_stream_offer(&s, 107, &frame) == EBASE_MQTT_OTA_ACCEPTED);
        assert(!ebase_mqtt_ota_stream_select(&s, 108)); /* Tail not written yet. */
        assert(ebase_mqtt_ota_stream_written(&s, 109, 1));
        frame.kind = EBASE_MQTT_OTA_CANCEL;
        if (scenario == 0) {
            assert(ebase_mqtt_ota_stream_select(&s, 110));
            assert(ebase_mqtt_ota_stream_offer(&s, 111, &frame) == EBASE_MQTT_OTA_IGNORED);
            assert(s.selecting && s.error == EBASE_MQTT_OTA_STREAM_OK);
            assert(!ebase_mqtt_ota_stream_select(&s, 112));
        } else if (scenario == 1) {
            assert(ebase_mqtt_ota_stream_offer(&s, 110, &frame) == EBASE_MQTT_OTA_ACCEPTED);
            assert(!ebase_mqtt_ota_stream_select(&s, 111));
            assert(s.error == EBASE_MQTT_OTA_STREAM_CANCELLED);
        } else if (scenario == 2) {
            assert(!ebase_mqtt_ota_stream_select(&s, 309)); /* Expiry during verification. */
            assert(s.error == EBASE_MQTT_OTA_STREAM_TIMEOUT);
        } else {
            frame.kind = EBASE_MQTT_OTA_END; frame.tag[1] ^= 1;
            assert(ebase_mqtt_ota_stream_offer(&s, 110, &frame) == EBASE_MQTT_OTA_REJECTED);
            assert(!ebase_mqtt_ota_stream_select(&s, 111));
            assert(s.error == EBASE_MQTT_OTA_STREAM_CONFLICT);
        }
        ebase_mqtt_ota_stream_close(&s);
    }
}

int main(int argc, char **argv)
{
    assert(argc == 2);
    for (size_t i = 0; i < sizeof key; ++i) key[i] = (uint8_t)i;
    codec_vectors(argv[1]);
    stream_tests();
    stream_many_blocks();
    selection_boundary();
    puts("MQTT OTA: binary authentication vectors and bounded stream passed (no hardware)");
    return 0;
}

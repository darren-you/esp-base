// SPDX-License-Identifier: Apache-2.0
#include "esp_base_frp_management_listener.h"
#include "esp_base_network_auth.h"

#include <arpa/inet.h>
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>
#include <pthread.h>
#include <stdatomic.h>

static const char body[] =
    "{\"protocol_version\":1,\"device_id\":\"22222222-2222-4222-8222-222222222222\","
    "\"request_id\":\"11111111-1111-4111-8111-111111111111\","
    "\"command\":\"status\"}";
static const char result[] = "{\"state\":\"succeeded\"}";
static unsigned authenticated, handled, signed_responses;
static bool fail_sign;
static int handler_status = 200;
static bool empty_result, oversized_result;
static const uint8_t *expected_request = (const uint8_t *)body;
static size_t expected_request_length = sizeof body - 1;
static const char upload_operation[] = "11111111-1111-4111-8111-111111111111";
static const char upload_device[] = "22222222-2222-4222-8222-222222222222";
static const char upload_boot[] = "33333333-3333-4333-8333-333333333333";
static uint32_t expected_upload_size = 128;


bool ebase_management_sign(const uint8_t key[EBASE_MANAGEMENT_KEY_BYTES],
                           const uint8_t *message, size_t length,
                           uint8_t tag[EBASE_MANAGEMENT_TAG_BYTES])
{
    ++signed_responses;
    assert(length == sizeof result - 1 && !memcmp(message, result, length));
    memset(tag, key[0], EBASE_MANAGEMENT_TAG_BYTES);
    return !fail_sign;
}

bool ebase_management_authenticate(const uint8_t key[EBASE_MANAGEMENT_KEY_BYTES],
                                   const uint8_t tag[EBASE_MANAGEMENT_TAG_BYTES],
                                   const uint8_t *request, size_t length)
{
    ++authenticated;
    if (length >= 23 && !memcmp(request, "esp-base-ota-upload-v1\n", 23)) {
        char expected[256], sha[65];
        memset(sha, '4', 64); sha[64] = 0;
        const int bytes = snprintf(expected, sizeof expected,
            "esp-base-ota-upload-v1\n%s\n%s\n%s\n%u\n%s\n", upload_operation,
            upload_device, upload_boot, (unsigned)expected_upload_size, sha);
        if (bytes <= 0 || (size_t)bytes != length || memcmp(request, expected, length)) return false;
    } else if (length != expected_request_length || memcmp(request, expected_request, length)) return false;
    for (size_t i = 0; i < EBASE_MANAGEMENT_TAG_BYTES; ++i)
        if (tag[i] != key[0]) return false;
    return true;
}

static esp_base_frp_management_command_t expected_command = ESP_BASE_FRP_MANAGEMENT_STATUS;

static int status_handler(esp_base_frp_management_command_t command, const uint8_t *request, size_t length, char *response,
                          size_t capacity, size_t *written, void *context)
{
    (void)context;
    assert(command == expected_command);
    assert(length == expected_request_length && !memcmp(request, expected_request, length));
    ++handled;
    assert(capacity >= sizeof result);
    memcpy(response, result, sizeof result - 1);
    *written = empty_result ? 0 : oversized_result ? capacity : sizeof result - 1;
    return handler_status;
}

static uint16_t unused_port(void)
{
    const int fd = socket(AF_INET, SOCK_STREAM, 0);
    assert(fd >= 0);
    struct sockaddr_in address = {.sin_family = AF_INET,
        .sin_port = 0, .sin_addr.s_addr = htonl(0x7f000001u)};
    assert(bind(fd, (struct sockaddr *)&address, sizeof address) == 0);
    socklen_t length = sizeof address;
    assert(getsockname(fd, (struct sockaddr *)&address, &length) == 0);
    close(fd);
    return ntohs(address.sin_port);
}

static int connect_client(uint16_t port)
{
    const int fd = socket(AF_INET, SOCK_STREAM, 0);
    assert(fd >= 0);
    struct sockaddr_in address = {.sin_family = AF_INET,
        .sin_port = htons(port), .sin_addr.s_addr = htonl(0x7f000001u)};
    assert(connect(fd, (struct sockaddr *)&address, sizeof address) == 0);
    const int flags = fcntl(fd, F_GETFL);
    assert(flags >= 0 && fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0);
    return fd;
}

static size_t request(char *out, size_t capacity, char tag_digit, bool duplicate)
{
    char tag[65];
    memset(tag, tag_digit, 64);
    tag[64] = 0;
    const int length = snprintf(out, capacity,
        "POST /api/v1/commands/status HTTP/1.1\r\nHost: device\r\n"
        "Content-Type: application/json\r\nContent-Length: %zu\r\n"
        "%sX-ESP-Management-Tag: %s\r\n\r\n%s",
        sizeof body - 1, duplicate ? "Content-Length: 1\r\n" : "", tag, body);
    assert(length > 0 && (size_t)length < capacity);
    return (size_t)length;
}

static void write_all(int fd, const char *data, size_t length)
{
    size_t offset = 0;
    while (offset < length) {
        const ssize_t sent = send(fd, data + offset, length - offset, 0);
        assert(sent > 0);
        offset += (size_t)sent;
    }
}

static void assert_closed(int fd)
{
    char value;
    for (unsigned i = 0; i < 1000; ++i) {
        const ssize_t count = recv(fd, &value, 1, 0);
        if (count == 0 || (count < 0 && errno == ECONNRESET)) return;
        assert(count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK));
        nanosleep(&(struct timespec){.tv_nsec = 1000000}, NULL);
    }
    assert(!"server did not close partial request");
}

static void response(int fd, uint64_t start, int status, bool has_result)
{
    char text[1500] = {0};
    size_t length = 0;
    for (unsigned i = 0; i < 1000; ++i) {
        esp_base_frp_management_listener_poll(start + i, status_handler, NULL);
        const ssize_t count = recv(fd, text + length, sizeof text - length - 1, 0);
        if (count > 0) length += (size_t)count;
        else if (count == 0) break;
        else if (errno == ECONNRESET) break;
        else assert(errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR);
    }
    char prefix[32];
    snprintf(prefix, sizeof prefix, "HTTP/1.1 %d ", status);
    assert(strstr(text, prefix) == text);
    assert((strstr(text, "\"succeeded\"") != NULL) == has_result);
    const char *tag = strstr(text, "X-ESP-Management-Tag: ");
    assert((tag != NULL) == has_result);
    if (tag) {
        tag += strlen("X-ESP-Management-Tag: ");
        const char digit = start < 7600 ? 'a' : 'b';
        for (unsigned i = 0; i < 64; ++i) assert(tag[i] == digit);
        assert(tag[64] == '\r' && tag[65] == '\n');
        assert(!strstr(tag + 66, "X-ESP-Management-Tag: "));
        const char *response_body = strstr(text, "\r\n\r\n") + 4;
        assert(!strcmp(response_body, result));
    }
    if (status == 401) assert(strstr(text, "Content-Length: 0\r\n") != NULL);
    close(fd);
}


static size_t upload_request(char *output, size_t capacity, const char *operation,
    uint32_t size, char tag_digit, const char *extra_header)
{
    char tag[65];
    memset(tag, tag_digit, 64); tag[64] = 0;
    const int length = snprintf(output, capacity,
        "PUT /api/v1/ota-images/%s HTTP/1.1\r\nHost: device\r\n"
        "Content-Type: application/octet-stream\r\nContent-Length: %u\r\n"
        "%sX-ESP-Management-Tag: %s\r\n\r\n", operation, (unsigned)size,
        extra_header ? extra_header : "", tag);
    assert(length > 0 && (size_t)length < capacity);
    return (size_t)length;
}

static void poll_until_connected(uint64_t now_ms)
{
    for (unsigned i = 0; i < 1000 && !esp_base_frp_management_upload_connected(); ++i) {
        esp_base_frp_management_listener_poll(now_ms, status_handler, NULL);
        nanosleep(&(struct timespec){.tv_nsec = 1000000}, NULL);
    }
    assert(esp_base_frp_management_upload_connected());
}

static bool arm_upload(uint64_t now_ms)
{
    uint8_t sha[32];
    memset(sha, 0x44, sizeof sha);
    return esp_base_frp_management_upload_arm(upload_operation, upload_device, upload_boot,
        expected_upload_size, sha, now_ms);
}

static void test_extended_commands(uint16_t port)
{
    static const struct {
        const char *path;
        esp_base_frp_management_command_t command;
    } commands[] = {
        {"firmware-status", ESP_BASE_FRP_MANAGEMENT_FIRMWARE_STATUS},
        {"ota-start", ESP_BASE_FRP_MANAGEMENT_OTA_START},
        {"ota-result", ESP_BASE_FRP_MANAGEMENT_OTA_RESULT},
        {"business-status", ESP_BASE_FRP_MANAGEMENT_BUSINESS_STATUS},
        {"business-pause", ESP_BASE_FRP_MANAGEMENT_BUSINESS_PAUSE},
        {"business-resume", ESP_BASE_FRP_MANAGEMENT_BUSINESS_RESUME},
    };
    char full_body[1025];
    memset(full_body, ' ', sizeof full_body - 1);
    memcpy(full_body, body, sizeof body - 1);
    full_body[1024] = 0;
    char frame[1600], tag[65];
    memset(tag, 'b', 64); tag[64] = 0;
    expected_request = (const uint8_t *)full_body;
    expected_request_length = 1024;
    handler_status = 200;
    for (size_t i = 0; i < sizeof commands / sizeof commands[0]; ++i) {
        expected_command = commands[i].command;
        const int count = snprintf(frame, sizeof frame,
            "POST /api/v1/commands/%s HTTP/1.1\r\nHost: device\r\n"
            "Content-Type: application/json\r\nContent-Length: 1024\r\n"
            "X-ESP-Management-Tag: %s\r\n\r\n%s", commands[i].path, tag, full_body);
        assert(count > 0 && (size_t)count < sizeof frame);
        const int fd = connect_client(port);
        write_all(fd, frame, (size_t)count);
        response(fd, 10000 + i * 10, 200, true);
    }
    int fd = connect_client(port);
    const int count = snprintf(frame, sizeof frame,
        "POST /api/v1/commands/status HTTP/1.1\r\nHost: device\r\n"
        "Content-Type: application/json\r\nContent-Length: 1024\r\n"
        "X-ESP-Management-Tag: %s\r\n\r\n%s", tag, full_body);
    write_all(fd, frame, (size_t)count);
    response(fd, 10100, 400, false);
    expected_request = (const uint8_t *)body;
    expected_request_length = sizeof body - 1;
    expected_command = ESP_BASE_FRP_MANAGEMENT_STATUS;
}

static atomic_bool reader_started;
static int reader_result;
static void *blocking_upload_reader(void *unused)
{
    (void)unused;
    uint8_t byte;
    struct timespec started; assert(clock_gettime(CLOCK_MONOTONIC, &started) == 0);
    const int64_t deadline_us = (int64_t)started.tv_sec * 1000000 + started.tv_nsec / 1000 + 1000000;
    atomic_store(&reader_started, true);
    reader_result = -2;
    do {
        struct timespec now; assert(clock_gettime(CLOCK_MONOTONIC, &now) == 0);
        const int64_t left_us = deadline_us - ((int64_t)now.tv_sec * 1000000 + now.tv_nsec / 1000);
        if (left_us < 1000) break;
        const uint32_t timeout_ms = left_us / 1000 > 1000 ? 1000 : (uint32_t)(left_us / 1000);
        reader_result = esp_base_frp_management_upload_read(NULL, &byte, 1, timeout_ms);
    } while (reader_result == -2);
    return NULL;
}

static void test_uploads(ebase_frp_config_t *config)
{
    uint8_t bytes[128], received[128];
    memset(bytes, 0x5a, sizeof bytes);
    char frame[1400];
    size_t length;
    unsigned before;
    int fd;
    expected_upload_size = sizeof bytes;
    assert(!esp_base_frp_management_upload_arm("bad", upload_device, upload_boot, 128, bytes, 11000));
    assert(arm_upload(11000));
    assert(!arm_upload(11000));
    assert(!esp_base_frp_management_upload_connected());
    length = upload_request(frame, sizeof frame, upload_operation, 128, 'a', NULL);
    fd = connect_client(config->local_port);
    write_all(fd, frame, length);
    response(fd, 11001, 401, false);
    assert(!esp_base_frp_management_upload_connected());

    /* Authenticate before receiving any firmware; a second connected upload
     * is refused while commands keep using the same listener. */
    length = upload_request(frame, sizeof frame, upload_operation, 128, 'b', NULL);
    fd = connect_client(config->local_port);
    write_all(fd, frame, length);
    poll_until_connected(11002);
    const int duplicate = connect_client(config->local_port);
    write_all(duplicate, frame, length);
    response(duplicate, 11003, 409, false);
    assert(esp_base_frp_management_upload_read(NULL, received, 64, 5) == -2);
    write_all(fd, (const char *)bytes, sizeof bytes);
    assert(esp_base_frp_management_upload_read(NULL, received, 64, 1000) == 64);
    assert(esp_base_frp_management_upload_read(NULL, received + 64, 64, 1000) == 64);
    assert(!memcmp(received, bytes, sizeof bytes));
    assert(esp_base_frp_management_upload_read(NULL, received, 1, 1000) == 0);
    char query[1200];
    const size_t query_length = request(query, sizeof query, 'b', false);
    const int querying = connect_client(config->local_port);
    write_all(querying, query, query_length);
    response(querying, 11004, 200, true);
    assert(esp_base_frp_management_upload_finish(202, result, sizeof result - 1, 1000));
    response(fd, 11005, 202, true);
    assert(!esp_base_frp_management_upload_connected());
    const int replay = connect_client(config->local_port);
    write_all(replay, frame, length);
    response(replay, 11006, 409, false);

    /* Header-only expiry, malformed framing and metadata mismatches never
     * transfer ownership or consume firmware from an unauthenticated peer. */
    assert(arm_upload(12000));
    fd = connect_client(config->local_port);
    write_all(fd, frame, length);
    response(fd, 17000, 409, false);
    assert(!esp_base_frp_management_upload_connected());
    assert(!esp_base_frp_management_upload_finish(400, result, sizeof result - 1, 1000));
    const char *bad_headers[] = {"Content-Length: 128\r\n", "Transfer-Encoding: chunked\r\n",
        "Expect: 100-continue\r\n"};
    for (size_t i = 0; i < sizeof bad_headers / sizeof bad_headers[0]; ++i) {
        assert(arm_upload(18000));
        length = upload_request(frame, sizeof frame, upload_operation, 128, 'b', bad_headers[i]);
        fd = connect_client(config->local_port);
        write_all(fd, frame, length);
        response(fd, 18001, 400, false);
        assert(!esp_base_frp_management_upload_connected());
        assert(!esp_base_frp_management_upload_finish(400, result, sizeof result - 1, 1000));
    }
    for (int mismatch = 0; mismatch < 2; ++mismatch) {
        assert(arm_upload(19000));
        length = upload_request(frame, sizeof frame,
            mismatch ? upload_operation : upload_boot, mismatch ? 127 : 128, 'b', NULL);
        fd = connect_client(config->local_port);
        write_all(fd, frame, length);
        response(fd, 19001, 409, false);
        assert(!esp_base_frp_management_upload_connected());
        assert(!esp_base_frp_management_upload_finish(400, result, sizeof result - 1, 1000));
    }

    /* Pre-read body is bounded and preserved across handoff. Extra bytes in
     * the same request and queued after the length boundary both fail. */
    assert(arm_upload(20000));
    length = upload_request(frame, sizeof frame, upload_operation, 128, 'b', NULL);
    memcpy(frame + length, bytes, sizeof bytes);
    fd = connect_client(config->local_port);
    write_all(fd, frame, length + sizeof bytes);
    poll_until_connected(20001);
    assert(esp_base_frp_management_upload_read(NULL, received, 64, 1000) == 64);
    assert(esp_base_frp_management_upload_read(NULL, received + 64, 64, 1000) == 64);
    assert(esp_base_frp_management_upload_read(NULL, received, 1, 1000) == 0);
    assert(esp_base_frp_management_upload_finish(202, result, sizeof result - 1, 1000));
    response(fd, 20002, 202, true);
    assert(arm_upload(21000));
    frame[length + sizeof bytes] = 'X';
    fd = connect_client(config->local_port);
    write_all(fd, frame, length + sizeof bytes + 1);
    response(fd, 21001, 400, false);
    assert(!esp_base_frp_management_upload_connected());
    assert(!esp_base_frp_management_upload_finish(400, result, sizeof result - 1, 1000));
    assert(arm_upload(22000));
    fd = connect_client(config->local_port);
    write_all(fd, frame, length);
    poll_until_connected(22001);
    write_all(fd, (const char *)bytes, sizeof bytes);
    assert(esp_base_frp_management_upload_read(NULL, received, 64, 1000) == 64);
    assert(esp_base_frp_management_upload_read(NULL, received + 64, 64, 1000) == 64);
    write_all(fd, "X", 1);
    int extra_result = 0;
    for (unsigned i = 0; i < 1000 && extra_result == 0; ++i) {
        extra_result = esp_base_frp_management_upload_read(NULL, received, 1, 1000);
        if (extra_result == 0) nanosleep(&(struct timespec){.tv_nsec = 1000000}, NULL);
    }
    assert(extra_result == -1);
    assert(esp_base_frp_management_upload_finish(400, result, sizeof result - 1, 1000));
    response(fd, 22002, 400, true);

    assert(arm_upload(23000));
    fd = connect_client(config->local_port);
    write_all(fd, frame, length);
    poll_until_connected(23001);
    assert(shutdown(fd, SHUT_WR) == 0);
    assert(esp_base_frp_management_upload_read(NULL, received, 64, 1000) == -1);
    assert(esp_base_frp_management_upload_finish(400, result, sizeof result - 1, 1000));
    response(fd, 23002, 400, true);

    /* A configuration key change wakes a blocked worker; it never signs the
     * old operation with the new key or closes a newly reused descriptor. */
    assert(arm_upload(24000));
    fd = connect_client(config->local_port);
    write_all(fd, frame, length);
    poll_until_connected(24001);
    atomic_store(&reader_started, false);
    pthread_t reader;
    assert(pthread_create(&reader, NULL, blocking_upload_reader, NULL) == 0);
    while (!atomic_load(&reader_started)) nanosleep(&(struct timespec){.tv_nsec = 1000000}, NULL);
    nanosleep(&(struct timespec){.tv_nsec = 10000000}, NULL);
    before = signed_responses;
    config->management_key[0] = 0xcc;
    esp_base_frp_management_listener_configure(config);
    assert(pthread_join(reader, NULL) == 0 && reader_result == -1);
    assert(!esp_base_frp_management_upload_connected());
    assert(!esp_base_frp_management_upload_finish(409, result, sizeof result - 1, 1000));
    assert(signed_responses == before);
    assert_closed(fd);
    close(fd);
    assert(arm_upload(25000));
    esp_base_frp_management_upload_cancel();
    assert(!esp_base_frp_management_upload_finish(409, result, sizeof result - 1, 1000));
    config->management_key[0] = 0xbb;
    esp_base_frp_management_listener_configure(config);
    esp_base_frp_management_listener_poll(26000, status_handler, NULL);
    assert(arm_upload(26000));
    length = upload_request(frame, sizeof frame, upload_operation, 128, 'b', NULL);
    fd = connect_client(config->local_port);
    write_all(fd, frame, length);
    poll_until_connected(26001);
    write_all(fd, (const char *)bytes, sizeof bytes);
    assert(esp_base_frp_management_upload_read(NULL, received, 64, 1000) == 64);
    assert(esp_base_frp_management_upload_read(NULL, received + 64, 64, 1000) == 64);
    assert(esp_base_frp_management_upload_finish(202, result, sizeof result - 1, 1000));
    response(fd, 26002, 202, true);
}

int main(void)
{
    ebase_frp_config_t config = {.configured = true, .local_port = unused_port()};
    memset(config.management_key, 0xaa, sizeof config.management_key);
    esp_base_frp_management_listener_configure(&config);
    esp_base_frp_management_listener_poll(1000, status_handler, NULL);
    assert(esp_base_frp_management_listener_ready());

    char frame[1200];
    size_t length = request(frame, sizeof frame, 'a', false);
    int fd = connect_client(config.local_port);
    write_all(fd, frame, 23);
    esp_base_frp_management_listener_poll(1100, status_handler, NULL);
    char one;
    assert(recv(fd, &one, 1, 0) < 0 && (errno == EAGAIN || errno == EWOULDBLOCK));
    write_all(fd, frame + 23, length - 23);
    response(fd, 1101, 200, true);
    assert(authenticated == 1 && handled == 1);

    fd = connect_client(config.local_port);
    length = request(frame, sizeof frame, 'b', false);
    write_all(fd, frame, length);
    response(fd, 2200, 401, false);
    assert(authenticated == 2 && handled == 1);

    fd = connect_client(config.local_port);
    length = request(frame, sizeof frame, 'a', true);
    write_all(fd, frame, length);
    response(fd, 3300, 400, false);
    assert(authenticated == 2 && handled == 1);

    fd = connect_client(config.local_port);
    memset(frame, 'X', 600);
    write_all(fd, frame, 600);
    response(fd, 4400, 400, false);
    assert(authenticated == 2 && handled == 1);

    fd = connect_client(config.local_port);
    length = request(frame, sizeof frame, 'a', false);
    write_all(fd, frame, length - 4);
    for (unsigned i = 0; i < 20; ++i) {
        esp_base_frp_management_listener_poll(5500 + i, status_handler, NULL);
        nanosleep(&(struct timespec){.tv_nsec = 1000000}, NULL);
    }
    esp_base_frp_management_listener_poll(7520, status_handler, NULL);
    assert_closed(fd);
    close(fd);
    assert(authenticated == 2 && handled == 1);

    fd = connect_client(config.local_port);
    write_all(fd, frame, 16);
    for (unsigned i = 0; i < 20; ++i) {
        esp_base_frp_management_listener_poll(7530 + i, status_handler, NULL);
        nanosleep(&(struct timespec){.tv_nsec = 1000000}, NULL);
    }
    config.management_key[0] = 0xbb;
    esp_base_frp_management_listener_configure(&config);
    assert_closed(fd);
    close(fd);
    assert(!esp_base_frp_management_listener_ready());
    esp_base_frp_management_listener_poll(7600, status_handler, NULL);
    assert(esp_base_frp_management_listener_ready());
    fd = connect_client(config.local_port);
    length = request(frame, sizeof frame, 'a', false);
    write_all(fd, frame, length);
    response(fd, 7700, 401, false);
    fd = connect_client(config.local_port);
    length = request(frame, sizeof frame, 'b', false);
    write_all(fd, frame, length);
    response(fd, 8800, 200, true);
    assert(authenticated == 4 && handled == 2);

    handler_status = 400;
    fd = connect_client(config.local_port);
    write_all(fd, frame, length);
    response(fd, 9000, 400, true);
    handler_status = 409;
    fd = connect_client(config.local_port);
    write_all(fd, frame, length);
    response(fd, 9200, 409, true);
    handler_status = 200;
    empty_result = true;
    fd = connect_client(config.local_port);
    write_all(fd, frame, length);
    response(fd, 9300, 500, false);
    empty_result = false;
    oversized_result = true;
    fd = connect_client(config.local_port);
    write_all(fd, frame, length);
    response(fd, 9350, 500, false);
    oversized_result = false;
    fail_sign = true;
    fd = connect_client(config.local_port);
    write_all(fd, frame, length);
    for (unsigned i = 0; i < 1000 && signed_responses < 5; ++i) {
        esp_base_frp_management_listener_poll(9400 + i, status_handler, NULL);
        nanosleep(&(struct timespec){.tv_nsec = 1000000}, NULL);
    }
    assert_closed(fd);
    close(fd);
    assert(authenticated == 9 && handled == 7 && signed_responses == 5);

    /* Only fixed paths reach the typed handler; a restart receipt is signed. */
    handler_status = 202;
    expected_command = ESP_BASE_FRP_MANAGEMENT_RESTART;
    fail_sign = false;
    length = request(frame, sizeof frame, 'b', false);
    char restart_frame[1200];
    const char *tail = strstr(frame, " HTTP/1.1");
    assert(tail);
    snprintf(restart_frame, sizeof restart_frame, "POST /api/v1/commands/restart%s", tail);
    fd = connect_client(config.local_port);
    write_all(fd, restart_frame, strlen(restart_frame));
    response(fd, 9500, 202, true);
    assert(!esp_base_frp_management_listener_response_pending());
    const unsigned before = authenticated;
    fd = connect_client(config.local_port);
    snprintf(restart_frame, sizeof restart_frame, "POST /api/v1/commands/restart?x=1%s", tail);
    write_all(fd, restart_frame, strlen(restart_frame));
    response(fd, 9600, 400, false);
    assert(authenticated == before);

    esp_base_frp_management_listener_configure(NULL);
    assert(!esp_base_frp_management_listener_ready());
    memset(config.management_key, 0, sizeof config.management_key);
    esp_base_frp_management_listener_configure(&config);
    esp_base_frp_management_listener_poll(9900, status_handler, NULL);
    assert(!esp_base_frp_management_listener_ready());
    memset(config.management_key, 0xbb, sizeof config.management_key);
    esp_base_frp_management_listener_configure(&config);
    esp_base_frp_management_listener_poll(10000, status_handler, NULL);
    test_extended_commands(config.local_port);
    test_uploads(&config);
    esp_base_frp_management_listener_configure(NULL);
    puts("  frp_management_listener passed (commands, upload handoff, exact framing, auth, timeout, concurrent cancel)");
}

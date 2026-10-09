// SPDX-License-Identifier: Apache-2.0
#include "esp_base_frp_management_listener.h"
#include "esp_base_network_auth.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#ifdef ESP_PLATFORM
#include "lwip/sockets.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"
#else
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <stdatomic.h>
#include <time.h>
#endif

#define FRP_HTTP_INPUT_BYTES 1537u
#define FRP_HTTP_EXTENDED_BODY_BYTES 1024u
#define FRP_HTTP_UPLOAD_HEADER_BYTES 1024u
#define FRP_HTTP_UPLOAD_PREFIX_BYTES 1024u
#define FRP_HTTP_UPLOAD_CONNECT_MS 5000u
#define FRP_HTTP_HEADER_BYTES 512u
#define FRP_HTTP_BODY_BYTES 384u
#define FRP_HTTP_OUTPUT_BYTES 1024u
#define FRP_HTTP_RESPONSE_OFFSET 256u
#define FRP_HTTP_TOTAL_MS 2000u

static int s_listener = -1, s_client = -1;
static uint16_t s_port;
static uint8_t s_key[EBASE_MANAGEMENT_KEY_BYTES];
static bool s_configured;
static uint64_t s_retry_at_ms, s_client_since_ms;
static uint8_t s_input[FRP_HTTP_INPUT_BYTES];
static size_t s_input_length;
static char s_output[FRP_HTTP_OUTPUT_BYTES];
static size_t s_output_length, s_output_sent;

/* Only bounded metadata copies are protected. Network waits/shutdown/close
 * run outside the critical section; borrowers retain the fd until finish can
 * close it, so configuration cancellation cannot target a reused fd. */
#ifdef ESP_PLATFORM
static portMUX_TYPE s_upload_lock = portMUX_INITIALIZER_UNLOCKED;
static void upload_lock(void) { portENTER_CRITICAL(&s_upload_lock); }
static void upload_unlock(void) { portEXIT_CRITICAL(&s_upload_lock); }
#else
static atomic_flag s_upload_lock = ATOMIC_FLAG_INIT;
static void upload_lock(void)
{ while (atomic_flag_test_and_set_explicit(&s_upload_lock, memory_order_acquire)) {} }
static void upload_unlock(void)
{ atomic_flag_clear_explicit(&s_upload_lock, memory_order_release); }
#endif

typedef struct {
    bool active, claimed, cancelled, finishing;
    int fd;
    unsigned borrowers;
    uint64_t armed_at_ms;
    uint32_t image_size_bytes, received_bytes, generation;
    char operation_id[37], device_id[37], boot_id[37];
    uint8_t sha256[32], key[EBASE_MANAGEMENT_KEY_BYTES];
    uint8_t prefix[FRP_HTTP_UPLOAD_PREFIX_BYTES];
    size_t prefix_length, prefix_offset;
} firmware_upload_t;

static firmware_upload_t s_upload = {.fd = -1};
static uint32_t s_upload_generation;


static void wipe(void *memory, size_t length)
{
    volatile uint8_t *bytes = memory;
    while (length--) *bytes++ = 0;
}

static void close_client(void)
{
    if (s_client >= 0) close(s_client);
    s_client = -1;
    s_input_length = s_output_length = s_output_sent = 0;
    wipe(s_input, sizeof s_input);
    wipe(s_output, sizeof s_output);
}

void esp_base_frp_management_listener_configure(const ebase_frp_config_t *config)
{
    bool configured = config && config->configured && config->local_port != 0;
    if (configured) {
        uint8_t nonzero = 0;
        for (size_t i = 0; i < EBASE_MANAGEMENT_KEY_BYTES; ++i)
            nonzero |= config->management_key[i];
        configured = nonzero != 0;
    }
    if (s_configured == configured && (!configured ||
        (s_port == config->local_port &&
         !memcmp(s_key, config->management_key, sizeof s_key)))) return;
    esp_base_frp_management_upload_cancel();
    close_client();
    if (s_listener >= 0) close(s_listener);
    s_listener = -1;
    upload_lock();
    wipe(s_key, sizeof s_key);
    s_configured = configured;
    s_port = configured ? config->local_port : 0;
    if (configured) memcpy(s_key, config->management_key, sizeof s_key);
    upload_unlock();
    s_retry_at_ms = 0;
}

bool esp_base_frp_management_listener_ready(void)
{
    return s_configured && s_listener >= 0;
}

static bool nonblocking(int fd)
{
    if (fd < 0 || fd >= FD_SETSIZE) return false;
    const int flags = fcntl(fd, F_GETFL);
    return flags >= 0 && fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
}

static void bind_listener(uint64_t now_ms)
{
    if (!s_configured || s_listener >= 0 || now_ms < s_retry_at_ms) return;
    int fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (fd < 0) goto retry;
    const int reuse = 1;
    const struct sockaddr_in address = {.sin_family = AF_INET,
        .sin_port = htons(s_port), .sin_addr.s_addr = htonl(0x7f000001u)};
    if (!nonblocking(fd) || setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof reuse) < 0 ||
        bind(fd, (const struct sockaddr *)&address, sizeof address) < 0 || listen(fd, 1) < 0) {
        close(fd);
        goto retry;
    }
    s_listener = fd;
    return;
retry:
    s_retry_at_ms = now_ms + 5000;
}

static int lower_hex_digit(uint8_t value)
{
    if (value >= '0' && value <= '9') return value - '0';
    if (value >= 'a' && value <= 'f') return value - 'a' + 10;
    return -1;
}

static bool canonical_uuid(const char *value)
{
    if (value == NULL || strnlen(value, 37) != 36) return false;
    for (size_t i = 0; i < 36; ++i) {
        if (i == 8 || i == 13 || i == 18 || i == 23) {
            if (value[i] != '-') return false;
        } else if (lower_hex_digit((uint8_t)value[i]) < 0) return false;
    }
    return value[14] == '4' && (value[19] == '8' || value[19] == '9' ||
        value[19] == 'a' || value[19] == 'b');
}

bool esp_base_frp_management_upload_arm(const char *operation_id, const char *device_id,
    const char *boot_id, uint32_t image_size_bytes, const uint8_t sha256[32], uint64_t now_ms)
{
    if (!canonical_uuid(operation_id) || !canonical_uuid(device_id) ||
        !canonical_uuid(boot_id) || image_size_bytes == 0 || sha256 == NULL) return false;
    upload_lock();
    if (!s_configured || s_upload.active) { upload_unlock(); return false; }
    s_upload = (firmware_upload_t){.active = true, .fd = -1, .armed_at_ms = now_ms,
        .image_size_bytes = image_size_bytes, .generation = ++s_upload_generation};
    memcpy(s_upload.operation_id, operation_id, 37);
    memcpy(s_upload.device_id, device_id, 37);
    memcpy(s_upload.boot_id, boot_id, 37);
    memcpy(s_upload.sha256, sha256, sizeof s_upload.sha256);
    memcpy(s_upload.key, s_key, sizeof s_upload.key);
    upload_unlock();
    return true;
}

bool esp_base_frp_management_upload_connected(void)
{
    upload_lock();
    const bool connected = s_upload.active && s_upload.claimed &&
        !s_upload.cancelled && !s_upload.finishing && s_upload.fd >= 0;
    upload_unlock();
    return connected;
}

static void return_upload_socket(void)
{
    int fd = -1;
    upload_lock();
    if (s_upload.borrowers > 0) --s_upload.borrowers;
    if (s_upload.active && s_upload.finishing && s_upload.borrowers == 0) {
        fd = s_upload.fd;
        wipe(&s_upload, sizeof s_upload);
        s_upload.fd = -1;
    }
    upload_unlock();
    if (fd >= 0) close(fd);
}

void esp_base_frp_management_upload_cancel(void)
{
    int fd = -1;
    upload_lock();
    if (s_upload.active) {
        s_upload.cancelled = true;
        if (s_upload.fd >= 0) { fd = s_upload.fd; ++s_upload.borrowers; }
    }
    upload_unlock();
    if (fd >= 0) {
        (void)shutdown(fd, SHUT_RDWR);
        return_upload_socket();
    }
}

static int64_t monotonic_us(void)
{
#ifdef ESP_PLATFORM
    return esp_timer_get_time();
#else
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) return -1;
    return (int64_t)now.tv_sec * INT64_C(1000000) + now.tv_nsec / 1000;
#endif
}

static int wait_upload_socket(int fd, bool writing, int64_t deadline_us)
{
    if (fd < 0 || fd >= FD_SETSIZE) return -1;
#ifdef ESP_PLATFORM
    const int64_t tick_us = (int64_t)portTICK_PERIOD_MS * 1000;
#else
    /* Host scheduling/return costs use the frozen Base 100 Hz unit. */
    const int64_t tick_us = 10000;
#endif
    const int64_t return_reserve_us = 2 * tick_us;
    for (;;) {
        const int64_t now = monotonic_us();
        if (now < 0 || now >= deadline_us) return 0;
        const int64_t left = deadline_us - now;
        int64_t wait_us = left;
        if (!writing) {
            wait_us = 0;
            if (left > return_reserve_us) {
#ifdef ESP_PLATFORM
                /* The locked lwIP wait rounds up and adds one tick (AT LEAST
                 * timeout). Reverse its bound and reserve the measured return/
                 * scheduling cost inside the complete read callback budget. */
                const int64_t ticks = (left - return_reserve_us - 1) / tick_us;
                if (ticks > 1) wait_us = (ticks - 1) * tick_us;
#else
                wait_us = left - return_reserve_us - 1;
#endif
                /* One scheduler unit per poll keeps normal timeout/return
                 * well inside the callback cap. Long POSIX waits additionally
                 * exhibited 150 ms timer coalescing on the actual host. */
                if (wait_us > tick_us) wait_us = tick_us;
            }
        }
        struct timeval timeout = {.tv_sec = (long)(wait_us / 1000000),
            .tv_usec = (long)(wait_us % 1000000)};
        fd_set descriptors;
        FD_ZERO(&descriptors);
        FD_SET(fd, &descriptors);
        /* A zero timeval is a readiness poll, not sys_arch's infinite wait. */
        const int ready = select(fd + 1, writing ? NULL : &descriptors,
            writing ? &descriptors : NULL, NULL, &timeout);
        if (ready >= 0) return ready;
        if (errno != EINTR) return -1;
    }
}

int esp_base_frp_management_upload_read(void *context, uint8_t *buffer, size_t capacity,
    uint32_t timeout_ms)
{
    (void)context;
    if (buffer == NULL || capacity == 0 || capacity > 64 || timeout_ms == 0 ||
        timeout_ms > 1000) return -1;
    const int64_t started_us = monotonic_us();
    if (started_us < 0) return -1;
    int fd = -1, result = -1;
    size_t remaining = 0;
    upload_lock();
    if (s_upload.active && s_upload.claimed && !s_upload.cancelled &&
        !s_upload.finishing && s_upload.fd >= 0) {
        fd = s_upload.fd;
        ++s_upload.borrowers;
        remaining = s_upload.image_size_bytes - s_upload.received_bytes;
        if (s_upload.prefix_offset < s_upload.prefix_length) {
            size_t count = s_upload.prefix_length - s_upload.prefix_offset;
            if (count > capacity) count = capacity;
            memcpy(buffer, s_upload.prefix + s_upload.prefix_offset, count);
            s_upload.prefix_offset += count;
            s_upload.received_bytes += (uint32_t)count;
            result = (int)count;
        }
    }
    upload_unlock();
    if (fd < 0) return -1;
    if (result > 0) { return_upload_socket(); return result; }
    if (remaining == 0) {
        /* Content-Length is the authenticated framing boundary. Refuse bytes
         * already queued beyond it; do not require FIN from an HTTP client
         * which is correctly waiting for the prepare response. */
        uint8_t extra;
        const ssize_t count = recv(fd, &extra, 1, MSG_PEEK);
        result = count == 0 || (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) ? 0 : -1;
        return_upload_socket();
        return result;
    }
    if (capacity > remaining) capacity = remaining;
    const int64_t deadline = started_us + (int64_t)timeout_ms * 1000;
    for (;;) {
        const int ready = wait_upload_socket(fd, false, deadline);
        if (ready <= 0) { result = ready == 0 ? -2 : -1; break; }
        const ssize_t count = recv(fd, buffer, capacity, 0);
        if (count > 0) {
            upload_lock();
            if (!s_upload.cancelled && !s_upload.finishing) {
                s_upload.received_bytes += (uint32_t)count;
                result = (int)count;
            }
            upload_unlock();
            break;
        }
        if (count == 0) break;
        if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) break;
    }
    return_upload_socket();
    return result;
}

static bool decimal_length(const uint8_t *value, size_t size, size_t maximum, size_t *length)
{
    if (!size || size > 10 || (size > 1 && value[0] == '0')) return false;
    size_t parsed = 0;
    for (size_t i = 0; i < size; ++i) {
        if (value[i] < '0' || value[i] > '9' || parsed > maximum / 10) return false;
        parsed = parsed * 10 + value[i] - '0';
        if (parsed > maximum) return false;
    }
    if (!parsed) return false;
    *length = parsed;
    return true;
}

static bool equal_header(const uint8_t *name, size_t length, const char *expected)
{
    return length == strlen(expected) && !strncasecmp((const char *)name, expected, length);
}

typedef struct {
    size_t header_length, body_length;
    bool upload;
    char operation_id[37];
    esp_base_frp_management_command_t command;
    uint8_t tag[EBASE_MANAGEMENT_TAG_BYTES];
} request_fields_t;

/* Fixed command POST paths, Content-Length framing and no transfer encodings.
 * Duplicate security/framing headers, obs-fold and pipelining are rejected. */
static bool parse_headers(request_fields_t *out)
{
    static const struct {
        const char *path;
        esp_base_frp_management_command_t command;
    } commands[] = {
        {"status", ESP_BASE_FRP_MANAGEMENT_STATUS},
        {"restart", ESP_BASE_FRP_MANAGEMENT_RESTART},
        {"firmware-status", ESP_BASE_FRP_MANAGEMENT_FIRMWARE_STATUS},
        {"ota-start", ESP_BASE_FRP_MANAGEMENT_OTA_START},
        {"ota-result", ESP_BASE_FRP_MANAGEMENT_OTA_RESULT},
        {"business-status", ESP_BASE_FRP_MANAGEMENT_BUSINESS_STATUS},
        {"business-pause", ESP_BASE_FRP_MANAGEMENT_BUSINESS_PAUSE},
        {"business-resume", ESP_BASE_FRP_MANAGEMENT_BUSINESS_RESUME},
    };
    size_t position = 0, maximum = FRP_HTTP_BODY_BYTES;
    for (size_t i = 0; i < sizeof commands / sizeof commands[0]; ++i) {
        char line[80];
        const int length = snprintf(line, sizeof line, "POST /api/v1/commands/%s HTTP/1.1\r\n",
            commands[i].path);
        if (length > 0 && (size_t)length <= s_input_length &&
            !memcmp(s_input, line, (size_t)length)) {
            out->command = commands[i].command;
            position = (size_t)length;
            if (i > 1) maximum = FRP_HTTP_EXTENDED_BODY_BYTES;
            break;
        }
    }
    static const char upload_prefix[] = "PUT /api/v1/ota-images/";
    static const char upload_suffix[] = " HTTP/1.1\r\n";
    const size_t upload_line_bytes = sizeof upload_prefix - 1 + 36 + sizeof upload_suffix - 1;
    if (!position && s_input_length >= upload_line_bytes &&
        !memcmp(s_input, upload_prefix, sizeof upload_prefix - 1) &&
        !memcmp(s_input + sizeof upload_prefix - 1 + 36, upload_suffix, sizeof upload_suffix - 1)) {
        memcpy(out->operation_id, s_input + sizeof upload_prefix - 1, 36);
        if (!canonical_uuid(out->operation_id)) return false;
        out->upload = true;
        position = upload_line_bytes;
        maximum = UINT32_MAX;
    }
    if (!position) return false;
    bool host = false, type = false, length = false, tag = false;
    while (position < out->header_length - 2) {
        size_t end = position;
        while (end + 1 < out->header_length &&
               !(s_input[end] == '\r' && s_input[end + 1] == '\n')) ++end;
        if (end + 1 >= out->header_length || end == position) return false;
        size_t colon = position;
        while (colon < end && s_input[colon] != ':') ++colon;
        if (colon == position || colon == end || colon + 1 >= end || s_input[colon + 1] != ' ') return false;
        for (size_t i = position; i < colon; ++i)
            if (!((s_input[i] >= 'A' && s_input[i] <= 'Z') ||
                  (s_input[i] >= 'a' && s_input[i] <= 'z') ||
                  (s_input[i] >= '0' && s_input[i] <= '9') || s_input[i] == '-')) return false;
        const uint8_t *value = s_input + colon + 2;
        const size_t value_length = end - colon - 2;
        for (size_t i = position; i < end; ++i)
            if (s_input[i] < 0x20 || s_input[i] > 0x7e) return false;
        if (equal_header(s_input + position, colon - position, "Host")) {
            if (host || !value_length) return false;
            host = true;
        } else if (equal_header(s_input + position, colon - position, "Content-Type")) {
            const char *content_type = out->upload ? "application/octet-stream" : "application/json";
            if (type || value_length != strlen(content_type) ||
                memcmp(value, content_type, value_length)) return false;
            type = true;
        } else if (equal_header(s_input + position, colon - position, "Content-Length")) {
            if (length || !decimal_length(value, value_length, maximum, &out->body_length)) return false;
            length = true;
        } else if (equal_header(s_input + position, colon - position, "X-ESP-Management-Tag")) {
            if (tag || value_length != 64) return false;
            for (size_t i = 0; i < sizeof out->tag; ++i) {
                const int hi = lower_hex_digit(value[i * 2]);
                const int lo = lower_hex_digit(value[i * 2 + 1]);
                if (hi < 0 || lo < 0) return false;
                out->tag[i] = (uint8_t)((hi << 4) | lo);
            }
            tag = true;
        } else if (equal_header(s_input + position, colon - position, "Transfer-Encoding") ||
                   equal_header(s_input + position, colon - position, "Expect")) return false;
        position = end + 2;
    }
    return position == out->header_length - 2 && host && type && length && tag;
}

static size_t build_response(const uint8_t key[EBASE_MANAGEMENT_KEY_BYTES], char *output,
    int status, const char *body, size_t body_length)
{
    char tag_header[90] = {0};
    if (body_length) {
        uint8_t tag[EBASE_MANAGEMENT_TAG_BYTES];
        if (!ebase_management_sign(key, (const uint8_t *)body, body_length, tag)) {
            return 0;
        }
        static const char hex[] = "0123456789abcdef";
        static const char prefix[] = "X-ESP-Management-Tag: ";
        memcpy(tag_header, prefix, sizeof prefix - 1);
        for (size_t i = 0; i < sizeof tag; ++i) {
            tag_header[sizeof prefix - 1 + i * 2] = hex[tag[i] >> 4];
            tag_header[sizeof prefix + i * 2] = hex[tag[i] & 15];
        }
        memcpy(tag_header + sizeof prefix - 1 + sizeof tag * 2, "\r\n", 2);
        wipe(tag, sizeof tag);
    }
    const char *reason = status == 200 ? "OK" : status == 202 ? "Accepted" : status == 400 ? "Bad Request" :
        status == 401 ? "Unauthorized" : status == 409 ? "Conflict" : "Internal Server Error";
    const int header = snprintf(output, FRP_HTTP_RESPONSE_OFFSET,
        "HTTP/1.1 %d %s\r\nContent-Type: application/json\r\n"
        "Content-Length: %zu\r\nConnection: close\r\n%s\r\n", status, reason, body_length,
        tag_header);
    if (header < 0 || (size_t)header >= FRP_HTTP_RESPONSE_OFFSET ||
        (size_t)header + body_length >= FRP_HTTP_OUTPUT_BYTES) {
        return 0;
    }
    if (body_length) memmove(output + header, body, body_length);
    return (size_t)header + body_length;
}


static void prepare_response(int status, const char *body, size_t body_length)
{
    s_output_length = build_response(s_key, s_output, status, body, body_length);
    if (!s_output_length) close_client();
}

bool esp_base_frp_management_upload_finish(int status, const char *body,
    size_t body_length, uint32_t timeout_ms)
{
    uint8_t key[EBASE_MANAGEMENT_KEY_BYTES] = {0};
    int fd = -1;
    bool may_respond = false;
    upload_lock();
    if (!s_upload.active || s_upload.finishing) { upload_unlock(); return false; }
    s_upload.finishing = true;
    ++s_upload.borrowers;
    fd = s_upload.fd;
    may_respond = fd >= 0 && !s_upload.cancelled;
    memcpy(key, s_upload.key, sizeof key);
    upload_unlock();
    bool success = false;
    if (may_respond && body != NULL && body_length > 0 &&
        body_length < FRP_HTTP_OUTPUT_BYTES - FRP_HTTP_RESPONSE_OFFSET &&
        timeout_ms > 0 && timeout_ms <= 1000 &&
        (status == 200 || status == 202 || status == 400 || status == 409 || status == 500)) {
        char output[FRP_HTTP_OUTPUT_BYTES];
        const size_t length = build_response(key, output, status, body, body_length);
        const int64_t started_us = monotonic_us();
        const int64_t deadline = started_us + (int64_t)timeout_ms * 1000;
        size_t sent_bytes = 0;
        while (length > 0 && started_us >= 0 && sent_bytes < length) {
            upload_lock();
            const bool cancelled = s_upload.cancelled;
            upload_unlock();
            if (cancelled || wait_upload_socket(fd, true, deadline) <= 0) break;
            const ssize_t sent = send(fd, output + sent_bytes, length - sent_bytes,
#ifdef MSG_NOSIGNAL
                MSG_NOSIGNAL
#else
                0
#endif
            );
            if (sent > 0) sent_bytes += (size_t)sent;
            else if (sent == 0 || (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)) break;
        }
        success = length > 0 && sent_bytes == length;
        wipe(output, sizeof output);
    }
    wipe(key, sizeof key);
    return_upload_socket();
    return success;
}

static void offer_upload(const request_fields_t *fields, uint64_t now_ms)
{
    char metadata[256], sha256_hex[65];
    uint8_t key[EBASE_MANAGEMENT_KEY_BYTES];
    uint32_t generation = 0;
    upload_lock();
    if (!s_upload.active || s_upload.cancelled || s_upload.finishing || s_upload.claimed ||
        strcmp(fields->operation_id, s_upload.operation_id) ||
        fields->body_length != s_upload.image_size_bytes || now_ms < s_upload.armed_at_ms ||
        now_ms - s_upload.armed_at_ms >= FRP_HTTP_UPLOAD_CONNECT_MS) {
        upload_unlock();
        prepare_response(409, NULL, 0);
        return;
    }
    static const char hex[] = "0123456789abcdef";
    for (size_t i = 0; i < 32; ++i) {
        sha256_hex[i * 2] = hex[s_upload.sha256[i] >> 4];
        sha256_hex[i * 2 + 1] = hex[s_upload.sha256[i] & 15];
    }
    sha256_hex[64] = '\0';
    const int metadata_length = snprintf(metadata, sizeof metadata,
        "esp-base-ota-upload-v1\n%s\n%s\n%s\n%u\n%s\n", s_upload.operation_id,
        s_upload.device_id, s_upload.boot_id, (unsigned)s_upload.image_size_bytes, sha256_hex);
    memcpy(key, s_upload.key, sizeof key);
    generation = s_upload.generation;
    upload_unlock();
    const int64_t authenticated_started_us = monotonic_us();
    const bool authenticated = metadata_length > 0 && (size_t)metadata_length < sizeof metadata &&
        ebase_management_authenticate(key, fields->tag, (const uint8_t *)metadata,
            (size_t)metadata_length);
    const int64_t authenticated_finished_us = monotonic_us();
    wipe(key, sizeof key);
    wipe(metadata, sizeof metadata);
    if (!authenticated || authenticated_started_us < 0 ||
        authenticated_finished_us < authenticated_started_us) {
        prepare_response(401, NULL, 0); return;
    }
    const uint64_t authentication_ms =
        (uint64_t)(authenticated_finished_us - authenticated_started_us) / 1000;
    const size_t prefix_length = s_input_length - fields->header_length;
    if (prefix_length > FRP_HTTP_UPLOAD_PREFIX_BYTES || prefix_length > fields->body_length) {
        prepare_response(400, NULL, 0);
        return;
    }
    upload_lock();
    if (!s_upload.active || s_upload.generation != generation || s_upload.cancelled ||
        s_upload.finishing || s_upload.claimed ||
        authentication_ms >= FRP_HTTP_UPLOAD_CONNECT_MS ||
        now_ms - s_upload.armed_at_ms >= FRP_HTTP_UPLOAD_CONNECT_MS - authentication_ms) {
        upload_unlock();
        prepare_response(409, NULL, 0);
        return;
    }
    s_upload.claimed = true;
    s_upload.fd = s_client;
    s_upload.prefix_length = prefix_length;
    if (prefix_length) memcpy(s_upload.prefix, s_input + fields->header_length, prefix_length);
    s_client = -1;
    upload_unlock();
    s_input_length = s_output_length = s_output_sent = 0;
    wipe(s_input, sizeof s_input);
    wipe(s_output, sizeof s_output);
}

static size_t input_header_length(void)
{
    for (size_t i = 3; i < s_input_length; ++i)
        if (!memcmp(s_input + i - 3, "\r\n\r\n", 4)) return i + 1;
    return 0;
}

static bool possible_upload_request(void)
{
    static const char prefix[] = "PUT /api/v1/ota-images/";
    const size_t count = s_input_length < sizeof prefix - 1 ? s_input_length : sizeof prefix - 1;
    return count > 0 && !memcmp(s_input, prefix, count);
}

static void receive_request(uint64_t now_ms, esp_base_frp_management_handler_t handler, void *context)
{
    if (s_input_length == sizeof s_input) { prepare_response(400, NULL, 0); return; }
    size_t capacity = sizeof s_input - s_input_length;
    if (!input_header_length()) {
        if (s_input_length >= FRP_HTTP_UPLOAD_HEADER_BYTES) { prepare_response(400, NULL, 0); return; }
        capacity = FRP_HTTP_UPLOAD_HEADER_BYTES - s_input_length;
    }
    const ssize_t received = recv(s_client, s_input + s_input_length, capacity, 0);
    if (received == 0) { close_client(); return; }
    if (received < 0) {
        if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) close_client();
        return;
    }
    s_input_length += (size_t)received;
    const size_t header_length = input_header_length();
    if (!header_length) {
        const size_t limit = possible_upload_request() ? FRP_HTTP_UPLOAD_HEADER_BYTES : FRP_HTTP_HEADER_BYTES;
        if (s_input_length >= limit) prepare_response(400, NULL, 0);
        return;
    }
    const bool upload_candidate = possible_upload_request();
    if (header_length > (upload_candidate ? FRP_HTTP_UPLOAD_HEADER_BYTES : FRP_HTTP_HEADER_BYTES)) {
        prepare_response(400, NULL, 0); return;
    }
    request_fields_t fields = {.header_length = header_length};
    if (!parse_headers(&fields)) { prepare_response(400, NULL, 0); return; }
    if (fields.upload) { offer_upload(&fields, now_ms); return; }
    if (header_length + fields.body_length > sizeof s_input ||
        s_input_length > header_length + fields.body_length) {
        prepare_response(400, NULL, 0); return;
    }
    if (s_input_length < header_length + fields.body_length) return;
    const uint8_t *body = s_input + header_length;
    if (!ebase_management_authenticate(s_key, fields.tag, body, fields.body_length)) {
        prepare_response(401, NULL, 0);
        return;
    }
    size_t response_length = 0;
    const int status = handler(fields.command, body, fields.body_length,
                               s_output + FRP_HTTP_RESPONSE_OFFSET,
                               sizeof s_output - FRP_HTTP_RESPONSE_OFFSET,
                               &response_length, context);
    if ((status != 200 && status != 202 && status != 400 && status != 409) ||
        !response_length || response_length >= sizeof s_output - FRP_HTTP_RESPONSE_OFFSET) {
        prepare_response(500, NULL, 0);
        return;
    }
    prepare_response(status, s_output + FRP_HTTP_RESPONSE_OFFSET, response_length);
}

void esp_base_frp_management_listener_poll(uint64_t now_ms,
                                       esp_base_frp_management_handler_t handler,
                                       void *context)
{
    if (!handler) return;
    bind_listener(now_ms);
    if (s_listener < 0) return;
    if (s_client < 0) {
        struct sockaddr_in peer = {0};
        socklen_t peer_length = sizeof peer;
        const int fd = accept(s_listener, (struct sockaddr *)&peer, &peer_length);
        if (fd >= 0) {
            if (peer.sin_family != AF_INET || peer.sin_addr.s_addr != htonl(0x7f000001u) ||
                !nonblocking(fd)) close(fd);
            else {
#ifdef SO_NOSIGPIPE
                const int no_sigpipe = 1;
                (void)setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &no_sigpipe, sizeof no_sigpipe);
#endif
                s_client = fd;
                s_client_since_ms = now_ms;
            }
        } else if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
            close(s_listener);
            s_listener = -1;
            s_retry_at_ms = now_ms + 5000;
        }
    }
    if (s_client < 0) return;
    const uint64_t client_budget_ms = possible_upload_request() ? FRP_HTTP_UPLOAD_CONNECT_MS : FRP_HTTP_TOTAL_MS;
    if (now_ms < s_client_since_ms || now_ms - s_client_since_ms >= client_budget_ms) { close_client(); return; }
    if (!s_output_length) receive_request(now_ms, handler, context);
    if (s_client < 0 || !s_output_length) return;
    const ssize_t sent = send(s_client, s_output + s_output_sent,
                              s_output_length - s_output_sent,
#ifdef MSG_NOSIGNAL
                              MSG_NOSIGNAL
#else
                              0
#endif
    );
    if (sent > 0) s_output_sent += (size_t)sent;
    else if (sent < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
        close_client(); return;
    }
    if (s_output_sent == s_output_length) close_client();
}

bool esp_base_frp_management_listener_response_pending(void)
{
    return s_client >= 0 && s_output_length > s_output_sent;
}

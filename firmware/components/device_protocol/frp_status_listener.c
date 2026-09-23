// SPDX-License-Identifier: Apache-2.0
#include "esp_base_frp_status_listener.h"
#include "esp_base_network_auth.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#ifdef ESP_PLATFORM
#include "lwip/sockets.h"
#else
#include <arpa/inet.h>
#include <sys/socket.h>
#endif

#define FRP_HTTP_INPUT_BYTES 897u
#define FRP_HTTP_HEADER_BYTES 512u
#define FRP_HTTP_BODY_BYTES 384u
#define FRP_HTTP_OUTPUT_BYTES 1024u
#define FRP_HTTP_RESPONSE_OFFSET 128u
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

void esp_base_frp_status_listener_configure(const ebase_frp_config_t *config)
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
    close_client();
    if (s_listener >= 0) close(s_listener);
    s_listener = -1;
    wipe(s_key, sizeof s_key);
    s_configured = configured;
    s_port = configured ? config->local_port : 0;
    if (configured) memcpy(s_key, config->management_key, sizeof s_key);
    s_retry_at_ms = 0;
}

bool esp_base_frp_status_listener_ready(void)
{
    return s_configured && s_listener >= 0;
}

static bool nonblocking(int fd)
{
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

static bool decimal_length(const uint8_t *value, size_t size, size_t *length)
{
    if (!size || size > 3) return false;
    size_t parsed = 0;
    for (size_t i = 0; i < size; ++i) {
        if (value[i] < '0' || value[i] > '9') return false;
        parsed = parsed * 10 + value[i] - '0';
    }
    if (!parsed || parsed > FRP_HTTP_BODY_BYTES) return false;
    *length = parsed;
    return true;
}

static bool equal_header(const uint8_t *name, size_t length, const char *expected)
{
    return length == strlen(expected) && !strncasecmp((const char *)name, expected, length);
}

typedef struct {
    size_t header_length, body_length;
    uint8_t tag[EBASE_MANAGEMENT_TAG_BYTES];
} request_fields_t;

/* A single fixed POST, Content-Length framing and no transfer encodings.
 * Duplicate security/framing headers, obs-fold and pipelining are rejected. */
static bool parse_headers(request_fields_t *out)
{
    static const char request_line[] = "POST /api/v1/commands/status HTTP/1.1\r\n";
    if (s_input_length < sizeof request_line - 1 ||
        memcmp(s_input, request_line, sizeof request_line - 1)) return false;
    size_t position = sizeof request_line - 1;
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
            if (type || value_length != 16 || memcmp(value, "application/json", 16)) return false;
            type = true;
        } else if (equal_header(s_input + position, colon - position, "Content-Length")) {
            if (length || !decimal_length(value, value_length, &out->body_length)) return false;
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

static void prepare_response(int status, const char *body, size_t body_length)
{
    const char *reason = status == 200 ? "OK" : status == 400 ? "Bad Request" :
        status == 401 ? "Unauthorized" : status == 409 ? "Conflict" : "Internal Server Error";
    const int header = snprintf(s_output, FRP_HTTP_RESPONSE_OFFSET,
        "HTTP/1.1 %d %s\r\nContent-Type: application/json\r\n"
        "Content-Length: %zu\r\nConnection: close\r\n\r\n", status, reason, body_length);
    if (header < 0 || (size_t)header >= FRP_HTTP_RESPONSE_OFFSET ||
        (size_t)header + body_length >= sizeof s_output) {
        close_client();
        return;
    }
    if (body_length) memmove(s_output + header, body, body_length);
    s_output_length = (size_t)header + body_length;
}

static void receive_request(esp_base_frp_status_handler_t handler, void *context)
{
    if (s_input_length == sizeof s_input) { prepare_response(400, NULL, 0); return; }
    const ssize_t received = recv(s_client, s_input + s_input_length,
                                  sizeof s_input - s_input_length, 0);
    if (received == 0) { close_client(); return; }
    if (received < 0) {
        if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) close_client();
        return;
    }
    s_input_length += (size_t)received;
    size_t header_length = 0;
    for (size_t i = 3; i < s_input_length; ++i)
        if (!memcmp(s_input + i - 3, "\r\n\r\n", 4)) { header_length = i + 1; break; }
    if (!header_length) {
        if (s_input_length > FRP_HTTP_HEADER_BYTES) prepare_response(400, NULL, 0);
        return;
    }
    if (header_length > FRP_HTTP_HEADER_BYTES) { prepare_response(400, NULL, 0); return; }
    request_fields_t fields = {.header_length = header_length};
    if (!parse_headers(&fields) || header_length + fields.body_length > sizeof s_input ||
        s_input_length > header_length + fields.body_length) {
        prepare_response(400, NULL, 0);
        return;
    }
    if (s_input_length < header_length + fields.body_length) return;
    const uint8_t *body = s_input + header_length;
    if (!ebase_management_authenticate(s_key, fields.tag, body, fields.body_length)) {
        prepare_response(401, NULL, 0);
        return;
    }
    size_t response_length = 0;
    const int status = handler(body, fields.body_length,
                               s_output + FRP_HTTP_RESPONSE_OFFSET,
                               sizeof s_output - FRP_HTTP_RESPONSE_OFFSET,
                               &response_length, context);
    if ((status != 200 && status != 400 && status != 409) ||
        response_length > sizeof s_output - FRP_HTTP_RESPONSE_OFFSET) {
        prepare_response(500, NULL, 0);
        return;
    }
    prepare_response(status, s_output + FRP_HTTP_RESPONSE_OFFSET, response_length);
}

void esp_base_frp_status_listener_poll(uint64_t now_ms,
                                       esp_base_frp_status_handler_t handler,
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
    if (now_ms - s_client_since_ms >= FRP_HTTP_TOTAL_MS) { close_client(); return; }
    if (!s_output_length) receive_request(handler, context);
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

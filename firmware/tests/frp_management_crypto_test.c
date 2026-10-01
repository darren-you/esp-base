// SPDX-License-Identifier: Apache-2.0
/* Explicit host probe: actual listener, parser and auth wrapper. OpenSSL is a
 * test PSA port; this does not exercise FRPS, IDF crypto, TLS or a real board. */
#include "esp_base_command.h"
#include "esp_base_frp_management_listener.h"
#include "esp_base_network_auth.h"
#include "psa/crypto.h"

#include <arpa/inet.h>
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <openssl/crypto.h>
#include <openssl/hmac.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>
#include "fakes/network_auth_openssl.inc"

#define DEVICE "22222222-2222-4222-8222-222222222222"
#define BOOT "33333333-3333-4333-8333-333333333333"
#define REQUEST "11111111-1111-4111-8111-111111111111"
static const char request[] = "{\"protocol_version\":1,\"device_id\":\"" DEVICE
    "\",\"request_id\":\"" REQUEST "\",\"command\":\"status\"}";
/* Independently frozen with Python's hmac/hashlib using public key 00..1f. */
static const char request_tag[] = "b0f28bc8e99d87489146c8b94e102b5a921bdda91d20494ab848bb9e4d6cdaba";
static const char response_tag[] = "e684f36e21beb63cc3e7ee59b83139142f9d0cf343da77c555b7c07a94d72a1a";

static const char restart_request[] = "{\"protocol_version\":1,\"device_id\":\"22222222-2222-4222-8222-222222222222\",\"request_id\":\"11111111-1111-4111-8111-111111111111\",\"command\":\"restart\",\"target_boot_id\":\"33333333-3333-4333-8333-333333333333\",\"expires_at_uptime_ms\":31000,\"parameters\":{}}";
static const char restart_request_tag[] = "a89ef400315a3507ee46a74a8448731d57e77f5d693e6b4c6af47075ab168495";
static const char restart_response_tag[] = "9229e60eb16716943c314cd464830ec08338360b81ff62284c0efe90824d8c65";
static ebase_request_guard_t guard;
static unsigned restart_admissions;
static uint8_t key[32];
static unsigned handled;

static void tag_hex(const uint8_t tag[32], char hex[65])
{
    for (unsigned i = 0; i < 32; ++i) snprintf(hex + i * 2, 3, "%02x", tag[i]);
}

static int handle(esp_base_frp_management_command_t command, const uint8_t *body, size_t length, char *out, size_t capacity,
                  size_t *written, void *context)
{
    (void)context;
    assert(!key_present); /* Request verify must have cleaned up before parse. */
    ebase_request_t parsed;
    const bool restart = command == ESP_BASE_FRP_MANAGEMENT_RESTART;
    const char *error = restart ? ebase_parse_frp_restart((const char *)body, length, &parsed) :
        ebase_parse_frp_status((const char *)body, length, &parsed);
    ++handled;
    if (!restart) assert(parsed.boot_id[0] == 0 && parsed.expires_at_ms == 0);
    int status = error ? 400 : 200;
    if (!error && strcmp(parsed.device_id, DEVICE)) { error = "wrong_device"; status = 409; }
    if (restart && !error) {
        size_t slot = 0;
        const ebase_admission_t decision = ebase_admit(&guard, &parsed, DEVICE, BOOT, 1000, &slot);
        static const char *const errors[] = {NULL, NULL, "invalid_identity", "wrong_device",
            "wrong_boot", "expired", "invalid_deadline", "request_conflict", "capacity_exceeded"};
        error = errors[decision];
        if (decision == EBASE_ACCEPT) ++restart_admissions;
        status = error ? 409 : 202;
    }
    const int n = snprintf(out, capacity,
        "{\"protocol_version\":1,\"device_id\":\"" DEVICE "\",\"boot_id\":\"" BOOT
        "\",\"request_id\":%s%s%s,\"state\":\"%s\",\"error_code\":%s%s%s,\"result\":%s}",
        parsed.request_id[0] ? "\"" : "null", parsed.request_id, parsed.request_id[0] ? "\"" : "",
        error ? "failed" : restart ? "running" : "succeeded", error ? "\"" : "null", error ? error : "", error ? "\"" : "",
        error || restart ? "null" : "{\"uptime_ms\":1000}");
    assert(n > 0 && (size_t)n < capacity);
    *written = (size_t)n;
    return status;
}

static uint16_t unused_port(void)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    assert(fd >= 0);
    struct sockaddr_in address = {.sin_family = AF_INET, .sin_port = 0,
        .sin_addr.s_addr = htonl(0x7f000001u)};
    assert(bind(fd, (struct sockaddr *)&address, sizeof address) == 0);
    socklen_t size = sizeof address;
    assert(getsockname(fd, (struct sockaddr *)&address, &size) == 0);
    close(fd);
    return ntohs(address.sin_port);
}

static void roundtrip(const char *command, uint16_t port, const char *body, bool tamper_request,
                      int expected_status, bool signed_body)
{
    uint8_t tag[32];
    assert(ebase_management_sign(key, (const uint8_t *)body, strlen(body), tag));
    assert(!key_present);
    char hex[65];
    tag_hex(tag, hex);
    if (!strcmp(body, request)) assert(!strcmp(hex, request_tag));
    if (!strcmp(body, restart_request)) assert(!strcmp(hex, restart_request_tag));
    if (tamper_request) hex[0] = hex[0] == '0' ? '1' : '0';
    char frame[1024];
    const int size = snprintf(frame, sizeof frame,
        "POST /api/v1/commands/%s HTTP/1.1\r\nHost: device\r\nContent-Type: application/json\r\n"
        "Content-Length: %zu\r\nX-ESP-Management-Tag: %s\r\n\r\n%s", command, strlen(body), hex, body);
    assert(size > 0 && (size_t)size < sizeof frame);
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    assert(fd >= 0);
    struct sockaddr_in address = {.sin_family = AF_INET, .sin_port = htons(port),
        .sin_addr.s_addr = htonl(0x7f000001u)};
    assert(connect(fd, (struct sockaddr *)&address, sizeof address) == 0);
    size_t sent = 0;
    while (sent < (size_t)size) {
        const ssize_t n = send(fd, frame + sent, (size_t)size - sent, 0);
        assert(n > 0);
        sent += (size_t)n;
    }
    const int flags = fcntl(fd, F_GETFL);
    assert(flags >= 0 && fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0);
    char reply[1025] = {0};
    size_t received = 0;
    bool closed = false;
    for (unsigned i = 0; i < 1000; ++i) {
        esp_base_frp_management_listener_poll(i + 1000, handle, NULL);
        const ssize_t n = recv(fd, reply + received, sizeof reply - received - 1, 0);
        if (n > 0) received += (size_t)n;
        else if (n == 0) { closed = true; break; }
        else assert(errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR);
        assert(received < sizeof reply - 1);
        nanosleep(&(struct timespec){.tv_nsec = 1000000}, NULL);
    }
    close(fd);
    assert(closed && !key_present);
    char prefix[32];
    snprintf(prefix, sizeof prefix, "HTTP/1.1 %d ", expected_status);
    assert(!strncmp(reply, prefix, strlen(prefix)));
    const char *payload = strstr(reply, "\r\n\r\n");
    assert(payload);
    payload += 4;
    const char *header_tag = strstr(reply, "X-ESP-Management-Tag: ");
    assert((header_tag != NULL) == signed_body);
    if (!signed_body) { assert(!payload[0]); return; }
    header_tag += strlen("X-ESP-Management-Tag: ");
    assert(header_tag[64] == '\r' && header_tag[65] == '\n');
    assert(!strstr(header_tag + 66, "X-ESP-Management-Tag: "));
    for (unsigned i = 0; i < 32; ++i) {
        unsigned byte = 0;
        assert(sscanf(header_tag + i * 2, "%2x", &byte) == 1);
        tag[i] = (uint8_t)byte;
    }
    assert(ebase_management_authenticate(key, tag, (const uint8_t *)payload, strlen(payload)));
    if (!strcmp(body, request) && expected_status == 200) {
        assert(!strncmp(header_tag, response_tag, 64));
        assert(strstr(payload, "\"boot_id\":\"" BOOT "\"") &&
               strstr(payload, "\"request_id\":\"" REQUEST "\""));
    }
    if (!strcmp(body, restart_request) && expected_status == 202)
        assert(!strncmp(header_tag, restart_response_tag, 64));
    uint8_t wrong_key[32];
    memcpy(wrong_key, key, sizeof wrong_key);
    wrong_key[0] ^= 1;
    assert(!ebase_management_authenticate(wrong_key, tag, (const uint8_t *)payload, strlen(payload)));
    tag[31] ^= 1;
    assert(!ebase_management_authenticate(key, tag, (const uint8_t *)payload, strlen(payload)));
    tag[31] ^= 1;
    char changed[768];
    assert(strlen(payload) < sizeof changed);
    strcpy(changed, payload);
    changed[1] ^= 1;
    assert(!ebase_management_authenticate(key, tag, (const uint8_t *)changed, strlen(changed)));
    assert(!key_present);
}

int main(void)
{
    for (unsigned i = 0; i < sizeof key; ++i) key[i] = (uint8_t)i;
    ebase_frp_config_t config = {.configured = true, .local_port = unused_port()};
    memcpy(config.management_key, key, sizeof key);
    esp_base_frp_management_listener_configure(&config);
    esp_base_frp_management_listener_poll(1000, handle, NULL);
    assert(esp_base_frp_management_listener_ready());
    roundtrip("status", config.local_port, request, false, 200, true);
    roundtrip("status", config.local_port, request, true, 401, false);
    roundtrip("status", config.local_port,
        "{\"protocol_version\":1,\"device_id\":\"" DEVICE "\",\"request_id\":\"" REQUEST
        "\",\"command\":\"status\",\"target_boot_id\":\"" BOOT "\",\"expires_at_uptime_ms\":30000}",
        false, 400, true);
    roundtrip("status", config.local_port,
        "{\"protocol_version\":1,\"device_id\":\"99999999-9999-4999-8999-999999999999\","
        "\"request_id\":\"" REQUEST "\",\"command\":\"status\"}", false, 409, true);
    assert(handled == 3);
    roundtrip("restart", config.local_port, restart_request, true, 401, false);
    assert(restart_admissions == 0U);
    roundtrip("restart", config.local_port, request, false, 400, true);
    roundtrip("status", config.local_port, restart_request, false, 400, true);
    roundtrip("restart", config.local_port, restart_request, false, 202, true);
    roundtrip("restart", config.local_port, restart_request, false, 202, true);
    assert(restart_admissions == 1U && guard.count == 1U);
    char changed_request[384];
    strcpy(changed_request, restart_request);
    char *deadline = strstr(changed_request, "31000");
    assert(deadline); deadline[0] = '2';
    roundtrip("restart", config.local_port, changed_request, false, 409, true);
    strcpy(changed_request, restart_request);
    char *boot = strstr(changed_request, BOOT);
    assert(boot); boot[0] = '9';
    roundtrip("restart", config.local_port, changed_request, false, 409, true);
    assert(restart_admissions == 1U && guard.count == 1U);
    esp_base_frp_management_listener_configure(NULL);
    puts("  frp_management_crypto passed (actual HTTP/parser/auth; frozen MACs; signed errors; tampering)");
}

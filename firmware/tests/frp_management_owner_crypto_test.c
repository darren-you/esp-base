// SPDX-License-Identifier: Apache-2.0
/* Actual HTTP, HMAC, decoder, Base handler, serializer and restart scheduling.
 * Reuse the explicit SDK/storage/network fakes; no board or FRPS is involved. */
#define ESP_BASE_TEST_REAL_FRP_LISTENER 1
#define main protocol_fixture_main
#include "protocol_ota_owner_test.c"
#undef main

#include "esp_base_network_auth.h"
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <openssl/hmac.h>
#include <sys/socket.h>
#include <time.h>

static uint8_t management_key[32];

static uint16_t management_unused_port(void)
{
    const int fd = socket(AF_INET, SOCK_STREAM, 0);
    assert(fd >= 0);
    struct sockaddr_in address = {.sin_family = AF_INET,
        .sin_port = 0, .sin_addr.s_addr = htonl(0x7f000001U)};
    assert(bind(fd, (struct sockaddr *)&address, sizeof address) == 0);
    socklen_t size = sizeof address;
    assert(getsockname(fd, (struct sockaddr *)&address, &size) == 0);
    close(fd);
    return ntohs(address.sin_port);
}

static void management_roundtrip(uint16_t port, const char *command,
                                  const char *body, bool tamper, int expected_http,
                                  const char *expected_state, const char *expected_error)
{
    uint8_t digest[32]; unsigned digest_size = 0;
    assert(HMAC(EVP_sha256(), management_key, sizeof management_key,
        (const uint8_t *)body, strlen(body), digest, &digest_size) && digest_size == 32U);
    if (tamper) digest[0] ^= 1;
    char tag[65];
    for (unsigned i = 0; i < 32U; ++i) snprintf(tag + i * 2U, 3, "%02x", digest[i]);
    char frame[1024];
    const int size = snprintf(frame, sizeof frame,
        "POST /api/v1/commands/%s HTTP/1.1\r\nHost: device\r\n"
        "Content-Type: application/json\r\nContent-Length: %zu\r\n"
        "X-ESP-Management-Tag: %s\r\n\r\n%s", command, strlen(body), tag, body);
    assert(size > 0 && (size_t)size < sizeof frame);
    const int fd = socket(AF_INET, SOCK_STREAM, 0);
    assert(fd >= 0);
    const struct sockaddr_in address = {.sin_family = AF_INET,
        .sin_port = htons(port), .sin_addr.s_addr = htonl(0x7f000001U)};
    assert(connect(fd, (const struct sockaddr *)&address, sizeof address) == 0);
    size_t sent = 0;
    while (sent < (size_t)size) {
        const ssize_t n = send(fd, frame + sent, (size_t)size - sent, 0);
        assert(n > 0); sent += (size_t)n;
    }
    const int flags = fcntl(fd, F_GETFL);
    assert(flags >= 0 && fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0);
    char reply[1025] = {0}; size_t received = 0; bool closed = false;
    for (unsigned i = 0; i < 1000U; ++i) {
        esp_base_frp_management_listener_poll(fake_now_ms, handle_frp_management, NULL);
        /* Keep the monotonic test clock below the restart's minimum delay. */
        poll_frp_restart(fake_now_ms);
        const ssize_t n = recv(fd, reply + received, sizeof reply - received - 1U, 0);
        if (n > 0) received += (size_t)n;
        else if (n == 0) { closed = true; break; }
        else assert(errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR);
        assert(received < sizeof reply - 1U);
        nanosleep(&(struct timespec){.tv_nsec = 1000000}, NULL);
    }
    close(fd);
    assert(closed && restart_calls == 0U);
    char prefix[32]; snprintf(prefix, sizeof prefix, "HTTP/1.1 %d ", expected_http);
    assert(!strncmp(reply, prefix, strlen(prefix)));
    const char *payload = strstr(reply, "\r\n\r\n"); assert(payload); payload += 4;
    const char *response_tag = strstr(reply, "X-ESP-Management-Tag: ");
    if (tamper) { assert(!payload[0] && !response_tag); return; }
    assert(response_tag); response_tag += strlen("X-ESP-Management-Tag: ");
    assert(response_tag[64] == '\r' && response_tag[65] == '\n');
    assert(!strstr(response_tag + 66, "X-ESP-Management-Tag: "));
    for (unsigned i = 0; i < 32U; ++i) {
        unsigned byte = 0;
        assert(sscanf(response_tag + i * 2U, "%2x", &byte) == 1);
        digest[i] = (uint8_t)byte;
    }
    assert(ebase_management_authenticate(management_key, digest,
        (const uint8_t *)payload, strlen(payload)));
    char field[100]; snprintf(field, sizeof field, "\"state\":\"%s\"", expected_state);
    assert(strstr(payload, field));
    if (expected_error) assert(strstr(payload, expected_error));
    else assert(strstr(payload, "\"error_code\":null"));
    assert(strstr(payload, "22222222-2222-4222-8222-222222222222") &&
           strstr(payload, "33333333-3333-4333-8333-333333333333"));
}

int main(void)
{
    reset_case();
    fake_frp_state = "ready";
    for (unsigned i = 0; i < sizeof management_key; ++i) management_key[i] = (uint8_t)i;
    ebase_frp_config_t config = {.configured = true, .local_port = management_unused_port()};
    memcpy(config.management_key, management_key, sizeof management_key);
    esp_base_frp_management_listener_configure(&config);
    esp_base_frp_management_listener_poll(fake_now_ms, handle_frp_management, NULL);
    assert(esp_base_frp_management_listener_ready());
    const char status[] = "{\"protocol_version\":1,\"device_id\":\"22222222-2222-4222-8222-222222222222\","
        "\"request_id\":\"11111111-1111-4111-8111-111111111111\",\"command\":\"status\"}";
    management_roundtrip(config.local_port, "status", status, false, 200, "succeeded", NULL);
    char request[384];
    restart_request(request, 1U, s_context.device_id, s_boot_id, 31000U);
    management_roundtrip(config.local_port, "restart", request, true, 401, NULL, NULL);
    assert(s_guard.count == 0U && !s_frp_restart_pending);
    management_roundtrip(config.local_port, "status", request, false, 400, "failed", "invalid_request");
    management_roundtrip(config.local_port, "restart", status, false, 400, "failed", "invalid_request");
    assert(s_guard.count == 0U && !s_frp_restart_pending);
    management_roundtrip(config.local_port, "restart", request, false, 202, "running", NULL);
    management_roundtrip(config.local_port, "restart", request, false, 202, "running", NULL);
    assert(s_guard.count == 1U && s_frp_restart_pending && s_frp_restart_since_ms == 1000U);
    restart_request(request, 2U, s_context.device_id, s_boot_id, 31000U);
    management_roundtrip(config.local_port, "restart", request, false, 409, "failed", "operation_busy");
    assert(s_guard.count == 2U && task_calls == 0U && register_calls == 0U &&
           config_commit_calls == 0U && !esp_base_frp_management_listener_response_pending());
    poll_frp_restart(1099U); assert(restart_calls == 0U);
    poll_frp_restart(1100U); assert(restart_calls == 1U && !s_frp_restart_pending);
    esp_base_frp_management_listener_configure(NULL);
    puts("  frp_management_owner_crypto passed (real HTTP/auth/Base handler; replay; bounded deferred restart; fake SDK)");
    return 0;
}

// SPDX-License-Identifier: Apache-2.0
#include "esp_base_frp_status_listener.h"
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

static const char body[] =
    "{\"protocol_version\":1,\"device_id\":\"22222222-2222-4222-8222-222222222222\","
    "\"target_boot_id\":\"33333333-3333-4333-8333-333333333333\","
    "\"request_id\":\"11111111-1111-4111-8111-111111111111\","
    "\"command\":\"status\",\"expires_at_uptime_ms\":30000}";
static unsigned authenticated, handled;

bool ebase_management_authenticate(const uint8_t key[EBASE_MANAGEMENT_KEY_BYTES],
                                   const uint8_t tag[EBASE_MANAGEMENT_TAG_BYTES],
                                   const uint8_t *request, size_t length)
{
    ++authenticated;
    if (length != sizeof body - 1 || memcmp(request, body, length)) return false;
    for (size_t i = 0; i < EBASE_MANAGEMENT_TAG_BYTES; ++i)
        if (tag[i] != key[0]) return false;
    return true;
}

static int status_handler(const uint8_t *request, size_t length, char *response,
                          size_t capacity, size_t *written, void *context)
{
    (void)context;
    assert(length == sizeof body - 1 && !memcmp(request, body, length));
    ++handled;
    const char result[] = "{\"state\":\"succeeded\"}";
    assert(capacity >= sizeof result);
    memcpy(response, result, sizeof result - 1);
    *written = sizeof result - 1;
    return 200;
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
        esp_base_frp_status_listener_poll(start + i, status_handler, NULL);
        const ssize_t count = recv(fd, text + length, sizeof text - length - 1, 0);
        if (count > 0) length += (size_t)count;
        else if (count == 0) break;
        else assert(errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR);
    }
    char prefix[32];
    snprintf(prefix, sizeof prefix, "HTTP/1.1 %d ", status);
    assert(strstr(text, prefix) == text);
    assert((strstr(text, "\"succeeded\"") != NULL) == has_result);
    if (status == 401) assert(strstr(text, "Content-Length: 0\r\n") != NULL);
    close(fd);
}

int main(void)
{
    ebase_frp_config_t config = {.configured = true, .local_port = unused_port()};
    memset(config.management_key, 0xaa, sizeof config.management_key);
    esp_base_frp_status_listener_configure(&config);
    esp_base_frp_status_listener_poll(1000, status_handler, NULL);
    assert(esp_base_frp_status_listener_ready());

    char frame[1200];
    size_t length = request(frame, sizeof frame, 'a', false);
    int fd = connect_client(config.local_port);
    write_all(fd, frame, 23);
    esp_base_frp_status_listener_poll(1100, status_handler, NULL);
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
        esp_base_frp_status_listener_poll(5500 + i, status_handler, NULL);
        nanosleep(&(struct timespec){.tv_nsec = 1000000}, NULL);
    }
    esp_base_frp_status_listener_poll(7520, status_handler, NULL);
    assert_closed(fd);
    close(fd);
    assert(authenticated == 2 && handled == 1);

    fd = connect_client(config.local_port);
    write_all(fd, frame, 16);
    for (unsigned i = 0; i < 20; ++i) {
        esp_base_frp_status_listener_poll(7530 + i, status_handler, NULL);
        nanosleep(&(struct timespec){.tv_nsec = 1000000}, NULL);
    }
    config.management_key[0] = 0xbb;
    esp_base_frp_status_listener_configure(&config);
    assert_closed(fd);
    close(fd);
    assert(!esp_base_frp_status_listener_ready());
    esp_base_frp_status_listener_poll(7600, status_handler, NULL);
    assert(esp_base_frp_status_listener_ready());
    fd = connect_client(config.local_port);
    length = request(frame, sizeof frame, 'a', false);
    write_all(fd, frame, length);
    response(fd, 7700, 401, false);
    fd = connect_client(config.local_port);
    length = request(frame, sizeof frame, 'b', false);
    write_all(fd, frame, length);
    response(fd, 8800, 200, true);
    assert(authenticated == 4 && handled == 2);

    esp_base_frp_status_listener_configure(NULL);
    assert(!esp_base_frp_status_listener_ready());
    memset(config.management_key, 0, sizeof config.management_key);
    esp_base_frp_status_listener_configure(&config);
    esp_base_frp_status_listener_poll(9900, status_handler, NULL);
    assert(!esp_base_frp_status_listener_ready());
    puts("  frp_status_listener passed (loopback, framing, authentication, reconfigure, deadline)");
}

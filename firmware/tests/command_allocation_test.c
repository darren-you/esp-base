// SPDX-License-Identifier: Apache-2.0
#include "esp_base_command.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Instrument only the decoder's allocations. cJSON and the test continue to
 * use the real allocator. Inspect each block before it leaves our ownership. */
static struct { void *data; size_t size; } blocks[4];
static size_t attempts, fail_at, live_bytes, peak_bytes;
static unsigned delivered, rejected;
static size_t expected_size;
static const char query[] = "{\"protocol_version\":1,\"request_id\":\"11111111-1111-4111-8111-111111111111\",\"command\":\"status\"}";

void *ebase_line_test_malloc(size_t size)
{
    ++attempts;
    if (attempts == fail_at) return NULL;
    void *data = malloc(size);
    assert(data != NULL);
    memset(data, 0xa5, size);
    for (size_t i = 0; i < sizeof blocks / sizeof *blocks; ++i) {
        if (blocks[i].data) continue;
        blocks[i].data = data;
        blocks[i].size = size;
        live_bytes += size;
        if (peak_bytes < live_bytes) peak_bytes = live_bytes;
        return data;
    }
    abort();
}

void ebase_line_test_free(void *data)
{
    if (!data) return;
    for (size_t i = 0; i < sizeof blocks / sizeof *blocks; ++i) {
        if (blocks[i].data != data) continue;
        for (size_t j = 0; j < blocks[i].size; ++j)
            assert(((const unsigned char *)data)[j] == 0U);
        live_bytes -= blocks[i].size;
        memset(&blocks[i], 0, sizeof blocks[i]);
        free(data);
        return;
    }
    abort();
}

static void receive(const char *line, size_t size, void *context)
{
    (void)context;
    if (!line) { assert(size == 0U); ++rejected; return; }
    assert(size == expected_size);
    assert(line[size] == '\0');
    if (size == sizeof query - 1U) assert(memcmp(line, query, size) == 0);
    else for (size_t i = 0; i < size; ++i) assert(line[i] == 's');
    ++delivered;
}

static void reset_counters(void)
{
    assert(live_bytes == 0U);
    attempts = fail_at = live_bytes = peak_bytes = 0U;
    delivered = rejected = 0U;
}

static void feed_query(ebase_line_reader_t *reader)
{
    expected_size = sizeof query - 1U;
    ebase_line_feed(reader, query, expected_size, receive, NULL);
    ebase_line_feed(reader, "\n", 1U, receive, NULL);
    assert(reader->data == NULL && reader->length == 0U &&
           reader->capacity == 0U && !reader->discard && live_bytes == 0U);
}

static void command_payload_tests(void)
{
    const char *const configuration =
        "{\"protocol_version\":1,\"request_id\":\"11111111-1111-4111-8111-111111111111\","
        "\"command\":\"config.set\",\"device_id\":\"22222222-2222-4222-8222-222222222222\","
        "\"target_boot_id\":\"33333333-3333-4333-8333-333333333333\","
        "\"expires_at_uptime_ms\":31000,\"parameters\":{\"expected_revision\":0,"
        "\"config\":{\"schema_version\":3,\"wifi\":{\"ssid\":\"test\","
        "\"password\":\"test-password\"},\"mqtt\":null,\"frp\":null,\"business\":null}}}";
    const char *const operation_query =
        "{\"protocol_version\":1,\"request_id\":\"11111111-1111-4111-8111-111111111111\","
        "\"command\":\"ota.result\",\"parameters\":{"
        "\"operation_id\":\"44444444-4444-4444-8444-444444444444\"}}";
    ebase_command_t command = {0};
    reset_counters();
    assert(!ebase_parse_command(query, sizeof query - 1U, &command,
                                ebase_line_test_malloc));
    assert(command.kind == EBASE_STATUS && command.payload == NULL && attempts == 0U);
    assert(sizeof command < 256U);
    assert(!ebase_parse_command(configuration, strlen(configuration), &command,
                                ebase_line_test_malloc));
    assert(command.kind == EBASE_CONFIG_SET && command.config != NULL &&
           command.config->wifi.configured &&
           !strcmp(command.config->wifi.password, "test-password"));
    assert(attempts == 1U && live_bytes == sizeof *command.config &&
           command.payload_size_bytes == live_bytes);
    /* Switching to a read-only command wipes and releases previous secrets. */
    assert(!ebase_parse_command(query, sizeof query - 1U, &command,
                                ebase_line_test_malloc));
    assert(live_bytes == 0U && command.payload == NULL && attempts == 1U);
    assert(!ebase_parse_command(operation_query, strlen(operation_query), &command,
                                ebase_line_test_malloc));
    assert(command.kind == EBASE_OTA_RESULT &&
           live_bytes == ESP_BASE_OTA_OPERATION_ID_BYTES &&
           strlen(command.operation_id) == 36U);
    ebase_command_release(&command);
    ebase_command_release(&command);
    assert(live_bytes == 0U);

    const char *const allocating_commands[] = {configuration, operation_query};
    for (size_t i = 0; i < sizeof allocating_commands / sizeof *allocating_commands; ++i) {
        reset_counters();
        fail_at = 1U;
        const char *error = ebase_parse_command(allocating_commands[i],
            strlen(allocating_commands[i]), &command, ebase_line_test_malloc);
        assert(error && !strcmp(error, "resource_failure") && attempts == 1U &&
               command.payload == NULL && command.payload_size_bytes == 0U &&
               strlen(command.request.request_id) == 36U && live_bytes == 0U);
        /* An allocator failure does not prevent the next read-only request. */
        assert(!ebase_parse_command(query, sizeof query - 1U, &command,
                                    ebase_line_test_malloc));
        assert(command.payload == NULL && attempts == 1U);
        ebase_command_release(&command);
    }

    reset_counters();
    assert(!ebase_parse_command(configuration, strlen(configuration), &command,
                                ebase_line_test_malloc));
    assert(ebase_parse_command("invalid", 7U, &command, ebase_line_test_malloc));
    assert(live_bytes == 0U && command.payload == NULL);
    ebase_command_release(&command);
    printf("command payload: query owns no write storage; header %zu bytes; secrets wiped\n",
           sizeof command);
}

int main(void)
{
    ebase_line_reader_t reader = {0};
    reset_counters();
    feed_query(&reader);
    assert(delivered == 1U && rejected == 0U && peak_bytes < 512U);
    const size_t short_peak = peak_bytes;

    char *large = malloc(EBASE_LINE_LIMIT + 2U);
    assert(large);
    memset(large, 's', EBASE_LINE_LIMIT + 2U);
    reset_counters();
    expected_size = EBASE_LINE_LIMIT;
    ebase_line_feed(&reader, large, EBASE_LINE_LIMIT, receive, NULL);
    assert(delivered == 0U && rejected == 0U && reader.length == EBASE_LINE_LIMIT);
    ebase_line_feed(&reader, "\n", 1U, receive, NULL);
    assert(delivered == 1U && rejected == 0U && live_bytes == 0U);
    const size_t growth_attempts = attempts;

    /* Every growth allocation can fail; one error is delivered only at LF,
     * and the following complete command still succeeds. */
    for (size_t fault = 1U; fault <= growth_attempts; ++fault) {
        reset_counters();
        fail_at = fault;
        ebase_line_feed(&reader, large, EBASE_LINE_LIMIT, receive, NULL);
        assert(reader.discard && reader.data == NULL && live_bytes == 0U);
        assert(delivered == 0U && rejected == 0U && attempts == fault);
        ebase_line_feed(&reader, large, 2U, receive, NULL);
        ebase_line_feed(&reader, "\n", 1U, receive, NULL);
        assert(delivered == 0U && rejected == 1U);
        fail_at = 0U;
        feed_query(&reader);
        assert(delivered == 1U && rejected == 1U);
    }

    reset_counters();
    ebase_line_feed(&reader, large, EBASE_LINE_LIMIT + 1U, receive, NULL);
    assert(reader.discard && live_bytes == 0U && rejected == 0U);
    ebase_line_feed(&reader, "\n", 1U, receive, NULL);
    feed_query(&reader);
    assert(rejected == 1U && delivered == 1U);

    reset_counters();
    ebase_line_feed(&reader, "private-input\0suffix", 20U, receive, NULL);
    assert(reader.discard && live_bytes == 0U);
    ebase_line_feed(&reader, "\n", 1U, receive, NULL);
    feed_query(&reader);
    assert(rejected == 1U && delivered == 1U);

    reset_counters();
    ebase_line_feed(&reader, "private-partial-input", 21U, receive, NULL);
    assert(reader.data && live_bytes > 0U && delivered == 0U && rejected == 0U);
    ebase_line_release(&reader);
    ebase_line_release(&reader);
    assert(live_bytes == 0U && reader.data == NULL);
    feed_query(&reader);
    ebase_line_feed(&reader, "\n\n", 2U, receive, NULL);
    assert(delivered == 1U && rejected == 0U && live_bytes == 0U);
    free(large);
    printf("line reader: short peak %zu bytes; %zu injected growth failures; wiped and released\n",
           short_peak, growth_attempts);
    command_payload_tests();
    return 0;
}

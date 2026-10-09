// SPDX-License-Identifier: Apache-2.0
/* Compose the exact locked OTA reader with the production upload callback.
 * Socketpair replaces transport only; unused SDK/Flash functions are stripped. */
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <time.h>
#include <unistd.h>
#include "psa/crypto.h"
psa_status_t psa_crypto_init(void);

static int measured_select(int width, fd_set *readers, fd_set *writers,
    fd_set *exceptions, struct timeval *timeout);
#define select measured_select
#ifdef EBASE_TEST_LISTENER_SOURCE
#include EBASE_TEST_LISTENER_SOURCE
#else
#include "../components/device_protocol/frp_management_listener.c"
#endif
#undef select
#include "src/update.c"
#include "src/http_deadline.c"

static int raw_count;
static int64_t raw_elapsed_us;
static unsigned timeout_reads;
static int64_t callback_started_us, callback_finished_us, select_finished_us;
static int64_t maximum_select_overrun_us;

static int measured_select(int width, fd_set *readers, fd_set *writers,
    fd_set *exceptions, struct timeval *timeout)
{
    const int64_t requested_us = (int64_t)timeout->tv_sec * 1000000 + timeout->tv_usec;
    const int64_t started_us = monotonic_us();
    const int count = select(width, readers, writers, exceptions, timeout);
    select_finished_us = monotonic_us();
    const int64_t overrun_us = select_finished_us - started_us - requested_us;
    if (overrun_us > maximum_select_overrun_us) maximum_select_overrun_us = overrun_us;
    return count;
}

int64_t esp_timer_get_time(void) { return monotonic_us(); }

static int observed_read(void *context, uint8_t *bytes, size_t capacity, uint32_t timeout_ms)
{
    const int64_t started_us = monotonic_us();
    callback_started_us = started_us;
    raw_count = esp_base_frp_management_upload_read(context, bytes, capacity, timeout_ms);
    callback_finished_us = monotonic_us();
    raw_elapsed_us = callback_finished_us - started_us;
    if (raw_count == EOTA_STREAM_READ_TIMEOUT) ++timeout_reads;
    return raw_count;
}

static void socket_upload(int descriptors[2], uint32_t length)
{
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, descriptors) == 0);
    assert(nonblocking(descriptors[0]) && nonblocking(descriptors[1]));
    s_upload = (firmware_upload_t){.active = true, .claimed = true,
        .fd = descriptors[0], .image_size_bytes = length};
    timeout_reads = 0;
}

static void close_upload(int descriptors[2])
{
    assert(s_upload.borrowers == 0);
    assert(close(descriptors[0]) == 0 && close(descriptors[1]) == 0);
    s_upload = (firmware_upload_t){.fd = -1};
}

static eota_stream_input_t reader(eota_policy_t *policy, eota_stream_t *stream,
    eota_http_deadline_t *deadline, uint32_t read_timeout_ms)
{
    *policy = (eota_policy_t){.read_timeout_ms = read_timeout_ms,
        .idle_timeout_ms = 30000, .total_timeout_ms = 300000};
    *stream = (eota_stream_t){.read = observed_read};
    assert(eota_http_deadline_init(deadline, policy->total_timeout_ms, policy->idle_timeout_ms));
    return (eota_stream_input_t){.policy = policy, .stream = stream, .deadline = deadline};
}

static void measure_timeouts(void)
{
    const uint32_t budgets_ms[] = {1, 5, 50, 1000};
    for (size_t i = 0; i < sizeof budgets_ms / sizeof budgets_ms[0]; ++i) {
        for (unsigned repeat = 0; repeat < 3; ++repeat) {
            int descriptors[2]; socket_upload(descriptors, 1);
            eota_policy_t policy; eota_stream_t stream; eota_http_deadline_t deadline;
            eota_stream_input_t input = reader(&policy, &stream, &deadline, budgets_ms[i]);
            uint8_t byte; const int64_t started_us = monotonic_us();
            const int count = read_stream_input(&input, &byte, 1);
            printf("TIMEOUT_MEASUREMENT budget_us=%u raw_count=%d raw_elapsed_us=%lld outer_count=%d outer_elapsed_us=%lld callback_entry_us=%lld callback_tail_us=%lld select_overrun_max_us=%lld\n",
                budgets_ms[i] * 1000, raw_count, (long long)raw_elapsed_us,
                count, (long long)(monotonic_us() - started_us),
                (long long)(callback_started_us - started_us),
                (long long)(callback_finished_us - select_finished_us),
                (long long)maximum_select_overrun_us);
            close_upload(descriptors);
        }
    }
}

static void wait_until(int64_t deadline_us)
{
    for (;;) {
        const int64_t left_us = deadline_us - monotonic_us();
        if (left_us <= 0) return;
        struct timespec pause = {.tv_sec = (time_t)(left_us / 1000000),
            .tv_nsec = (long)(left_us % 1000000) * 1000};
        if (nanosleep(&pause, NULL) < 0) assert(errno == EINTR);
    }
}

static void *delayed_body(void *argument)
{
    const int fd = *(const int *)argument;
    wait_until(monotonic_us() + INT64_C(2250000));
    assert(send(fd, "abc", 3, 0) == 3);
    return NULL;
}

static void retry_then_body(void)
{
    int descriptors[2]; socket_upload(descriptors, 3);
    eota_policy_t policy; eota_stream_t stream; eota_http_deadline_t deadline;
    eota_stream_input_t input = reader(&policy, &stream, &deadline, 1000);
    pthread_t sender; assert(pthread_create(&sender, NULL, delayed_body, &descriptors[1]) == 0);
    uint8_t bytes[3]; size_t received = 0; const int64_t started_us = monotonic_us();
    while (received < sizeof bytes) {
        const int count = read_stream_input(&input, bytes + received, sizeof bytes - received);
        if (count != EOTA_STREAM_READ_TIMEOUT || timeout_reads <= 2) {
            printf("BODY_READ raw_count=%d raw_elapsed_us=%lld outer_count=%d\n",
                raw_count, (long long)raw_elapsed_us, count); fflush(stdout);
        }
        if (count == EOTA_STREAM_READ_TIMEOUT) continue;
        assert(count > 0);
        received += (size_t)count;
    }
    assert(pthread_join(sender, NULL) == 0);
    assert(timeout_reads >= 2 && !memcmp(bytes, "abc", sizeof bytes));
    assert(monotonic_us() - started_us > 1000000 && monotonic_us() - started_us < 30000000);
    assert(finish_stream_input(&input));
    close_upload(descriptors);
    printf("RETRY_BODY PASS timeout_reads=%u exact_framing=1\n", timeout_reads);
}

static void reject_broken_inputs(void)
{
    int descriptors[2]; socket_upload(descriptors, 1);
    eota_policy_t policy; eota_stream_t stream; eota_http_deadline_t deadline;
    eota_stream_input_t input = reader(&policy, &stream, &deadline, 1000);
    assert(shutdown(descriptors[1], SHUT_WR) == 0);
    uint8_t byte;
    assert(read_stream_input(&input, &byte, 1) == -1);
    close_upload(descriptors);

    socket_upload(descriptors, 1); input = reader(&policy, &stream, &deadline, 1000);
    assert(send(descriptors[1], "ab", 2, 0) == 2);
    assert(read_stream_input(&input, &byte, 1) == 1 && byte == 'a');
    assert(!finish_stream_input(&input));
    close_upload(descriptors);
    puts("BROKEN_INPUT PASS premature_eof=1 trailing_byte=1");
}

static int late_read(void *context, uint8_t *bytes, size_t capacity, uint32_t timeout_ms)
{
    wait_until(monotonic_us() + (int64_t)timeout_ms * 1000);
    return observed_read(context, bytes, capacity, timeout_ms);
}

static void reject_late_data_and_eof(void)
{
    for (unsigned eof = 0; eof < 2; ++eof) {
        int descriptors[2]; socket_upload(descriptors, 1);
        eota_policy_t policy; eota_stream_t stream; eota_http_deadline_t deadline;
        eota_stream_input_t input = reader(&policy, &stream, &deadline, 1000);
        stream.read = late_read;
        if (eof) s_upload.received_bytes = 1;
        else assert(send(descriptors[1], "a", 1, 0) == 1);
        const int64_t last_progress_us = deadline.last_progress_us;
        uint8_t byte;
        assert(read_stream_input(&input, &byte, 1) == -1);
        assert(raw_count == (eof ? 0 : 1) && deadline.last_progress_us == last_progress_us);
        close_upload(descriptors);
    }
    puts("LATE_INPUT PASS data_rejected=1 eof_rejected=1 idle_not_refreshed=1");
}

static void reject_deadline(bool total)
{
    int descriptors[2]; socket_upload(descriptors, 1);
    eota_policy_t policy; eota_stream_t stream; eota_http_deadline_t deadline;
    eota_stream_input_t input = reader(&policy, &stream, &deadline, 1000);
    if (total) {
        /* Exercise the final real-clock 500 ms of the unchanged 300 s total
         * budget, even though idle has just been refreshed. No clock is faked. */
        deadline.started_us -= INT64_C(299500000);
        assert(eota_http_deadline_progress(&deadline));
    }
    const int64_t started_us = monotonic_us();
    const int64_t initial_budget_us = eota_http_deadline_remaining_us_at(&deadline, started_us);
    const int64_t last_progress_us = deadline.last_progress_us;
    uint8_t byte; int count;
    do { count = read_stream_input(&input, &byte, 1); }
    while (count == EOTA_STREAM_READ_TIMEOUT);
    assert(count == -1 && eota_http_deadline_remaining_us(&deadline) < 1000);
    assert(deadline.total_timeout_ms == 300000 && deadline.idle_timeout_ms == 30000 &&
        deadline.last_progress_us == last_progress_us && timeout_reads >= 2);
    const int64_t elapsed_us = monotonic_us() - started_us;
    assert(elapsed_us >= initial_budget_us - 1000);
    printf("DEADLINE PASS kind=%s unchanged_ms=%u elapsed_us=%lld timeout_reads=%u\n",
        total ? "total" : "idle", total ? 300000 : 30000,
        (long long)elapsed_us, timeout_reads);
    close_upload(descriptors);
}

int main(int argc, char **argv)
{
    if (argc == 2 && !strcmp(argv[1], "measure")) { measure_timeouts(); return 0; }
    assert(argc == 1);
    retry_then_body();
    reject_broken_inputs();
    reject_late_data_and_eof();
    reject_deadline(false);
    reject_deadline(true);
    return 0;
}

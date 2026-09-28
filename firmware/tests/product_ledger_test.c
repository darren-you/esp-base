// SPDX-License-Identifier: Apache-2.0
#include "esp_base_product_ledger.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

typedef struct {
    uint8_t bytes[EBASE_PRODUCT_LEDGER_BYTES];
    bool present;
    bool fail_write;
    bool busy_write;
    bool busy_read;
    bool corrupt_readback;
} memory_store_t;

static ebase_ledger_io_result_t read_bytes(
    void *context, uint8_t bytes[EBASE_PRODUCT_LEDGER_BYTES])
{
    memory_store_t *store = context;
    if (store->busy_read) return EBASE_LEDGER_IO_BUSY;
    if (!store->present) return EBASE_LEDGER_IO_NOT_FOUND;
    memcpy(bytes, store->bytes, EBASE_PRODUCT_LEDGER_BYTES);
    if (store->corrupt_readback) bytes[10] ^= 1U;
    return EBASE_LEDGER_IO_OK;
}

static ebase_ledger_io_result_t write_bytes(
    void *context, const uint8_t bytes[EBASE_PRODUCT_LEDGER_BYTES])
{
    memory_store_t *store = context;
    if (store->busy_write) return EBASE_LEDGER_IO_BUSY;
    if (store->fail_write) return EBASE_LEDGER_IO_ERROR;
    memcpy(store->bytes, bytes, EBASE_PRODUCT_LEDGER_BYTES);
    store->present = true;
    return EBASE_LEDGER_IO_OK;
}

static ebase_product_record_t intent(uint32_t sequence)
{
    ebase_product_record_t record = {0};
    record.sequence = sequence;
    snprintf(record.operation_id, sizeof record.operation_id,
             "00000000-0000-4000-8000-%012x", sequence);
    memset(record.fingerprint, (int)sequence, sizeof record.fingerprint);
    memset(record.package_sha256, 0xab, sizeof record.package_sha256);
    record.container_sequence = sequence + 4U;
    record.kind = EBASE_PRODUCT_INSTALL;
    record.state = EBASE_PRODUCT_PREPARED;
    return record;
}

int main(void)
{
    memory_store_t store = {0};
    ebase_product_ledger_io_t io = {
        .read = read_bytes, .write = write_bytes, .context = &store,
    };
    ebase_product_ledger_t ledger;
    assert(ebase_product_ledger_open(&ledger, &io) == EBASE_LEDGER_UNINITIALIZED);
    assert(ledger.high_watermark == 0 && ledger.count == 0);

    ebase_product_record_t first = intent(1);
    assert(ebase_product_ledger_begin(&ledger, &io, &first) == EBASE_LEDGER_UNINITIALIZED);
    /* The controller may call this only after independently proving an exact
     * pristine Container binding; the codec itself cannot establish that. */
    assert(ebase_product_ledger_initialize_empty(&ledger, &io) == EBASE_LEDGER_OK);
    assert(ledger.initialized);
    assert(ebase_product_ledger_initialize_empty(&ledger, &io) == EBASE_LEDGER_CONFLICT);
    assert(ebase_product_ledger_begin(&ledger, &io, &first) == EBASE_LEDGER_OK);
    assert(ledger.high_watermark == 1 && ledger.count == 1);
    assert(ebase_product_ledger_begin(&ledger, &io, &first) == EBASE_LEDGER_DUPLICATE);
    ebase_product_record_t altered = first;
    altered.fingerprint[0] ^= 1U;
    assert(ebase_product_ledger_begin(&ledger, &io, &altered) == EBASE_LEDGER_CONFLICT);
    ebase_product_record_t second = intent(2);
    assert(ebase_product_ledger_begin(&ledger, &io, &second) == EBASE_LEDGER_PENDING);

    /* A cold boot sees the original intent and will not silently replay it. */
    ebase_product_ledger_t rebooted;
    assert(ebase_product_ledger_open(&rebooted, &io) == EBASE_LEDGER_OK);
    ebase_product_record_t observed;
    assert(ebase_product_ledger_query(&rebooted, first.operation_id, &observed) == EBASE_LEDGER_OK);
    assert(observed.state == EBASE_PRODUCT_PREPARED);
    assert(ebase_product_ledger_begin(&rebooted, &io, &second) == EBASE_LEDGER_PENDING);
    assert(ebase_product_ledger_finish(&rebooted, &io, 1, first.operation_id,
           first.fingerprint, EBASE_PRODUCT_SUCCEEDED, 0, 6) == EBASE_LEDGER_OK);
    assert(ebase_product_ledger_finish(&rebooted, &io, 1, first.operation_id,
           first.fingerprint, EBASE_PRODUCT_SUCCEEDED, 0, 6) == EBASE_LEDGER_DUPLICATE);
    second.sequence = 3;
    assert(ebase_product_ledger_begin(&rebooted, &io, &second) == EBASE_LEDGER_SEQUENCE_GAP);
    second = intent(2);
    assert(ebase_product_ledger_begin(&rebooted, &io, &second) == EBASE_LEDGER_OK);
    assert(ebase_product_ledger_finish(&rebooted, &io, 2, second.operation_id,
           second.fingerprint, EBASE_PRODUCT_FAILED, 1, second.container_sequence) == EBASE_LEDGER_OK);

    for (uint32_t sequence = 3; sequence <= 10; ++sequence) {
        ebase_product_record_t next = intent(sequence);
        assert(ebase_product_ledger_begin(&rebooted, &io, &next) == EBASE_LEDGER_OK);
        assert(ebase_product_ledger_finish(&rebooted, &io, sequence, next.operation_id,
               next.fingerprint, EBASE_PRODUCT_SUCCEEDED, 0, sequence + 5U) == EBASE_LEDGER_OK);
    }
    assert(rebooted.count == EBASE_PRODUCT_LEDGER_SLOTS && rebooted.high_watermark == 10);
    assert(ebase_product_ledger_query(&rebooted, first.operation_id, &observed) == EBASE_LEDGER_UNKNOWN);
    assert(ebase_product_ledger_query(&rebooted, second.operation_id, &observed) == EBASE_LEDGER_UNKNOWN);
    assert(ebase_product_ledger_begin(&rebooted, &io, &first) == EBASE_LEDGER_STALE_SEQUENCE);
    assert(ebase_product_ledger_open(&ledger, &io) == EBASE_LEDGER_OK);
    assert(ledger.high_watermark == 10 && ledger.count == 8);

    /* A vanished key must never silently reset the anti-replay sequence. */
    store.present = false;
    assert(ebase_product_ledger_open(&ledger, &io) == EBASE_LEDGER_UNINITIALIZED);
    assert(ebase_product_ledger_begin(&ledger, &io, &first) == EBASE_LEDGER_UNINITIALIZED);
    store.present = true;
    assert(ebase_product_ledger_open(&ledger, &io) == EBASE_LEDGER_OK);
    assert(ledger.high_watermark == 10);

    ebase_product_record_t eleventh = intent(11);
    store.busy_write = true;
    assert(ebase_product_ledger_begin(&ledger, &io, &eleventh) == EBASE_LEDGER_BUSY);
    assert(!ledger.uncertain && ledger.high_watermark == 10);
    store.busy_write = false;
    store.busy_read = true;
    assert(ebase_product_ledger_open(&ledger, &io) == EBASE_LEDGER_BUSY);
    store.busy_read = false;
    assert(ebase_product_ledger_open(&ledger, &io) == EBASE_LEDGER_OK);
    store.fail_write = true;
    assert(ebase_product_ledger_begin(&ledger, &io, &eleventh) == EBASE_LEDGER_UNCERTAIN);
    assert(ledger.uncertain);
    assert(ebase_product_ledger_begin(&ledger, &io, &eleventh) == EBASE_LEDGER_UNCERTAIN);
    store.fail_write = false;
    assert(ebase_product_ledger_open(&ledger, &io) == EBASE_LEDGER_OK);
    assert(ledger.high_watermark == 10);
    store.corrupt_readback = true;
    assert(ebase_product_ledger_begin(&ledger, &io, &eleventh) == EBASE_LEDGER_UNCERTAIN);
    store.corrupt_readback = false;
    assert(ebase_product_ledger_open(&ledger, &io) == EBASE_LEDGER_OK);
    assert(ledger.high_watermark == 11);
    assert(ebase_product_ledger_query(&ledger, eleventh.operation_id, &observed) == EBASE_LEDGER_OK);
    assert(observed.state == EBASE_PRODUCT_PREPARED);
    store.bytes[20] ^= 1U;
    assert(ebase_product_ledger_open(&ledger, &io) == EBASE_LEDGER_UNCERTAIN);
    assert(ledger.uncertain);
    return 0;
}

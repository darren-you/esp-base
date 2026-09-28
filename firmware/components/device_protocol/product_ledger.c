// SPDX-License-Identifier: Apache-2.0
#include "esp_base_product_ledger.h"

#include <limits.h>
#include <stdatomic.h>
#include <string.h>

#define HEADER_BYTES 10U
#define RECORD_BYTES 112U
#define CRC_OFFSET (HEADER_BYTES + EBASE_PRODUCT_LEDGER_SLOTS * RECORD_BYTES)
_Static_assert(CRC_OFFSET + 4U == EBASE_PRODUCT_LEDGER_BYTES, "ledger size");
/* Keep the two 910-byte NVS images off the control task stack.
 * Product access is single-owner; this guard makes accidental overlap fail
 * before any write rather than racing over shared codec scratch. */
static atomic_flag s_scratch_busy = ATOMIC_FLAG_INIT;
static uint8_t s_scratch[2][EBASE_PRODUCT_LEDGER_BYTES];

static bool scratch_acquire(void)
{
    return !atomic_flag_test_and_set_explicit(&s_scratch_busy, memory_order_acquire);
}

static void scratch_release(void)
{
    atomic_flag_clear_explicit(&s_scratch_busy, memory_order_release);
}

static void put_u32(uint8_t *out, uint32_t value)
{
    for (unsigned i = 0; i < 4; ++i) out[i] = (uint8_t)(value >> (8U * i));
}

static uint32_t get_u32(const uint8_t *in)
{
    uint32_t value = 0;
    for (unsigned i = 0; i < 4; ++i) value |= (uint32_t)in[i] << (8U * i);
    return value;
}

static uint32_t crc32(const uint8_t *bytes, size_t length)
{
    uint32_t crc = UINT32_MAX;
    for (size_t i = 0; i < length; ++i) {
        crc ^= bytes[i];
        for (unsigned bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ (0xedb88320U & (0U - (crc & 1U)));
    }
    return ~crc;
}

static bool uuid(const char id[EBASE_PRODUCT_ID_BYTES])
{
    if (!id) return false;
    for (size_t i = 0; i < 36; ++i) {
        const char c = id[i];
        if (i == 8 || i == 13 || i == 18 || i == 23) {
            if (c != '-') return false;
        } else if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
    }
    return id[36] == '\0' && id[14] == '4' && strchr("89ab", id[19]) != NULL;
}

static bool nonzero(const uint8_t *bytes, size_t length)
{
    uint8_t any = 0;
    for (size_t i = 0; i < length; ++i) any |= bytes[i];
    return any != 0;
}

static bool record_valid(const ebase_product_record_t *record)
{
    return record->sequence != 0U && record->container_sequence != 0U &&
        uuid(record->operation_id) &&
        nonzero(record->fingerprint, sizeof record->fingerprint) &&
        record->kind >= EBASE_PRODUCT_INSTALL && record->kind <= EBASE_PRODUCT_UNINSTALL &&
        record->state >= EBASE_PRODUCT_PREPARED && record->state <= EBASE_PRODUCT_FAILED &&
        (record->state == EBASE_PRODUCT_FAILED ? record->result_code != 0U : record->result_code == 0U) &&
        nonzero(record->package_sha256, sizeof record->package_sha256);
}

static void encode(const ebase_product_ledger_t *ledger,
                   uint8_t bytes[EBASE_PRODUCT_LEDGER_BYTES])
{
    memset(bytes, 0, EBASE_PRODUCT_LEDGER_BYTES);
    memcpy(bytes, "EPRD", 4);
    bytes[4] = 1;
    bytes[5] = ledger->count;
    put_u32(bytes + 6, ledger->high_watermark);
    for (size_t i = 0; i < ledger->count; ++i) {
        const ebase_product_record_t *record = &ledger->records[i];
        uint8_t *out = bytes + HEADER_BYTES + i * RECORD_BYTES;
        put_u32(out, record->sequence);
        memcpy(out + 4, record->operation_id, EBASE_PRODUCT_ID_BYTES);
        memcpy(out + 41, record->fingerprint, 32);
        out[73] = (uint8_t)record->kind;
        out[74] = (uint8_t)record->state;
        out[75] = record->result_code;
        memcpy(out + 76, record->package_sha256, 32);
        put_u32(out + 108, record->container_sequence);
    }
    put_u32(bytes + CRC_OFFSET, crc32(bytes, CRC_OFFSET));
}

static bool decode(const uint8_t bytes[EBASE_PRODUCT_LEDGER_BYTES],
                   ebase_product_ledger_t *ledger)
{
    if (memcmp(bytes, "EPRD", 4) || bytes[4] != 1 ||
        bytes[5] > EBASE_PRODUCT_LEDGER_SLOTS ||
        get_u32(bytes + CRC_OFFSET) != crc32(bytes, CRC_OFFSET)) return false;
    memset(ledger, 0, sizeof *ledger);
    ledger->count = bytes[5];
    ledger->high_watermark = get_u32(bytes + 6);
    if ((ledger->count == 0) != (ledger->high_watermark == 0) ||
        ledger->high_watermark < ledger->count) return false;
    for (size_t i = 0; i < EBASE_PRODUCT_LEDGER_SLOTS; ++i) {
        const uint8_t *in = bytes + HEADER_BYTES + i * RECORD_BYTES;
        if (i >= ledger->count) {
            if (nonzero(in, RECORD_BYTES)) return false;
            continue;
        }
        ebase_product_record_t *record = &ledger->records[i];
        record->sequence = get_u32(in);
        memcpy(record->operation_id, in + 4, EBASE_PRODUCT_ID_BYTES);
        memcpy(record->fingerprint, in + 41, 32);
        record->kind = (ebase_product_kind_t)in[73];
        record->state = (ebase_product_state_t)in[74];
        record->result_code = in[75];
        memcpy(record->package_sha256, in + 76, 32);
        record->container_sequence = get_u32(in + 108);
        if (!record_valid(record) ||
            record->sequence != ledger->high_watermark - ledger->count + 1U + i ||
            (i + 1U < ledger->count && record->state == EBASE_PRODUCT_PREPARED)) return false;
        for (size_t j = 0; j < i; ++j)
            if (!strcmp(record->operation_id, ledger->records[j].operation_id)) return false;
    }
    return true;
}

static ebase_product_ledger_result_t persist(
    ebase_product_ledger_t *ledger, const ebase_product_ledger_io_t *io,
    const ebase_product_ledger_t *candidate)
{
    if (!scratch_acquire()) return EBASE_LEDGER_BUSY;
    encode(candidate, s_scratch[0]);
    const ebase_ledger_io_result_t written = io->write(io->context, s_scratch[0]);
    const bool verified = written == EBASE_LEDGER_IO_OK &&
        io->read(io->context, s_scratch[1]) == EBASE_LEDGER_IO_OK &&
        memcmp(s_scratch[0], s_scratch[1], EBASE_PRODUCT_LEDGER_BYTES) == 0;
    scratch_release();
    if (written == EBASE_LEDGER_IO_BUSY) return EBASE_LEDGER_BUSY;
    if (!verified) {
        ledger->uncertain = true;
        return EBASE_LEDGER_UNCERTAIN;
    }
    *ledger = *candidate;
    return EBASE_LEDGER_OK;
}

ebase_product_ledger_result_t ebase_product_ledger_open(
    ebase_product_ledger_t *ledger, const ebase_product_ledger_io_t *io)
{
    if (!ledger || !io || !io->read || !io->write) return EBASE_LEDGER_INVALID;
    memset(ledger, 0, sizeof *ledger);
    if (!scratch_acquire()) return EBASE_LEDGER_BUSY;
    const ebase_ledger_io_result_t result = io->read(io->context, s_scratch[0]);
    const bool valid = result != EBASE_LEDGER_IO_OK || decode(s_scratch[0], ledger);
    scratch_release();
    if (result == EBASE_LEDGER_IO_NOT_FOUND) return EBASE_LEDGER_UNINITIALIZED;
    if (result == EBASE_LEDGER_IO_BUSY) return EBASE_LEDGER_BUSY;
    if (result != EBASE_LEDGER_IO_OK || !valid) {
        ledger->uncertain = true;
        return EBASE_LEDGER_UNCERTAIN;
    }
    ledger->initialized = true;
    return EBASE_LEDGER_OK;
}

ebase_product_ledger_result_t ebase_product_ledger_initialize_empty(
    ebase_product_ledger_t *ledger, const ebase_product_ledger_io_t *io)
{
    if (!ledger || !io || !io->read || !io->write) return EBASE_LEDGER_INVALID;
    if (ledger->uncertain) return EBASE_LEDGER_UNCERTAIN;
    if (ledger->initialized || ledger->high_watermark || ledger->count)
        return EBASE_LEDGER_CONFLICT;
    /* A second read prevents initialization if another owner created the key
     * after open. The caller must separately prove a pristine ECS2 baseline. */
    if (!scratch_acquire()) return EBASE_LEDGER_BUSY;
    const ebase_ledger_io_result_t observed = io->read(io->context, s_scratch[0]);
    scratch_release();
    if (observed == EBASE_LEDGER_IO_BUSY) return EBASE_LEDGER_BUSY;
    if (observed == EBASE_LEDGER_IO_OK) return EBASE_LEDGER_CONFLICT;
    if (observed != EBASE_LEDGER_IO_NOT_FOUND) {
        ledger->uncertain = true;
        return EBASE_LEDGER_UNCERTAIN;
    }
    ebase_product_ledger_t candidate = {.initialized = true};
    return persist(ledger, io, &candidate);
}

ebase_product_ledger_result_t ebase_product_ledger_query(
    const ebase_product_ledger_t *ledger, const char *operation_id,
    ebase_product_record_t *record)
{
    if (!ledger || !operation_id || !record || !uuid(operation_id)) return EBASE_LEDGER_INVALID;
    if (ledger->uncertain) return EBASE_LEDGER_UNCERTAIN;
    if (!ledger->initialized) return EBASE_LEDGER_UNKNOWN;
    for (size_t i = 0; i < ledger->count; ++i) {
        if (!strcmp(ledger->records[i].operation_id, operation_id)) {
            *record = ledger->records[i];
            return EBASE_LEDGER_OK;
        }
    }
    return EBASE_LEDGER_UNKNOWN;
}

ebase_product_ledger_result_t ebase_product_ledger_begin(
    ebase_product_ledger_t *ledger, const ebase_product_ledger_io_t *io,
    const ebase_product_record_t *intent)
{
    if (!ledger || !io || !io->read || !io->write || !intent ||
        !record_valid(intent) || intent->state != EBASE_PRODUCT_PREPARED)
        return EBASE_LEDGER_INVALID;
    if (ledger->uncertain) return EBASE_LEDGER_UNCERTAIN;
    if (!ledger->initialized) return EBASE_LEDGER_UNINITIALIZED;
    for (size_t i = 0; i < ledger->count; ++i) {
        if (!strcmp(ledger->records[i].operation_id, intent->operation_id))
            return ledger->records[i].sequence == intent->sequence &&
                ledger->records[i].kind == intent->kind &&
                !memcmp(ledger->records[i].fingerprint, intent->fingerprint, 32) &&
                !memcmp(ledger->records[i].package_sha256, intent->package_sha256, 32) ?
                EBASE_LEDGER_DUPLICATE : EBASE_LEDGER_CONFLICT;
    }
    if (intent->sequence <= ledger->high_watermark) return EBASE_LEDGER_STALE_SEQUENCE;
    if (ledger->high_watermark == UINT32_MAX) return EBASE_LEDGER_EXHAUSTED;
    if (intent->sequence != ledger->high_watermark + 1U) return EBASE_LEDGER_SEQUENCE_GAP;
    if (ledger->count && ledger->records[ledger->count - 1U].state == EBASE_PRODUCT_PREPARED)
        return EBASE_LEDGER_PENDING;
    ebase_product_ledger_t candidate = *ledger;
    if (candidate.count == EBASE_PRODUCT_LEDGER_SLOTS) {
        memmove(candidate.records, candidate.records + 1,
                (EBASE_PRODUCT_LEDGER_SLOTS - 1U) * sizeof candidate.records[0]);
        --candidate.count;
    }
    candidate.records[candidate.count++] = *intent;
    candidate.high_watermark = intent->sequence;
    return persist(ledger, io, &candidate);
}

ebase_product_ledger_result_t ebase_product_ledger_finish(
    ebase_product_ledger_t *ledger, const ebase_product_ledger_io_t *io,
    uint32_t sequence, const char *operation_id, const uint8_t fingerprint[32],
    ebase_product_state_t state, uint8_t result_code,
    uint32_t container_sequence)
{
    if (!ledger || !io || !io->read || !io->write || !operation_id || !uuid(operation_id) ||
        !fingerprint || (state != EBASE_PRODUCT_SUCCEEDED && state != EBASE_PRODUCT_FAILED) ||
        (state == EBASE_PRODUCT_FAILED ? result_code == 0U : result_code != 0U))
        return EBASE_LEDGER_INVALID;
    if (ledger->uncertain) return EBASE_LEDGER_UNCERTAIN;
    if (!ledger->initialized) return EBASE_LEDGER_UNINITIALIZED;
    if (!ledger->count) return EBASE_LEDGER_UNKNOWN;
    ebase_product_record_t *last = &ledger->records[ledger->count - 1U];
    if (last->sequence != sequence || strcmp(last->operation_id, operation_id) ||
        memcmp(last->fingerprint, fingerprint, 32)) return EBASE_LEDGER_CONFLICT;
    if (last->state != EBASE_PRODUCT_PREPARED)
        return last->state == state && last->result_code == result_code &&
            last->container_sequence == container_sequence ?
            EBASE_LEDGER_DUPLICATE : EBASE_LEDGER_CONFLICT;
    ebase_product_ledger_t candidate = *ledger;
    last = &candidate.records[candidate.count - 1U];
    last->state = state;
    last->result_code = result_code;
    last->container_sequence = container_sequence;
    return persist(ledger, io, &candidate);
}

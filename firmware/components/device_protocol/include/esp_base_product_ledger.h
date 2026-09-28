// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define EBASE_PRODUCT_LEDGER_SLOTS 8U
#define EBASE_PRODUCT_LEDGER_BYTES 910U
#define EBASE_PRODUCT_ID_BYTES 37U

typedef enum {
    EBASE_PRODUCT_INSTALL = 1,
    EBASE_PRODUCT_UPGRADE = 2,
    EBASE_PRODUCT_UNINSTALL = 3,
} ebase_product_kind_t;

typedef enum {
    EBASE_PRODUCT_PREPARED = 1,
    EBASE_PRODUCT_SUCCEEDED = 2,
    EBASE_PRODUCT_FAILED = 3,
} ebase_product_state_t;

typedef struct {
    uint32_t sequence;
    char operation_id[EBASE_PRODUCT_ID_BYTES];
    uint8_t fingerprint[32];
    uint8_t package_sha256[32];
    uint32_t container_sequence;
    ebase_product_kind_t kind;
    ebase_product_state_t state;
    uint8_t result_code;
} ebase_product_record_t;

typedef struct {
    uint32_t high_watermark;
    uint8_t count;
    bool initialized;
    bool uncertain;
    ebase_product_record_t records[EBASE_PRODUCT_LEDGER_SLOTS];
} ebase_product_ledger_t;

typedef enum {
    EBASE_LEDGER_IO_OK,
    EBASE_LEDGER_IO_NOT_FOUND,
    EBASE_LEDGER_IO_BUSY,
    EBASE_LEDGER_IO_ERROR,
} ebase_ledger_io_result_t;

/* write must durably commit the entire blob. The ledger independently reads
 * back exact bytes before it authorizes any product mutation. */
typedef struct {
    ebase_ledger_io_result_t (*read)(void *context, uint8_t bytes[EBASE_PRODUCT_LEDGER_BYTES]);
    ebase_ledger_io_result_t (*write)(
        void *context, const uint8_t bytes[EBASE_PRODUCT_LEDGER_BYTES]);
    void *context;
} ebase_product_ledger_io_t;

typedef enum {
    EBASE_LEDGER_OK,
    EBASE_LEDGER_UNINITIALIZED,
    EBASE_LEDGER_UNKNOWN,
    EBASE_LEDGER_DUPLICATE,
    EBASE_LEDGER_CONFLICT,
    EBASE_LEDGER_STALE_SEQUENCE,
    EBASE_LEDGER_SEQUENCE_GAP,
    EBASE_LEDGER_PENDING,
    EBASE_LEDGER_BUSY,
    EBASE_LEDGER_EXHAUSTED,
    EBASE_LEDGER_UNCERTAIN,
    EBASE_LEDGER_INVALID,
} ebase_product_ledger_result_t;

/* All access to one ledger is serialized by Base's product transaction owner.
 * A separate bounded scratch guard also prevents simultaneous codec use. */
ebase_product_ledger_result_t ebase_product_ledger_open(
    ebase_product_ledger_t *ledger, const ebase_product_ledger_io_t *io);
/* Only the product controller may initialize a missing key, after separately
 * proving that the persisted Container binding has never had a product
 * operation. A missing key after an operation is lost evidence, not reset. */
ebase_product_ledger_result_t ebase_product_ledger_initialize_empty(
    ebase_product_ledger_t *ledger, const ebase_product_ledger_io_t *io);
ebase_product_ledger_result_t ebase_product_ledger_query(
    const ebase_product_ledger_t *ledger, const char *operation_id,
    ebase_product_record_t *record);
/* Only the next monotonic sequence is accepted. An unresolved PREPARED record
 * blocks later operations, including after reboot. No old ID is re-executed. */
ebase_product_ledger_result_t ebase_product_ledger_begin(
    ebase_product_ledger_t *ledger, const ebase_product_ledger_io_t *io,
    const ebase_product_record_t *intent);
/* Caller must have independently proved the physical result. A failed result
 * is terminal only after safe cleanup; uncertain work remains PREPARED. */
ebase_product_ledger_result_t ebase_product_ledger_finish(
    ebase_product_ledger_t *ledger, const ebase_product_ledger_io_t *io,
    uint32_t sequence, const char *operation_id, const uint8_t fingerprint[32],
    ebase_product_state_t state, uint8_t result_code,
    uint32_t container_sequence);

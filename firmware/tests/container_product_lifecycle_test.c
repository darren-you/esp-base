// SPDX-License-Identifier: Apache-2.0
/* Run with Container's slot_runtime_test.py signed counter package generator.
 * Host substitutes supply Flash/NVS, scheduler, and firmware digest observation;
 * slot persistence, product signature validation, WAMR, and Base are production
 * code. This test does not attest a signed ESP-IDF firmware image. */
#include <assert.h>
#include <errno.h>
#include <openssl/sha.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

bool test_policy_enabled = true;
#include "esp_base_container_product.c"

enum { FLASH_BASE = 0x10000, SLOT_BYTES = 32768, FLASH_BYTES = 3 * SLOT_BYTES };

typedef struct {
    uint8_t flash[FLASH_BYTES];
    uint8_t blob[ECONTAINER_SLOT_BLOB_BYTES];
    bool present;
    bool locked;
    pthread_mutex_t mutex;
    uint8_t *mapping;
    unsigned blob_writes;
    unsigned fail_read_after_next_write;
    unsigned read_fail_countdown;
    unsigned flash_erases;
    unsigned flash_writes;
} store_t;

typedef struct { uint8_t *bytes; size_t size; } file_t;

static store_t store;
static const econtainer_slots_geometry_t geometry = {
    .partition_offset_bytes = FLASH_BASE, .partition_size_bytes = FLASH_BYTES,
    .erase_unit_bytes = 4096, .write_unit_bytes = 4,
    .slots = {{FLASH_BASE, SLOT_BYTES}, {FLASH_BASE + SLOT_BYTES, SLOT_BYTES},
              {FLASH_BASE + 2 * SLOT_BYTES, SLOT_BYTES}},
};
static esp_base_ota_firmware_set_t physical;

static bool in_range(uint32_t offset, size_t size)
{
    return size > 0 && offset >= FLASH_BASE &&
        (uint64_t)offset + size <= FLASH_BASE + FLASH_BYTES;
}

static bool lock_store(void *context)
{
    store_t *device = context;
    if (pthread_mutex_trylock(&device->mutex) != 0) return false;
    assert(!device->locked);
    device->locked = true;
    return true;
}

static void unlock_store(void *context)
{
    store_t *device = context;
    assert(device->locked && device->mapping == NULL);
    device->locked = false;
    assert(pthread_mutex_unlock(&device->mutex) == 0);
}

static econtainer_slot_blob_result_t read_blob(void *context, uint8_t *blob)
{
    store_t *device = context;
    assert(device->locked);
    if (device->read_fail_countdown != 0U &&
        --device->read_fail_countdown == 0U) {
        return ECONTAINER_SLOT_BLOB_READ_FAILED;
    }
    if (!device->present) return ECONTAINER_SLOT_BLOB_NOT_FOUND;
    memcpy(blob, device->blob, sizeof device->blob);
    return ECONTAINER_SLOT_BLOB_FOUND;
}

static bool write_blob(void *context, const uint8_t *blob)
{
    store_t *device = context;
    assert(device->locked && device->mapping == NULL);
    memcpy(device->blob, blob, sizeof device->blob);
    device->present = true;
    ++device->blob_writes;
    if (device->fail_read_after_next_write != 0U) {
        device->read_fail_countdown = device->fail_read_after_next_write;
        device->fail_read_after_next_write = 0U;
    }
    return true;
}

static bool read_flash(void *context, uint32_t offset, uint8_t *bytes, size_t size)
{
    store_t *device = context;
    assert(device->locked && in_range(offset, size));
    memcpy(bytes, device->flash + (offset - FLASH_BASE), size);
    return true;
}

static bool erase_flash(void *context, uint32_t offset, uint32_t size)
{
    store_t *device = context;
    assert(device->locked && device->mapping == NULL && in_range(offset, size));
    memset(device->flash + (offset - FLASH_BASE), 0xff, size);
    ++device->flash_erases;
    return true;
}

static bool write_flash(void *context, uint32_t offset, const uint8_t *bytes, size_t size)
{
    store_t *device = context;
    assert(device->locked && device->mapping == NULL && in_range(offset, size));
    for (size_t index = 0; index < size; ++index) {
        uint8_t *destination = device->flash + (offset - FLASH_BASE) + index;
        assert((*destination & bytes[index]) == bytes[index]);
        *destination &= bytes[index];
    }
    ++device->flash_writes;
    return true;
}

static bool map_flash(void *context, uint32_t offset, size_t size,
                      const uint8_t **mapped, uintptr_t *handle)
{
    store_t *device = context;
    assert(device->locked && device->mapping == NULL && in_range(offset, size));
    device->mapping = malloc(size);
    if (device->mapping == NULL) return false;
    memcpy(device->mapping, device->flash + (offset - FLASH_BASE), size);
    *mapped = device->mapping;
    *handle = 0;
    return true;
}

static void unmap_flash(void *context, uintptr_t handle)
{
    store_t *device = context;
    assert(device->locked && handle == 0 && device->mapping != NULL);
    free(device->mapping);
    device->mapping = NULL;
}

static const econtainer_slots_io_t io = {
    .lock = lock_store, .unlock = unlock_store, .read_blob = read_blob,
    .write_blob = write_blob, .flash_read = read_flash, .flash_erase = erase_flash,
    .flash_write = write_flash, .flash_map = map_flash, .flash_unmap = unmap_flash,
    .context = &store,
};

static file_t read_file(const char *directory, const char *name)
{
    char path[1024];
    const int length = snprintf(path, sizeof path, "%s/%s", directory, name);
    assert(length > 0 && (size_t)length < sizeof path);
    FILE *input = fopen(path, "rb");
    assert(input != NULL && fseek(input, 0, SEEK_END) == 0);
    const long size = ftell(input);
    assert(size > 0 && fseek(input, 0, SEEK_SET) == 0);
    file_t file = {malloc((size_t)size), (size_t)size};
    assert(file.bytes != NULL && fread(file.bytes, 1, file.size, input) == file.size);
    assert(fclose(input) == 0);
    return file;
}

static bool read_source(void *context, size_t offset, uint8_t *bytes, size_t size)
{
    const file_t *file = context;
    if (offset > file->size || size > file->size - offset) return false;
    memcpy(bytes, file->bytes + offset, size);
    return true;
}

esp_base_ota_firmware_result_t esp_base_ota_observe_firmware_set(
    esp_base_ota_firmware_observation_t observation, const eota_prepared_t *prepared,
    esp_base_ota_firmware_set_t *firmware_set)
{
    assert(observation == ESP_BASE_OTA_FIRMWARE_CONFIRMED && prepared == NULL);
    *firmware_set = physical;
    return ESP_BASE_OTA_FIRMWARE_OK;
}

/* The IDF provider is bypassed only because host Flash/NVS are in memory. */
bool econtainer_slots_idf_bind(econtainer_slots_idf_provider_t *provider,
                               const econtainer_slots_idf_config_t *config)
{
    (void)provider;
    (void)config;
    assert(!"host provider was not preset");
    return false;
}

esp_err_t nvs_flash_init_partition(const char *label)
{
    (void)label;
    assert(!"host provider was not preset");
    return ESP_FAIL;
}

typedef struct {
    pthread_mutex_t mutex;
    pthread_cond_t condition;
    bool available;
} binary_sem_t;
static bool timeout_stop_once;

static SemaphoreHandle_t create_sem(bool available)
{
    binary_sem_t *sem = calloc(1, sizeof *sem);
    assert(sem != NULL && pthread_mutex_init(&sem->mutex, NULL) == 0);
    assert(pthread_cond_init(&sem->condition, NULL) == 0);
    sem->available = available;
    return sem;
}

SemaphoreHandle_t xSemaphoreCreateMutex(void) { return create_sem(true); }
SemaphoreHandle_t xSemaphoreCreateBinary(void) { return create_sem(false); }

BaseType_t xSemaphoreGive(SemaphoreHandle_t handle)
{
    binary_sem_t *sem = handle;
    assert(pthread_mutex_lock(&sem->mutex) == 0);
    assert(!sem->available);
    sem->available = true;
    assert(pthread_cond_signal(&sem->condition) == 0);
    assert(pthread_mutex_unlock(&sem->mutex) == 0);
    return pdTRUE;
}

BaseType_t xSemaphoreTake(SemaphoreHandle_t handle, TickType_t ticks)
{
    if (handle == s_product.stopped && ticks != portMAX_DELAY && timeout_stop_once) {
        timeout_stop_once = false;
        return pdFALSE;
    }
    binary_sem_t *sem = handle;
    assert(pthread_mutex_lock(&sem->mutex) == 0);
    if (ticks == portMAX_DELAY) {
        while (!sem->available) assert(pthread_cond_wait(&sem->condition, &sem->mutex) == 0);
    } else {
        struct timespec deadline;
        assert(clock_gettime(CLOCK_REALTIME, &deadline) == 0);
        deadline.tv_sec += ticks / 1000U;
        deadline.tv_nsec += (long)(ticks % 1000U) * 1000000L;
        if (deadline.tv_nsec >= 1000000000L) {
            ++deadline.tv_sec;
            deadline.tv_nsec -= 1000000000L;
        }
        while (!sem->available) {
            const int result = pthread_cond_timedwait(&sem->condition, &sem->mutex, &deadline);
            if (result == ETIMEDOUT) break;
            assert(result == 0);
        }
    }
    const bool taken = sem->available;
    sem->available = false;
    assert(pthread_mutex_unlock(&sem->mutex) == 0);
    return taken ? pdTRUE : pdFALSE;
}

void vTaskDelay(TickType_t ticks)
{
    (void)ticks;
    const struct timespec delay = {.tv_nsec = 1000000L};
    assert(nanosleep(&delay, NULL) == 0);
}

int64_t esp_timer_get_time(void)
{
    struct timespec now;
    assert(clock_gettime(CLOCK_MONOTONIC, &now) == 0);
    return (int64_t)now.tv_sec * 1000000 + now.tv_nsec / 1000;
}

static void configure_product(const file_t *public_key)
{
    memset(&s_product, 0, sizeof s_product);
    s_product.provider.io = io;
    s_product.provider.geometry = geometry;
    s_product.provider_bound = true;
    s_product.validation = (econtainer_package_slot_validation_t){
        .expected_product_id = "counter", .public_key_rsa_der = public_key->bytes,
        .public_key_size_bytes = public_key->size, .expected_key_id = "test-key",
        .max_wasm_bytes = 16384,
        .wasm_authorization = {.allowed_capabilities = ECONTAINER_CAP_ALL,
            .max_memory_bytes = 65536, .max_stack_bytes = 16384},
        .max_event_queue_limit = 8, .max_instruction_budget = 100000,
        .max_host_call_timeout_ms = 100, .max_storage_limit_bytes = 0,
        .package_workspace = &s_product.package_workspace,
        .wasm_workspace = &s_product.wasm_workspace,
        .verified_info = &s_product.verified_info,
    };
    s_product.limits = (econtainer_runtime_limits_t){
        .max_wasm_bytes = 16384, .max_memory_pages = 1, .stack_size_bytes = 16384,
        .max_event_bytes = 128, .allowed_capabilities = ECONTAINER_CAP_ALL,
        .max_log_bytes = 16, .max_timers = 1,
        .init_instruction_budget = 200000, .event_instruction_budget = 200000,
        .stop_instruction_budget = 200000, .max_entry_duration_ms = 1000,
    };
}

static void dispose_product(void)
{
    binary_sem_t *ready = s_product.ready;
    binary_sem_t *stopped = s_product.stopped;
    assert(!s_product.thread_joinable);
    if (ready != NULL) {
        assert(pthread_cond_destroy(&ready->condition) == 0);
        assert(pthread_mutex_destroy(&ready->mutex) == 0);
        free(ready);
    }
    if (stopped != NULL) {
        assert(pthread_cond_destroy(&stopped->condition) == 0);
        assert(pthread_mutex_destroy(&stopped->mutex) == 0);
        free(stopped);
    }
    memset(&s_product, 0, sizeof s_product);
}

static void configure(const file_t *public_key)
{
    memset(&store, 0, sizeof store);
    assert(pthread_mutex_init(&store.mutex, NULL) == 0);
    memset(store.flash, 0xff, sizeof store.flash);
    physical = (esp_base_ota_firmware_set_t){.bootable_count = 2};
    memset(physical.bootable_firmware_sha256[0], 0x11, 32);
    memset(physical.bootable_firmware_sha256[1], 0x22, 32);
    memcpy(physical.running_firmware_sha256, physical.bootable_firmware_sha256[0], 32);
    configure_product(public_key);
}

typedef struct { const file_t *package; uint8_t operation_marker; } install_context_t;

static econtainer_slots_result_t install_signed(
    const econtainer_slot_firmware_set_t *firmware_set, void *context)
{
    const file_t *package = ((const install_context_t *)context)->package;
    econtainer_slots_state_t state = {0};
    econtainer_slots_result_t result = econtainer_slots_load(&io, &geometry, &state);
    if (result != ECONTAINER_SLOTS_OK) { fprintf(stderr, "install load=%d\n", (int)result); return result; }
    econtainer_slot_operation_t operation = {0};
    operation.operation_id[0] = ((const install_context_t *)context)->operation_marker;
    if (operation.operation_id[0] == 0U) operation.operation_id[0] = 0x44;
    memcpy(operation.target_firmware_sha256, firmware_set->running_firmware_sha256, 32);
    assert(SHA256(package->bytes, package->size, operation.package_sha256) != NULL);
    operation.package_size_bytes = (uint32_t)package->size;
    operation.guest_abi_version = 2;
    operation.data_schema_version = 1;
    result = econtainer_slots_reserve(&io, &geometry, state.sequence, firmware_set,
                                      &operation, &state);
    if (result != ECONTAINER_SLOTS_OK) { fprintf(stderr, "install reserve=%d\n", (int)result); return result; }
    result = econtainer_slots_write_and_prepare(&io, &geometry, state.sequence,
        read_source, (void *)package, econtainer_package_slot_validate,
        &s_product.validation, &state);
    if (result != ECONTAINER_SLOTS_OK) { fprintf(stderr, "install write=%d\n", (int)result); return result; }
    uint8_t trial_boot[ECONTAINER_SLOT_BOOT_ID_BYTES] = {0x55};
    result = econtainer_slots_begin_trial(&io, &geometry, state.sequence,
        firmware_set->running_firmware_sha256, trial_boot, &state);
    if (result != ECONTAINER_SLOTS_OK) { fprintf(stderr, "install begin=%d\n", (int)result); return result; }
    result = econtainer_slots_mark_healthy(&io, &geometry, state.sequence,
        trial_boot, &state);
    if (result != ECONTAINER_SLOTS_OK) { fprintf(stderr, "install healthy=%d\n", (int)result); return result; }
    result = econtainer_slots_confirm(&io, &geometry, state.sequence,
        firmware_set->running_firmware_sha256, trial_boot, &state);
    if (result != ECONTAINER_SLOTS_OK) fprintf(stderr, "install confirm=%d\n", (int)result);
    return result;
}

static econtainer_slots_result_t open_after_stop(
    const econtainer_slot_firmware_set_t *firmware_set, void *context)
{
    (void)context;
    econtainer_slots_state_t state = {0};
    econtainer_slots_result_t loaded = econtainer_slots_load(&io, &geometry, &state);
    if (loaded != ECONTAINER_SLOTS_OK) return loaded;
    econtainer_slot_selection_request_t request = {
        .expected_sequence = state.sequence, .firmware_set = *firmware_set,
        .selection = ECONTAINER_SLOT_SELECT_CONFIRMED,
    };
    econtainer_runtime_t *runtime = NULL;
    const econtainer_slot_runtime_result_t opened = econtainer_product_open(
        &io, &geometry, &request, &s_product.validation, &s_product.limits, &runtime);
    if (opened.slots != ECONTAINER_SLOTS_OK) return opened.slots;
    assert(opened.runtime == ECONTAINER_RUNTIME_OK && runtime != NULL);
    assert(econtainer_product_init(runtime) == ECONTAINER_RUNTIME_OK);
    const uint8_t event[] = {1, 2, 3};
    int32_t guest_result = -1;
    assert(econtainer_product_on_event(runtime, event, sizeof event, &guest_result) ==
           ECONTAINER_RUNTIME_OK);
    assert(guest_result == 3);
    assert(econtainer_product_stop(runtime) == ECONTAINER_RUNTIME_OK);
    assert(econtainer_product_close(&runtime) == ECONTAINER_RUNTIME_OK && runtime == NULL);
    return ECONTAINER_SLOTS_OK;
}

static void run_uninstall_with_fallback(const file_t *key, const file_t *package,
                                        const char boot_id[37])
{
    configure(key);
    esp_base_storage_owner_t owner;
    esp_base_storage_owner_init(&owner);
    esp_base_storage_claim_t claim = {0};
    assert(esp_base_storage_claim(&owner, &claim));
    assert(esp_base_container_product_boot(&claim, boot_id) == ESP_BASE_CONTAINER_EMPTY);
    install_context_t install_a = {.package = package, .operation_marker = 0x44};
    assert(esp_base_container_with_firmware_set(&claim, ESP_BASE_OTA_FIRMWARE_CONFIRMED,
        NULL, install_signed, &install_a) == ECONTAINER_SLOTS_OK);

    /* The second signed firmware is selected after a real persisted binding
     * exists for A. Its own signed package is installed under the same set. */
    physical = (esp_base_ota_firmware_set_t){.bootable_count = 2};
    memset(physical.bootable_firmware_sha256[0], 0x22, 32);
    memset(physical.bootable_firmware_sha256[1], 0x11, 32);
    memcpy(physical.running_firmware_sha256, physical.bootable_firmware_sha256[0], 32);
    install_context_t install_b = {.package = package, .operation_marker = 0x45};
    assert(esp_base_container_with_firmware_set(&claim, ESP_BASE_OTA_FIRMWARE_CONFIRMED,
        NULL, install_signed, &install_b) == ECONTAINER_SLOTS_OK);
    assert(esp_base_container_product_boot(&claim, boot_id) == ESP_BASE_CONTAINER_RUNNING);
    assert(esp_base_storage_release(&claim));
    assert(esp_base_storage_claim(&owner, &claim));

    econtainer_slots_state_t before = {0}, after = {0};
    assert(econtainer_slots_load(&io, &geometry, &before) == ECONTAINER_SLOTS_OK);
    const int running_index = binding_index(&before, physical.running_firmware_sha256);
    const int fallback_index = 1 - running_index;
    assert(running_index >= 0 && before.bindings[running_index].package_present &&
           before.bindings[fallback_index].package_present);
    uint8_t flash_before[FLASH_BYTES];
    memcpy(flash_before, store.flash, sizeof flash_before);
    const unsigned writes_before = store.blob_writes;
    const unsigned erases_before = store.flash_erases;
    const unsigned flash_writes_before = store.flash_writes;
    const char uninstall_id[] = "33333333-3333-4333-8333-333333333333";
    uint8_t wrong_digest[32] = {1};
    assert(esp_base_container_product_uninstall(&claim, uninstall_id,
        before.sequence - 1U, before.bindings[running_index].package_sha256) ==
        ESP_BASE_CONTAINER_UNINSTALL_REJECTED);
    assert(esp_base_container_product_uninstall(&claim, uninstall_id,
        before.sequence, wrong_digest) == ESP_BASE_CONTAINER_UNINSTALL_REJECTED);
    assert(s_product.thread_joinable &&
           atomic_load(&s_product.result) == ESP_BASE_CONTAINER_RUNNING &&
           store.blob_writes == writes_before);
    assert(esp_base_container_product_uninstall(&claim, uninstall_id,
        before.sequence, before.bindings[running_index].package_sha256) ==
        ESP_BASE_CONTAINER_UNINSTALL_COMPLETE);
    assert(esp_base_storage_claim_active(&claim));
    assert(econtainer_slots_load(&io, &geometry, &after) == ECONTAINER_SLOTS_OK);
    assert(after.sequence == before.sequence + 1U &&
           !after.bindings[running_index].package_present &&
           same_binding(&before.bindings[fallback_index], &after.bindings[fallback_index]));
    assert(store.blob_writes == writes_before + 1U &&
           store.flash_erases == erases_before &&
           store.flash_writes == flash_writes_before &&
           memcmp(flash_before, store.flash, sizeof flash_before) == 0);
    assert(esp_base_container_product_boot(&claim, boot_id) == ESP_BASE_CONTAINER_EMPTY);
    assert(esp_base_storage_release(&claim));
    dispose_product();
    assert(pthread_mutex_destroy(&store.mutex) == 0);
}

static void run_uninstall_uncertain(const file_t *key, const file_t *package,
                                    const char boot_id[37], unsigned failed_read)
{
    configure(key);
    esp_base_storage_owner_t owner;
    esp_base_storage_owner_init(&owner);
    esp_base_storage_claim_t claim = {0};
    assert(esp_base_storage_claim(&owner, &claim));
    assert(esp_base_container_product_boot(&claim, boot_id) == ESP_BASE_CONTAINER_EMPTY);
    install_context_t install = {.package = package};
    assert(esp_base_container_with_firmware_set(&claim, ESP_BASE_OTA_FIRMWARE_CONFIRMED,
        NULL, install_signed, &install) == ECONTAINER_SLOTS_OK);
    assert(esp_base_container_product_boot(&claim, boot_id) == ESP_BASE_CONTAINER_RUNNING);
    econtainer_slots_state_t state = {0};
    assert(econtainer_slots_load(&io, &geometry, &state) == ECONTAINER_SLOTS_OK);
    const int index = binding_index(&state, physical.running_firmware_sha256);
    assert(index >= 0 && state.bindings[index].package_present);
    const char uninstall_id[] = "44444444-4444-4444-8444-444444444444";
    store.fail_read_after_next_write = failed_read;
    assert(esp_base_container_product_uninstall(&claim, uninstall_id,
        state.sequence, state.bindings[index].package_sha256) ==
        ESP_BASE_CONTAINER_UNINSTALL_UNCERTAIN);
    assert(!s_product.thread_joinable && !s_product.reopen_allowed &&
           esp_base_storage_claim_active(&claim));
    assert(esp_base_container_product_boot(&claim, boot_id) == ESP_BASE_CONTAINER_BLOCKED);
    assert(econtainer_slots_load(&io, &geometry, &state) == ECONTAINER_SLOTS_OK &&
           !state.bindings[index].package_present);

    /* A new boot may resolve the durable state. The uncertain boot cannot. */
    dispose_product();
    configure_product(key);
    assert(esp_base_container_product_without_ota_receipt(&claim));
    assert(esp_base_container_product_boot(&claim, boot_id) == ESP_BASE_CONTAINER_EMPTY);
    assert(esp_base_storage_release(&claim));
    dispose_product();
    assert(pthread_mutex_destroy(&store.mutex) == 0);
}

static void run_uninstall_lost_state(const file_t *key, const file_t *package,
                                     const char boot_id[37])
{
    configure(key);
    esp_base_storage_owner_t owner;
    esp_base_storage_owner_init(&owner);
    esp_base_storage_claim_t claim = {0};
    assert(esp_base_storage_claim(&owner, &claim));
    assert(esp_base_container_product_boot(&claim, boot_id) == ESP_BASE_CONTAINER_EMPTY);
    install_context_t install = {.package = package};
    assert(esp_base_container_with_firmware_set(&claim, ESP_BASE_OTA_FIRMWARE_CONFIRMED,
        NULL, install_signed, &install) == ECONTAINER_SLOTS_OK);
    assert(esp_base_container_product_boot(&claim, boot_id) == ESP_BASE_CONTAINER_RUNNING);
    econtainer_slots_state_t state = {0};
    assert(econtainer_slots_load(&io, &geometry, &state) == ECONTAINER_SLOTS_OK);
    const int index = binding_index(&state, physical.running_firmware_sha256);
    assert(index >= 0 && state.bindings[index].package_present);
    const unsigned writes_before = store.blob_writes;
    store.present = false;
    const char uninstall_id[] = "77777777-7777-4777-8777-777777777777";
    assert(esp_base_container_product_uninstall(&claim, uninstall_id,
        state.sequence, state.bindings[index].package_sha256) ==
        ESP_BASE_CONTAINER_UNINSTALL_UNCERTAIN);
    assert(store.blob_writes == writes_before && s_product.thread_joinable &&
           atomic_load(&s_product.result) == ESP_BASE_CONTAINER_RUNNING &&
           s_product.uninstall_uncertain && !s_product.reopen_allowed);
    store.present = true;
    assert(esp_base_container_product_stop_confirmed(&claim));
    assert(!s_product.reopen_allowed &&
           esp_base_container_product_boot(&claim, boot_id) == ESP_BASE_CONTAINER_BLOCKED);
    assert(econtainer_slots_load(&io, &geometry, &state) == ECONTAINER_SLOTS_OK &&
           state.bindings[index].package_present);
    assert(esp_base_storage_release(&claim));
    dispose_product();
    assert(pthread_mutex_destroy(&store.mutex) == 0);
}

static void run_uninstall_reclaimed_guest(const file_t *key, const file_t *package,
                                          const char boot_id[37], bool corrupt_package)
{
    configure(key);
    esp_base_storage_owner_t owner;
    esp_base_storage_owner_init(&owner);
    esp_base_storage_claim_t claim = {0};
    assert(esp_base_storage_claim(&owner, &claim));
    assert(esp_base_container_product_boot(&claim, boot_id) == ESP_BASE_CONTAINER_EMPTY);
    install_context_t install = {.package = package};
    assert(esp_base_container_with_firmware_set(&claim, ESP_BASE_OTA_FIRMWARE_CONFIRMED,
        NULL, install_signed, &install) == ECONTAINER_SLOTS_OK);
    econtainer_slots_state_t state = {0};
    assert(econtainer_slots_load(&io, &geometry, &state) == ECONTAINER_SLOTS_OK);
    const int index = binding_index(&state, physical.running_firmware_sha256);
    assert(index >= 0 && state.bindings[index].package_present);
    if (corrupt_package) {
        const size_t flash_index = geometry.slots[state.bindings[index].slot].offset_bytes -
                                   FLASH_BASE;
        store.flash[flash_index] ^= 1U;
        dispose_product();
        configure_product(key);
        assert(esp_base_container_product_boot(&claim, boot_id) == ESP_BASE_CONTAINER_BLOCKED);
        assert(!s_product.thread_joinable && s_product.native_reclaimed &&
               !atomic_load(&s_product.instance_active));
    } else {
        assert(esp_base_container_product_boot(&claim, boot_id) == ESP_BASE_CONTAINER_RUNNING);
        assert(esp_base_container_product_stop_confirmed(&claim));
        assert(!s_product.thread_joinable && s_product.native_reclaimed);
    }
    uint8_t flash_before[FLASH_BYTES];
    memcpy(flash_before, store.flash, sizeof flash_before);
    const char uninstall_id[] = "88888888-8888-4888-8888-888888888888";
    assert(esp_base_container_product_uninstall(&claim, uninstall_id,
        state.sequence, state.bindings[index].package_sha256) ==
        ESP_BASE_CONTAINER_UNINSTALL_COMPLETE);
    assert(memcmp(flash_before, store.flash, sizeof flash_before) == 0);
    assert(esp_base_container_product_boot(&claim, boot_id) == ESP_BASE_CONTAINER_EMPTY);
    assert(esp_base_storage_release(&claim));
    dispose_product();
    assert(pthread_mutex_destroy(&store.mutex) == 0);
}

static void run_uninstall_stop_timeout(const file_t *key, const file_t *package,
                                       const char boot_id[37])
{
    configure(key);
    esp_base_storage_owner_t owner;
    esp_base_storage_owner_init(&owner);
    esp_base_storage_claim_t claim = {0};
    assert(esp_base_storage_claim(&owner, &claim));
    assert(esp_base_container_product_boot(&claim, boot_id) == ESP_BASE_CONTAINER_EMPTY);
    install_context_t install = {.package = package};
    assert(esp_base_container_with_firmware_set(&claim, ESP_BASE_OTA_FIRMWARE_CONFIRMED,
        NULL, install_signed, &install) == ECONTAINER_SLOTS_OK);
    assert(esp_base_container_product_boot(&claim, boot_id) == ESP_BASE_CONTAINER_RUNNING);
    econtainer_slots_state_t before = {0}, after = {0};
    assert(econtainer_slots_load(&io, &geometry, &before) == ECONTAINER_SLOTS_OK);
    const int index = binding_index(&before, physical.running_firmware_sha256);
    assert(index >= 0 && before.bindings[index].package_present);
    const unsigned writes_before = store.blob_writes;
    timeout_stop_once = true;
    const char uninstall_id[] = "66666666-6666-4666-8666-666666666666";
    assert(esp_base_container_product_uninstall(&claim, uninstall_id,
        before.sequence, before.bindings[index].package_sha256) ==
        ESP_BASE_CONTAINER_UNINSTALL_UNCERTAIN);
    assert(esp_base_storage_claim_active(&claim) && !s_product.reopen_allowed &&
           store.blob_writes == writes_before);
    assert(esp_base_container_product_boot(&claim, boot_id) == ESP_BASE_CONTAINER_BLOCKED);
    assert(pthread_join(s_product.thread, NULL) == 0);
    s_product.thread_joinable = false; /* Test teardown after unproven stop. */
    assert(econtainer_slots_load(&io, &geometry, &after) == ECONTAINER_SLOTS_OK &&
           same_binding(&before.bindings[index], &after.bindings[index]));
    assert(esp_base_storage_release(&claim));
    dispose_product();
    assert(pthread_mutex_destroy(&store.mutex) == 0);
}

static void run_success_receipt_replay(const file_t *key, const file_t *package,
                                       const char boot_id[37])
{
    configure(key);
    esp_base_storage_owner_t owner;
    esp_base_storage_owner_init(&owner);
    esp_base_storage_claim_t claim = {0};
    assert(esp_base_storage_claim(&owner, &claim));
    assert(esp_base_container_product_boot(&claim, boot_id) == ESP_BASE_CONTAINER_EMPTY);
    econtainer_slots_state_t state = {0};
    assert(econtainer_slots_load(&io, &geometry, &state) == ECONTAINER_SLOTS_OK);
    const uint32_t original_sequence = state.sequence;

    /* Real Container firmware transition: retire B, stage C without a
     * package, begin its trial, mark healthy, and confirm it. */
    uint8_t old_a[32], old_b[32], new_c[32];
    memset(old_a, 0x11, sizeof old_a);
    memset(old_b, 0x22, sizeof old_b);
    memset(new_c, 0x33, sizeof new_c);
    econtainer_slot_firmware_set_t a_only = {.bootable_count = 1};
    memcpy(a_only.running_firmware_sha256, old_a, 32);
    memcpy(a_only.bootable_firmware_sha256[0], old_a, 32);
    physical.bootable_count = 1;
    memset(physical.bootable_firmware_sha256[1], 0, 32);
    assert(econtainer_slots_retire_inactive_firmware(&io, &geometry,
        state.sequence, &a_only, old_b, &state) == ECONTAINER_SLOTS_OK);
    econtainer_slot_firmware_set_t prepared = {.bootable_count = 2};
    memcpy(prepared.running_firmware_sha256, old_a, 32);
    memcpy(prepared.bootable_firmware_sha256[0], old_a, 32);
    memcpy(prepared.bootable_firmware_sha256[1], new_c, 32);
    const char operation_id[] = "11111111-1111-4111-8111-111111111111";
    econtainer_slot_operation_t operation = {.kind = ECONTAINER_SLOT_NO_PACKAGE};
    assert(decode_uuid(operation_id, operation.operation_id));
    memcpy(operation.target_firmware_sha256, new_c, 32);
    assert(econtainer_slots_stage_firmware(&io, &geometry, state.sequence,
        &prepared, &operation, NULL, NULL, &state) == ECONTAINER_SLOTS_OK);
    uint8_t trial_boot[ECONTAINER_SLOT_BOOT_ID_BYTES] = {0x66};
    assert(econtainer_slots_begin_trial(&io, &geometry, state.sequence,
        new_c, trial_boot, &state) == ECONTAINER_SLOTS_OK);
    assert(econtainer_slots_mark_healthy(&io, &geometry, state.sequence,
        trial_boot, &state) == ECONTAINER_SLOTS_OK);
    assert(econtainer_slots_confirm(&io, &geometry, state.sequence,
        new_c, trial_boot, &state) == ECONTAINER_SLOTS_OK);
    assert(state.sequence == original_sequence + 5U);

    physical = (esp_base_ota_firmware_set_t){.bootable_count = 2};
    memcpy(physical.running_firmware_sha256, new_c, 32);
    memcpy(physical.bootable_firmware_sha256[0], new_c, 32);
    memcpy(physical.bootable_firmware_sha256[1], old_a, 32);
    esp_base_ota_receipt_recovery_t receipt = {
        .status = ESP_BASE_OTA_RECEIPT_SUCCEEDED,
        .container_enabled = true,
        .container_sequence = original_sequence,
    };
    strcpy(receipt.operation_id, operation_id);
    memcpy(receipt.source_sha256, old_a, 32);
    memcpy(receipt.inactive_sha256, old_b, 32);
    memcpy(receipt.candidate_sha256, new_c, 32);

    /* A new boot first proves the exact historical C confirmation. */
    dispose_product();
    configure_product(key);
    assert(esp_base_container_product_reconcile_selected_ota(
        &claim, &receipt, EOTA_STATE_VALID));
    install_context_t install = {.package = package};
    assert(esp_base_container_with_firmware_set(&claim, ESP_BASE_OTA_FIRMWARE_CONFIRMED,
        NULL, install_signed, &install) == ECONTAINER_SLOTS_OK);

    /* The old V2 receipt remains on the next boot after the product-only
     * operation replaces its ECS2 operation. Both old and new slot histories
     * are real durable Container transitions in this fixture. */
    dispose_product();
    configure_product(key);
    assert(esp_base_container_product_reconcile_selected_ota(
        &claim, &receipt, EOTA_STATE_VALID));
    receipt.status = ESP_BASE_OTA_RECEIPT_PREPARED;
    assert(!esp_base_container_product_reconcile_selected_ota(
        &claim, &receipt, EOTA_STATE_VALID));
    receipt.status = ESP_BASE_OTA_RECEIPT_SUCCEEDED;
    assert(econtainer_slots_load(&io, &geometry, &state) == ECONTAINER_SLOTS_OK);
    unsigned selected_slot = ECONTAINER_SLOT_COUNT;
    for (unsigned index = 0; index < ECONTAINER_SLOT_BINDING_COUNT; ++index) {
        if (state.bindings[index].present &&
            memcmp(state.bindings[index].firmware_sha256, new_c, 32) == 0) {
            assert(state.bindings[index].package_present);
            selected_slot = state.bindings[index].slot;
        }
    }
    assert(selected_slot < ECONTAINER_SLOT_COUNT);
    const size_t flash_index = geometry.slots[selected_slot].offset_bytes - FLASH_BASE;
    store.flash[flash_index] ^= 1U;
    assert(!esp_base_container_product_reconcile_selected_ota(
        &claim, &receipt, EOTA_STATE_VALID));
    store.flash[flash_index] ^= 1U;
    assert(esp_base_container_product_reconcile_selected_ota(
        &claim, &receipt, EOTA_STATE_VALID));
    assert(esp_base_container_product_boot(&claim, boot_id) == ESP_BASE_CONTAINER_RUNNING);
    assert(econtainer_slots_load(&io, &geometry, &state) == ECONTAINER_SLOTS_OK);
    const int running_index = binding_index(&state, new_c);
    assert(running_index >= 0 && state.bindings[running_index].package_present);
    uint8_t flash_before[FLASH_BYTES];
    memcpy(flash_before, store.flash, sizeof flash_before);
    const char uninstall_id[] = "55555555-5555-4555-8555-555555555555";
    assert(esp_base_container_product_uninstall(&claim, uninstall_id,
        state.sequence, state.bindings[running_index].package_sha256) ==
        ESP_BASE_CONTAINER_UNINSTALL_COMPLETE);
    assert(memcmp(flash_before, store.flash, sizeof flash_before) == 0);
    assert(esp_base_container_product_boot(&claim, boot_id) == ESP_BASE_CONTAINER_EMPTY);
    dispose_product();
    configure_product(key);
    assert(esp_base_container_product_reconcile_selected_ota(
        &claim, &receipt, EOTA_STATE_VALID));
    receipt.status = ESP_BASE_OTA_RECEIPT_PREPARED;
    assert(!esp_base_container_product_reconcile_selected_ota(
        &claim, &receipt, EOTA_STATE_VALID));
    receipt.status = ESP_BASE_OTA_RECEIPT_SUCCEEDED;
    assert(esp_base_container_product_boot(&claim, boot_id) == ESP_BASE_CONTAINER_EMPTY);
    assert(esp_base_storage_release(&claim));
    dispose_product();
    assert(pthread_mutex_destroy(&store.mutex) == 0);
}

int main(int argc, char **argv)
{
    assert(argc == 2);
    file_t key = read_file(argv[1], "public.der");
    file_t package = read_file(argv[1], "p0.pkg");
    configure(&key);
    esp_base_storage_owner_t owner;
    esp_base_storage_owner_init(&owner);
    esp_base_storage_claim_t claim = {0};
    const char boot_id[] = "22222222-2222-4222-8222-222222222222";

    assert(esp_base_storage_claim(&owner, &claim));
    assert(esp_base_container_product_boot(&claim, boot_id) == ESP_BASE_CONTAINER_EMPTY);
    assert(!esp_base_container_product_stop_confirmed(&claim));
    assert(esp_base_storage_release(&claim));
    assert(esp_base_container_product_boot(&claim, boot_id) == ESP_BASE_CONTAINER_BLOCKED);

    assert(esp_base_storage_claim(&owner, &claim));
    install_context_t install = {.package = &package};
    const econtainer_slots_result_t installed = esp_base_container_with_firmware_set(
        &claim, ESP_BASE_OTA_FIRMWARE_CONFIRMED, NULL, install_signed, &install);
    assert(installed == ECONTAINER_SLOTS_OK);
    assert(store.blob_writes >= 5 && store.flash_erases != 0 && store.flash_writes != 0);
    const unsigned blob_writes = store.blob_writes;
    const unsigned flash_erases = store.flash_erases;
    const unsigned flash_writes = store.flash_writes;
    uint8_t confirmed_blob[ECONTAINER_SLOT_BLOB_BYTES];
    memcpy(confirmed_blob, store.blob, sizeof confirmed_blob);

    assert(esp_base_container_product_boot(&claim, boot_id) == ESP_BASE_CONTAINER_RUNNING);
    assert(esp_base_container_product_boot(&claim, boot_id) == ESP_BASE_CONTAINER_BLOCKED);
    assert(esp_base_storage_release(&claim));
    assert(esp_base_storage_claim(&owner, &claim));
    assert(esp_base_container_product_stop_confirmed(&claim));
    assert(!esp_base_container_product_stop_confirmed(&claim));
    assert(esp_base_container_with_firmware_set(&claim, ESP_BASE_OTA_FIRMWARE_CONFIRMED,
        NULL, open_after_stop, NULL) == ECONTAINER_SLOTS_OK);
    assert(esp_base_container_product_boot(&claim, boot_id) == ESP_BASE_CONTAINER_RUNNING);
    assert(esp_base_container_product_stop_confirmed(&claim));
    assert(memcmp(store.blob, confirmed_blob, sizeof confirmed_blob) == 0);
    assert(store.blob_writes == blob_writes && store.flash_erases == flash_erases &&
           store.flash_writes == flash_writes);
    assert(esp_base_storage_release(&claim));
    assert(!store.locked && store.mapping == NULL);
    dispose_product();
    assert(pthread_mutex_destroy(&store.mutex) == 0);
    run_uninstall_with_fallback(&key, &package, boot_id);
    run_uninstall_uncertain(&key, &package, boot_id, 1U);
    run_uninstall_uncertain(&key, &package, boot_id, 2U);
    run_uninstall_lost_state(&key, &package, boot_id);
    run_uninstall_reclaimed_guest(&key, &package, boot_id, false);
    run_uninstall_reclaimed_guest(&key, &package, boot_id, true);
    run_uninstall_stop_timeout(&key, &package, boot_id);
    run_success_receipt_replay(&key, &package, boot_id);
    free(package.bytes);
    free(key.bytes);
    puts("container_product_lifecycle: signed stop/uninstall/readback, fallback and V2 replay passed");
    return 0;
}

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
#include <stdatomic.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "esp_container_package_slot.h"
#ifdef ESP_BASE_TEST_RESOURCE_STATS
#include <malloc/malloc.h>
#include <mach/mach.h>
#include <mach/task_info.h>
#include <sys/mman.h>
#endif

static atomic_bool fail_metadata_allocation;
static bool observe_event_allocations, stop_during_event_copy;
static size_t event_allocation_attempts, event_allocation_count, event_release_count;
static struct {
    void *pointer;
    size_t size_bytes;
    bool released;
} event_allocations[16];
static void *event_copy_target;
static void *test_product_malloc(size_t size_bytes)
{
    if (observe_event_allocations) ++event_allocation_attempts;
    void *memory = atomic_exchange(&fail_metadata_allocation, false) ? NULL : malloc(size_bytes);
    if (observe_event_allocations && memory != NULL) {
        assert(event_allocation_count < sizeof event_allocations / sizeof event_allocations[0]);
        event_allocations[event_allocation_count].pointer = memory;
        event_allocations[event_allocation_count].size_bytes = size_bytes;
        event_allocations[event_allocation_count++].released = false;
        event_copy_target = memory;
    }
    return memory;
}
static void test_product_free(void *memory);
static void *test_product_memcpy(void *out, const void *in, size_t length);
static void *test_product_calloc(size_t count, size_t size_bytes);
static econtainer_slot_validation_result_t test_written_package_validate(
    void *context, const econtainer_slot_operation_t *operation,
    econtainer_slot_read_fn read_fn, void *read_context, size_t package_size_bytes,
    const econtainer_slot_references_t *references);
bool test_policy_enabled = true;
#define malloc test_product_malloc
#define calloc test_product_calloc
#define free test_product_free
#define econtainer_package_slot_validate test_written_package_validate
#pragma push_macro("memcpy")
#undef memcpy
#define memcpy test_product_memcpy
#include "esp_base_container_product.c"
#undef malloc
#undef calloc
#undef free
#undef econtainer_package_slot_validate
#undef memcpy
#pragma pop_macro("memcpy")

/* Observe only the Base translation unit's per-WRITE workspace. Container's
 * real signature/Wasm validator and all test fixture allocations stay real. */
static struct {
    bool active, fail_allocation, fail_aborted_readback, source_complete;
    size_t source_bytes, callback_allocation_attempts, allocations, releases;
    unsigned core_validator_calls, reference_read_mask, corrupt_reference_mask;
    firmware_stage_workspace_t *live;
    econtainer_package_workspace_t *policy_package;
    econtainer_wasm_workspace_t *policy_wasm;
    econtainer_package_info_t *policy_info;
} write_workspace_observation;

static void begin_write_workspace_observation(bool fail_allocation,
                                               bool fail_aborted_readback)
{
    assert(!write_workspace_observation.active && write_workspace_observation.live == NULL);
    memset(&write_workspace_observation, 0, sizeof write_workspace_observation);
    write_workspace_observation.active = true;
    write_workspace_observation.fail_allocation = fail_allocation;
    write_workspace_observation.fail_aborted_readback = fail_aborted_readback;
    write_workspace_observation.policy_package = s_product.validation.package_workspace;
    write_workspace_observation.policy_wasm = s_product.validation.wasm_workspace;
    write_workspace_observation.policy_info = s_product.validation.verified_info;
}

static void end_write_workspace_observation(size_t callback_allocation_attempts,
                                            unsigned core_validator_calls,
                                            unsigned reference_read_mask)
{
    assert(write_workspace_observation.active && write_workspace_observation.live == NULL);
    assert(write_workspace_observation.callback_allocation_attempts == callback_allocation_attempts &&
           write_workspace_observation.core_validator_calls == core_validator_calls &&
           write_workspace_observation.reference_read_mask == reference_read_mask);
    const size_t allocated = write_workspace_observation.fail_allocation ? 0U :
                             callback_allocation_attempts;
    assert(write_workspace_observation.allocations == allocated &&
           write_workspace_observation.releases == allocated);
    assert(s_product.validation.package_workspace == write_workspace_observation.policy_package &&
           s_product.validation.wasm_workspace == write_workspace_observation.policy_wasm &&
           s_product.validation.verified_info == write_workspace_observation.policy_info);
    write_workspace_observation.active = false;
}

static bool read_observed_reference(void *context, unsigned index, size_t offset,
                                    uint8_t *bytes, size_t size)
{
    const econtainer_slot_references_t *references = context;
    assert(write_workspace_observation.active && write_workspace_observation.live != NULL &&
           index < ECONTAINER_SLOT_BINDING_COUNT && size > 0U && size <= 512U);
    const econtainer_slot_binding_t *binding = &references->bindings[index];
    assert(binding->present && binding->package_present &&
           offset <= binding->package_size_bytes &&
           size <= binding->package_size_bytes - offset);
    write_workspace_observation.reference_read_mask |= 1U << index;
    if (!references->read(references->context, index, offset, bytes, size)) return false;
    /* Fault only the validator's bounded reference read, after the slot engine
     * checked its digest. The real package/signature verifier must reject it. */
    if (offset == 0U && (write_workspace_observation.corrupt_reference_mask & (1U << index)))
        bytes[0] ^= 1U;
    return true;
}

static econtainer_slot_validation_result_t test_written_package_validate(
    void *context, const econtainer_slot_operation_t *operation,
    econtainer_slot_read_fn read_fn, void *read_context, size_t package_size_bytes,
    const econtainer_slot_references_t *references)
{
    if (!write_workspace_observation.active)
        return econtainer_package_slot_validate(context, operation, read_fn,
            read_context, package_size_bytes, references);
    econtainer_package_slot_validation_t *validation = context;
    firmware_stage_workspace_t *workspace = write_workspace_observation.live;
    assert(workspace != NULL && validation != &s_product.validation &&
           validation->package_workspace == &workspace->package &&
           validation->wasm_workspace == &workspace->wasm &&
           validation->verified_info == &workspace->info &&
           write_workspace_observation.source_complete && references != NULL);
    assert(++write_workspace_observation.core_validator_calls == 1U);
    econtainer_slot_references_t observed = *references;
    observed.read = read_observed_reference;
    observed.context = (void *)references;
    return econtainer_package_slot_validate(context, operation, read_fn,
        read_context, package_size_bytes, &observed);
}

static void test_product_free(void *memory)
{
    if (write_workspace_observation.active && memory != NULL &&
        memory == write_workspace_observation.live) {
        assert(write_workspace_observation.allocations == 1U &&
               write_workspace_observation.releases == 0U);
        write_workspace_observation.live = NULL;
        ++write_workspace_observation.releases;
    }
    if (observe_event_allocations && memory != NULL) {
        for (size_t index = event_allocation_count; index != 0U; --index) {
            if (event_allocations[index - 1U].pointer != memory) continue;
            assert(!event_allocations[index - 1U].released);
            const product_event_t *event = memory;
            assert(event->size_bytes == event_allocations[index - 1U].size_bytes - sizeof(*event));
            for (size_t byte = 0; byte < event->size_bytes; ++byte)
                assert(event->bytes[byte] == 0U);
            event_allocations[index - 1U].released = true;
            ++event_release_count;
            break;
        }
    }
    free(memory);
}

static void *test_product_memcpy(void *out, const void *in, size_t length)
{
    void *result = memcpy(out, in, length);
    if (observe_event_allocations && stop_during_event_copy &&
        out == (uint8_t *)event_copy_target + offsetof(product_event_t, bytes)) {
        stop_during_event_copy = false;
        atomic_store_explicit(&s_product.stop_requested, true, memory_order_release);
    }
    return result;
}

static bool wait_trial_snapshot(esp_base_container_trial_event_snapshot_t *snapshot)
{
    for (unsigned attempt = 0; attempt < 200U; ++attempt) {
        if (esp_base_container_product_trial_event_snapshot(snapshot)) return true;
        vTaskDelay(1U);
    }
    return false;
}

static esp_base_container_event_result_t offer_event(
    const uint8_t package_sha256[32], uint64_t event_sequence,
    const uint8_t *event, size_t size_bytes)
{
    uint8_t digest[32] = {0};
    if (size_bytes != 0U) assert(SHA256(event, size_bytes, digest) != NULL);
    return esp_base_container_product_offer_event(
        package_sha256, event_sequence, digest, event, size_bytes);
}

/* BUSY has not admitted a queue entry. Bound positive admission retries;
 * invalid/unavailable tests still call the one-attempt helper above. */
static esp_base_container_event_result_t offer_event_when_available(
    const uint8_t package_sha256[32], uint64_t event_sequence,
    const uint8_t *event, size_t size_bytes)
{
    esp_base_container_event_result_t result = ESP_BASE_CONTAINER_EVENT_BUSY;
    for (unsigned attempt = 0; attempt < 200U && result == ESP_BASE_CONTAINER_EVENT_BUSY;
         ++attempt) {
        result = offer_event(package_sha256, event_sequence, event, size_bytes);
        if (result == ESP_BASE_CONTAINER_EVENT_BUSY) vTaskDelay(1U);
    }
    return result;
}

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
    uint32_t fail_flash_read_offset;
    unsigned flash_erases;
    unsigned flash_writes;
} store_t;

typedef struct { uint8_t *bytes; size_t size; } file_t;

static file_t expected_confirmed_version, expected_candidate_version, expected_second_version;
static store_t store;
static void *test_product_calloc(size_t count, size_t size_bytes)
{
    if (!write_workspace_observation.active || count != 1U ||
        size_bytes != sizeof(firmware_stage_workspace_t)) return calloc(count, size_bytes);
    /* This allocation is the private WRITE callback's first statement; its
     * count does not claim that the core validator ran on allocation failure. */
    assert(++write_workspace_observation.callback_allocation_attempts == 1U &&
           write_workspace_observation.live == NULL &&
           write_workspace_observation.source_complete);
    if (write_workspace_observation.fail_allocation) {
        if (write_workspace_observation.fail_aborted_readback)
            store.fail_read_after_next_write = 2U;
        return NULL;
    }
    void *memory = calloc(count, size_bytes);
    assert(memory != NULL);
    write_workspace_observation.live = memory;
    ++write_workspace_observation.allocations;
    return memory;
}
static esp_base_storage_owner_t flash_io_owner;
static econtainer_package_workspace_t test_package_workspace;
static econtainer_wasm_workspace_t test_wasm_workspace;
static econtainer_package_info_t test_verified_info;
static uint8_t trial_event_digest[32];
static bool reject_confirmed_observation;
static const econtainer_slots_geometry_t geometry = {
    .partition_offset_bytes = FLASH_BASE, .partition_size_bytes = FLASH_BYTES,
    .erase_unit_bytes = 4096, .write_unit_bytes = 4,
    .slots = {{FLASH_BASE, SLOT_BYTES}, {FLASH_BASE + SLOT_BYTES, SLOT_BYTES},
              {FLASH_BASE + 2 * SLOT_BYTES, SLOT_BYTES}},
};
static esp_base_ota_firmware_set_t physical;

#ifdef ESP_BASE_TEST_RESOURCE_STATS
typedef struct {
    size_t malloc_bytes;
    mach_vm_size_t virtual_bytes;
    integer_t regions;
} resource_stats_t;

static bool sample_resources(resource_stats_t *sample)
{
    malloc_statistics_t statistics = {0};
    malloc_zone_statistics(malloc_default_zone(), &statistics);
    task_vm_info_data_t vm = {0};
    mach_msg_type_number_t count = TASK_VM_INFO_COUNT;
    if (task_info(mach_task_self(), TASK_VM_INFO, (task_info_t)&vm, &count) !=
        KERN_SUCCESS) return false;
    *sample = (resource_stats_t){statistics.size_in_use, vm.virtual_size,
                                 vm.region_count};
    return sample->malloc_bytes > 0 && sample->virtual_bytes > 0 &&
           sample->regions > 0;
}

static bool resource_probes_calibrated(void)
{
    resource_stats_t before = {0}, during = {0};
    if (!sample_resources(&before)) return false;
    void *heap = malloc(65536);
    if (heap == NULL) return false;
    memset(heap, 0x5a, 65536);
    const bool heap_visible = sample_resources(&during) &&
                              during.malloc_bytes >= before.malloc_bytes + 65536;
    free(heap);
    if (!heap_visible || !sample_resources(&before)) return false;
    void *mapping = mmap(NULL, 65536, PROT_READ | PROT_WRITE,
                         MAP_ANON | MAP_PRIVATE, -1, 0);
    if (mapping == MAP_FAILED) return false;
    const bool mapping_visible = sample_resources(&during) &&
                                 during.virtual_bytes >= before.virtual_bytes + 65536;
    return munmap(mapping, 65536) == 0 && mapping_visible;
}

static bool resources_stable(resource_stats_t after_ten,
                             resource_stats_t after_fifty,
                             resource_stats_t after_hundred)
{
    fprintf(stderr, "Base product resources 10/50/100: malloc=%zu/%zu/%zu "
            "virtual=%llu/%llu/%llu regions=%d/%d/%d\n",
            after_ten.malloc_bytes, after_fifty.malloc_bytes,
            after_hundred.malloc_bytes,
            (unsigned long long)after_ten.virtual_bytes,
            (unsigned long long)after_fifty.virtual_bytes,
            (unsigned long long)after_hundred.virtual_bytes,
            after_ten.regions, after_fifty.regions, after_hundred.regions);
    /* A VM map can split one region without allocating virtual or heap bytes.
     * Keep its count in the receipt; leak rejection uses stable byte totals. */
    return after_ten.malloc_bytes == after_fifty.malloc_bytes &&
           after_fifty.malloc_bytes == after_hundred.malloc_bytes &&
           after_ten.virtual_bytes == after_fifty.virtual_bytes &&
           after_fifty.virtual_bytes == after_hundred.virtual_bytes;
}
#endif

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
    if (device->fail_flash_read_offset != 0U &&
        offset <= device->fail_flash_read_offset &&
        (uint64_t)offset + size > device->fail_flash_read_offset) return false;
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

static bool unmap_flash(void *context, uintptr_t handle)
{
    store_t *device = context;
    assert(device->locked && handle == 0 && device->mapping != NULL);
    free(device->mapping);
    device->mapping = NULL;
    return true;
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
    if (write_workspace_observation.active) {
        assert(write_workspace_observation.callback_allocation_attempts == 0U &&
               write_workspace_observation.live == NULL &&
               offset == write_workspace_observation.source_bytes);
    }
    if (offset > file->size || size > file->size - offset) return false;
    memcpy(bytes, file->bytes + offset, size);
    if (write_workspace_observation.active) {
        write_workspace_observation.source_bytes += size;
        write_workspace_observation.source_complete = offset + size == file->size;
    }
    return true;
}

static bool read_failed_source(void *context, size_t offset, uint8_t *bytes, size_t size)
{
    if (!read_source(context, offset, bytes, size)) return false;
    if (offset != 0U) {
        write_workspace_observation.source_complete = false;
        return false;
    }
    return true;
}

static bool read_corrupt_source(void *context, size_t offset,
                                uint8_t *bytes, size_t size)
{
    if (!read_source(context, offset, bytes, size)) return false;
    if (offset == 0U) bytes[0] ^= 0x01U;
    return true;
}

esp_base_ota_firmware_result_t esp_base_ota_observe_firmware_set(
    esp_base_ota_firmware_observation_t observation, const eota_prepared_t *prepared,
    esp_base_ota_firmware_set_t *firmware_set)
{
    assert(((observation == ESP_BASE_OTA_FIRMWARE_CONFIRMED ||
             observation == ESP_BASE_OTA_FIRMWARE_PENDING_TRIAL) &&
            prepared == NULL) ||
           (observation == ESP_BASE_OTA_FIRMWARE_PREPARED_CANDIDATE &&
            prepared != NULL));
    if (observation == ESP_BASE_OTA_FIRMWARE_CONFIRMED && reject_confirmed_observation)
        return ESP_BASE_OTA_FIRMWARE_UNCERTAIN;
    *firmware_set = physical;
    if (prepared != NULL) {
        assert(physical.bootable_count == 1U);
        firmware_set->bootable_count = 2U;
        memcpy(firmware_set->bootable_firmware_sha256[1], prepared->sha256, 32);
    }
    return ESP_BASE_OTA_FIRMWARE_OK;
}

void vTaskPrioritySet(TaskHandle_t task, UBaseType_t priority)
{
    /* The real priority relationship is verified separately with IDF/QEMU. */
    assert(task == NULL && priority > 0U && priority < 4U);
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
    esp_base_storage_owner_init(&flash_io_owner);
    s_product.flash_io_owner = &flash_io_owner;
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
        .package_workspace = &test_package_workspace,
        .wasm_workspace = &test_wasm_workspace,
        .verified_info = &test_verified_info,
    };
    s_product.limits = (econtainer_runtime_limits_t){
        .max_wasm_bytes = 16384, .max_memory_pages = 1, .stack_size_bytes = 16384,
        .max_event_bytes = 128, .allowed_capabilities = ECONTAINER_CAP_ALL,
        .max_log_bytes = 16, .max_timers = 1,
        .init_instruction_budget = 200000, .event_instruction_budget = 200000,
        .stop_instruction_budget = 200000, .max_entry_duration_ms = 1000,
        .max_host_call_timeout_ms = 100,
        .cancel_requested = product_cancel_requested,
        .cancel_context = &s_product.stop_requested,
    };
}

static void dispose_product(void)
{
    binary_sem_t *ready = s_product.ready;
    binary_sem_t *stopped = s_product.stopped;
    binary_sem_t *event_lock = s_product.event_lock;
    assert(!s_product.thread_joinable);
    assert(s_product.event_queue == NULL && s_product.event_count == 0U);
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
    if (event_lock != NULL) {
        assert(pthread_cond_destroy(&event_lock->condition) == 0);
        assert(pthread_mutex_destroy(&event_lock->mutex) == 0);
        free(event_lock);
    }
    memset(&s_product, 0, sizeof s_product);
}

static void prepare_event_admission_queue(const uint8_t package_sha256[32])
{
    assert(s_product.event_queue == NULL && s_product.event_count == 0U);
    s_product.event_queue = calloc(8U, sizeof(*s_product.event_queue));
    assert(s_product.event_queue != NULL);
    s_product.event_capacity = 8U;
    s_product.limits.max_event_bytes = 4096U;
    memcpy(s_product.event_package_sha256, package_sha256, 32U);
    atomic_store(&s_product.result, ESP_BASE_CONTAINER_RUNNING);
    atomic_store(&s_product.stop_requested, false);
    atomic_store(&s_product.event_accepting, true);
}

static void check_event_admission(void)
{
    /* The authenticated 4096-byte MQTT frame leaves 3893 guest bytes after
     * its 203-byte signed prefix. Schedule the real FIFO consumer explicitly. */
    enum { AUTHENTICATED_EVENT_BYTES = 3893U, RUNTIME_EVENT_BYTES = 4096U };
    uint8_t event[RUNTIME_EVENT_BYTES + 1U], package_sha256[32], wrong_sha256[32];
    memset(event, 0x5a, sizeof event);
    memset(package_sha256, 0x11, sizeof package_sha256);
    memcpy(wrong_sha256, package_sha256, sizeof wrong_sha256);
    wrong_sha256[0] ^= 1U;
    memset(&s_product, 0, sizeof s_product);
    s_product.event_lock = xSemaphoreCreateMutex();
    assert(s_product.event_lock != NULL);
    prepare_event_admission_queue(package_sha256);
    observe_event_allocations = true;
    event_allocation_attempts = event_allocation_count = event_release_count = 0U;
    memset(event_allocations, 0, sizeof event_allocations);

    assert(offer_event(package_sha256, 0U, event, 1U) == ESP_BASE_CONTAINER_EVENT_INVALID);
    assert(offer_event(package_sha256, 1U, event, 0U) == ESP_BASE_CONTAINER_EVENT_INVALID);
    assert(offer_event(package_sha256, 1U, event, sizeof event) == ESP_BASE_CONTAINER_EVENT_INVALID);
    uint8_t digest[32];
    assert(SHA256(event, 1U, digest) != NULL);
    assert(esp_base_container_product_offer_event(NULL, 1U, digest, event, 1U) ==
           ESP_BASE_CONTAINER_EVENT_INVALID);
    assert(esp_base_container_product_offer_event(package_sha256, 1U, NULL, event, 1U) ==
           ESP_BASE_CONTAINER_EVENT_INVALID);
    assert(esp_base_container_product_offer_event(package_sha256, 1U, digest, NULL, 1U) ==
           ESP_BASE_CONTAINER_EVENT_INVALID);
    atomic_store(&s_product.event_accepting, false);
    assert(offer_event(package_sha256, 1U, event, 1U) == ESP_BASE_CONTAINER_EVENT_UNAVAILABLE);
    atomic_store(&s_product.event_accepting, true);
    atomic_store(&fail_metadata_allocation, true);
    assert(xSemaphoreTake(s_product.event_lock, 0U) == pdTRUE);
    assert(offer_event(package_sha256, 1U, event, 1U) == ESP_BASE_CONTAINER_EVENT_BUSY);
    xSemaphoreGive(s_product.event_lock);
    assert(offer_event(wrong_sha256, 1U, event, 1U) == ESP_BASE_CONTAINER_EVENT_INVALID);
    assert(event_allocation_attempts == 0U && atomic_load(&fail_metadata_allocation) &&
           s_product.event_count == 0U && s_product.event_head == 0U);
    assert(offer_event(package_sha256, 1U, event, 1U) == ESP_BASE_CONTAINER_EVENT_NO_MEMORY);
    assert(event_allocation_attempts == 1U && s_product.event_count == 0U);
    assert(xSemaphoreTake(s_product.event_lock, 0U) == pdTRUE);
    xSemaphoreGive(s_product.event_lock);

    assert(offer_event(package_sha256, 1U, event, RUNTIME_EVENT_BYTES) ==
           ESP_BASE_CONTAINER_EVENT_ACCEPTED);
    product_event_t *processing = take_event();
    assert(processing != NULL && processing->size_bytes == RUNTIME_EVENT_BYTES &&
           !memcmp(processing->bytes, event, RUNTIME_EVENT_BYTES));
    memset(processing->bytes, 0, processing->size_bytes);
    test_product_free(processing);
    assert(finish_guest_work());

    for (uint64_t sequence = 2U; sequence < 10U; ++sequence)
        assert(offer_event(package_sha256, sequence, event, AUTHENTICATED_EVENT_BYTES) ==
               ESP_BASE_CONTAINER_EVENT_ACCEPTED);
    processing = take_event();
    assert(processing != NULL && s_product.guest_call_processing && s_product.event_count == 7U);
    assert(offer_event(package_sha256, 10U, event, AUTHENTICATED_EVENT_BYTES) ==
           ESP_BASE_CONTAINER_EVENT_ACCEPTED);
    product_event_t *queue_before[8];
    memcpy(queue_before, s_product.event_queue, sizeof queue_before);
    const size_t attempts_before = event_allocation_attempts;
    const size_t head_before = s_product.event_head;
    atomic_store(&fail_metadata_allocation, true);
    assert(offer_event(package_sha256, 11U, event, AUTHENTICATED_EVENT_BYTES) ==
           ESP_BASE_CONTAINER_EVENT_FULL);
    assert(offer_event(wrong_sha256, 11U, event, AUTHENTICATED_EVENT_BYTES) ==
           ESP_BASE_CONTAINER_EVENT_INVALID);
    assert(xSemaphoreTake(s_product.event_lock, 0U) == pdTRUE);
    assert(offer_event(package_sha256, 11U, event, AUTHENTICATED_EVENT_BYTES) ==
           ESP_BASE_CONTAINER_EVENT_BUSY);
    xSemaphoreGive(s_product.event_lock);
    assert(event_allocation_attempts == attempts_before && atomic_load(&fail_metadata_allocation) &&
           s_product.event_count == 8U && s_product.event_head == head_before &&
           s_product.guest_call_processing &&
           !memcmp(queue_before, s_product.event_queue, sizeof queue_before));
    for (size_t index = 0; index < 8U; ++index)
        assert(queue_before[index]->size_bytes == AUTHENTICATED_EVENT_BYTES &&
               !memcmp(queue_before[index]->bytes, event, AUTHENTICATED_EVENT_BYTES));
    memset(processing->bytes, 0, processing->size_bytes);
    test_product_free(processing);
    assert(finish_guest_work());
    release_event_queue();
    assert(event_release_count == event_allocation_count);

    prepare_event_admission_queue(package_sha256);
    atomic_store(&fail_metadata_allocation, false);
    stop_during_event_copy = true;
    const size_t releases_before = event_release_count;
    assert(offer_event(package_sha256, 12U, event, AUTHENTICATED_EVENT_BYTES) ==
           ESP_BASE_CONTAINER_EVENT_UNAVAILABLE);
    assert(!stop_during_event_copy && atomic_load(&s_product.stop_requested) &&
           event_allocation_attempts == attempts_before + 1U &&
           event_release_count == releases_before + 1U && s_product.event_count == 0U);
    assert(xSemaphoreTake(s_product.event_lock, 0U) == pdTRUE);
    xSemaphoreGive(s_product.event_lock);
    assert(offer_event(package_sha256, 12U, event, 1U) == ESP_BASE_CONTAINER_EVENT_UNAVAILABLE);
    release_event_queue();
    prepare_event_admission_queue(wrong_sha256); /* A same-boot package switch. */
    assert(offer_event(package_sha256, 12U, event, 1U) == ESP_BASE_CONTAINER_EVENT_INVALID);
    assert(event_allocation_attempts == attempts_before + 1U && s_product.event_count == 0U);
    release_event_queue();
    assert(event_release_count == event_allocation_count);
    observe_event_allocations = false;
    event_copy_target = NULL;
    dispose_product();
    puts("container_event_admission: zero-allocation rejection, full eight plus processing, maximum payload, OOM unlock, copy cancellation and package isolation passed");
}

static void configure(const file_t *public_key)
{
    memset(&store, 0, sizeof store);
    reject_confirmed_observation = false;
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
    const int32_t expected_event_result =
        context == NULL ? 3 : *(const int32_t *)context;
    econtainer_slots_state_t state = {0};
    econtainer_slots_result_t loaded = econtainer_slots_load(&io, &geometry, &state);
    if (loaded != ECONTAINER_SLOTS_OK) return loaded;
    econtainer_slot_selection_request_t request = {
        .expected_sequence = state.sequence, .firmware_set = *firmware_set,
        .selection = ECONTAINER_SLOT_SELECT_CONFIRMED,
    };
    econtainer_runtime_t *runtime = NULL;
    /* This direct instance runs on the caller, outside the stopped Base owner. */
    econtainer_runtime_limits_t limits = s_product.limits;
    limits.cancel_requested = NULL;
    limits.cancel_context = NULL;
    const econtainer_slot_runtime_result_t opened = econtainer_product_open(
        &io, &geometry, &request, &s_product.validation, &limits, &runtime);
    if (opened.slots != ECONTAINER_SLOTS_OK) return opened.slots;
    assert(opened.runtime == ECONTAINER_RUNTIME_OK && runtime != NULL);
    assert(econtainer_product_init(runtime) == ECONTAINER_RUNTIME_OK);
    const uint8_t event[] = {1, 2, 3};
    int32_t guest_result = -1;
    assert(econtainer_product_on_event(runtime, event, sizeof event, &guest_result) ==
           ECONTAINER_RUNTIME_OK);
    assert(guest_result == expected_event_result);
    assert(econtainer_product_stop(runtime) == ECONTAINER_RUNTIME_OK);
    assert(econtainer_product_close(&runtime) == ECONTAINER_RUNTIME_OK && runtime == NULL);
    return ECONTAINER_SLOTS_OK;
}

static econtainer_slots_result_t open_expired_init(
    const econtainer_slot_firmware_set_t *firmware_set, void *context)
{
    (void)context;
    econtainer_slots_state_t state = {0};
    if (econtainer_slots_load(&io, &geometry, &state) != ECONTAINER_SLOTS_OK)
        return ECONTAINER_SLOTS_IO_FAILED;
    const econtainer_slot_selection_request_t request = {
        .expected_sequence = state.sequence, .firmware_set = *firmware_set,
        .selection = ECONTAINER_SLOT_SELECT_CONFIRMED,
    };
    econtainer_runtime_t *runtime = NULL;
    /* This direct instance runs on the caller, outside the stopped Base owner. */
    econtainer_runtime_limits_t limits = s_product.limits;
    limits.cancel_requested = NULL;
    limits.cancel_context = NULL;
    const econtainer_slot_runtime_result_t opened = econtainer_product_open(
        &io, &geometry, &request, &s_product.validation, &limits, &runtime);
    if (opened.slots != ECONTAINER_SLOTS_OK) return opened.slots;
    assert(opened.runtime == ECONTAINER_RUNTIME_OK && runtime != NULL);
    /* The signed ABI 2 guest logs, starts one timer, then spins in pure Wasm. */
    assert(econtainer_product_init(runtime) == ECONTAINER_RUNTIME_ENTRY_EXPIRED);
    uint8_t log[16] = {0};
    size_t log_size = 99;
    assert(econtainer_product_take_log(runtime, log, sizeof log, &log_size) ==
           ECONTAINER_RUNTIME_NO_LOG && log_size == 99);
    uint64_t deadline_ms = 0;
    econtainer_timer_event_t timer_event = {0};
    int32_t guest_result = 123;
    assert(econtainer_product_next_timer_deadline(runtime, &deadline_ms) ==
           ECONTAINER_RUNTIME_INVALID_STATE);
    assert(econtainer_product_poll_timer(runtime, &timer_event, &guest_result) ==
           ECONTAINER_RUNTIME_INVALID_STATE && guest_result == 123);
    assert(econtainer_product_stop(runtime) == ECONTAINER_RUNTIME_INVALID_STATE);
    assert(econtainer_product_close(&runtime) == ECONTAINER_RUNTIME_OK && runtime == NULL);
    return ECONTAINER_SLOTS_OK;
}

static atomic_bool cancel_entry_observed;

static bool observe_product_cancellation(void *context)
{
    atomic_store_explicit(&cancel_entry_observed, true, memory_order_release);
    return product_cancel_requested(context);
}

static void cancellation_policy(void)
{
    s_product.validation.max_instruction_budget = 100000000U;
    s_product.validation.max_host_call_timeout_ms = 500U;
    s_product.limits.init_instruction_budget = 100000000;
    s_product.limits.event_instruction_budget = 100000000;
    s_product.limits.stop_instruction_budget = 100000000;
    s_product.limits.max_entry_duration_ms = 500U;
    s_product.limits.cancel_requested = observe_product_cancellation;
    atomic_store(&cancel_entry_observed, false);
}

static void wait_cancel_entry(void)
{
    for (unsigned attempt = 0; attempt < 400U; ++attempt) {
        if (atomic_load_explicit(&cancel_entry_observed, memory_order_acquire)) return;
        vTaskDelay(1U);
    }
    assert(!"the actual runtime did not query its owner cancellation predicate");
}

static void *request_init_cancellation(void *context)
{
    atomic_bool *requested = context;
    wait_cancel_entry();
    const struct timespec delay = {.tv_nsec = 10000000};
    assert(nanosleep(&delay, NULL) == 0);
    atomic_store_explicit(requested, true, memory_order_release);
    return NULL;
}

static void run_cancel_product(const char *directory, const char *package_name,
                                bool cancel_init, bool cancel_timer,
                                bool stop_succeeds)
{
    file_t key = read_file(directory, "public.der");
    file_t package = read_file(directory, package_name);
    const char boot_id[] = "22222222-2222-4222-8222-222222222222";
    configure(&key);
    cancellation_policy();
    esp_base_storage_owner_t owner;
    esp_base_storage_owner_init(&owner);
    esp_base_storage_claim_t claim = {0};
    assert(esp_base_storage_claim(&owner, &claim));
    assert(esp_base_container_product_boot(&claim, boot_id) == ESP_BASE_CONTAINER_EMPTY);
    install_context_t install = {.package = &package};
    assert(esp_base_container_with_firmware_set(&claim, ESP_BASE_OTA_FIRMWARE_CONFIRMED,
        NULL, install_signed, &install) == ECONTAINER_SLOTS_OK);
    econtainer_slots_state_t before = {0}, after = {0};
    assert(econtainer_slots_load(&io, &geometry, &before) == ECONTAINER_SLOTS_OK);
    const int current = binding_index(&before, physical.running_firmware_sha256);
    assert(current >= 0 && before.bindings[current].package_present);
    const unsigned writes_before = store.blob_writes;
    const unsigned flash_before = store.flash_writes;
    const unsigned erases_before = store.flash_erases;
    atomic_store(&cancel_entry_observed, false);
    if (cancel_init) {
        pthread_t requester;
        assert(pthread_create(&requester, NULL, request_init_cancellation,
                              &s_product.stop_requested) == 0);
        assert(esp_base_container_product_boot(&claim, boot_id) ==
               ESP_BASE_CONTAINER_STOPPED);
        assert(pthread_join(requester, NULL) == 0);
        assert(!s_product.reopen_allowed);
    } else {
        assert(esp_base_container_product_boot(&claim, boot_id) ==
               ESP_BASE_CONTAINER_RUNNING);
        atomic_store(&cancel_entry_observed, false);
        const uint8_t event[] = {cancel_timer ? 'T' : 'E'};
        assert(offer_event_when_available(before.bindings[current].package_sha256,
            1U, event, sizeof event) == ESP_BASE_CONTAINER_EVENT_ACCEPTED);
        if (cancel_timer) {
            for (unsigned attempt = 0; attempt < 400U &&
                 esp_base_container_product_event_progress_count() == 0U; ++attempt)
                vTaskDelay(1U);
            assert(esp_base_container_product_event_progress_count() == 1U);
            atomic_store(&cancel_entry_observed, false);
        }
        wait_cancel_entry();
        const struct timespec delay = {.tv_nsec = 10000000};
        assert(nanosleep(&delay, NULL) == 0);
        assert(atomic_load_explicit(&s_product.result, memory_order_acquire) == ESP_BASE_CONTAINER_RUNNING);
        const int64_t began_us = esp_timer_get_time();
        assert(esp_base_container_product_stop_confirmed(&claim) == stop_succeeds);
        const int64_t elapsed_us = esp_timer_get_time() - began_us;
        if (stop_succeeds) assert(elapsed_us >= 0 && elapsed_us < 250000);
        assert(esp_base_container_product_event_progress_count() ==
               (cancel_timer ? 1U : 0U));
        assert(s_product.trial_failure_count == 0U);
        if (stop_succeeds) {
            assert(s_product.reopen_allowed);
            assert(esp_base_container_product_boot(&claim, boot_id) ==
                   ESP_BASE_CONTAINER_RUNNING);
            assert(esp_base_container_product_stop_confirmed(&claim));
        } else {
            assert(!s_product.reopen_allowed && !s_product.native_reclaimed);
            assert(esp_base_container_product_set_running(&claim, true, boot_id,
                before.sequence, before.bindings[current].package_sha256) ==
                ESP_BASE_CONTAINER_RUN_REJECTED);
            assert(atomic_load_explicit(&s_product.result, memory_order_acquire) == ESP_BASE_CONTAINER_BLOCKED);
            assert(esp_base_container_product_boot(&claim, boot_id) ==
                   ESP_BASE_CONTAINER_BLOCKED);
        }
    }
    assert(!s_product.thread_joinable && !atomic_load(&s_product.instance_active));
    assert(s_product.event_queue == NULL && s_product.event_count == 0U &&
           !s_product.guest_call_processing && store.mapping == NULL && !store.locked);
    if (stop_succeeds)
        assert(s_product.native_reclaimed && s_product.stop_succeeded &&
               atomic_load_explicit(&s_product.result, memory_order_acquire) == ESP_BASE_CONTAINER_STOPPED);
    assert(store.blob_writes == writes_before && store.flash_writes == flash_before &&
           store.flash_erases == erases_before);
    assert(econtainer_slots_load(&io, &geometry, &after) == ECONTAINER_SLOTS_OK &&
           before.sequence == after.sequence &&
           same_binding(&before.bindings[current], &after.bindings[current]));
    assert(esp_base_storage_release(&claim));
    dispose_product();
    assert(pthread_mutex_destroy(&store.mutex) == 0);
    free(key.bytes);
    free(package.bytes);
}

static void run_deadline_product(const char *directory)
{
    file_t key = read_file(directory, "public.der");
    file_t deadline_package = read_file(directory, "deadline.pkg");
    file_t normal_package = read_file(directory, "normal.pkg");
    const char boot_id[] = "22222222-2222-4222-8222-222222222222";
    esp_base_storage_owner_t owner;
    esp_base_storage_claim_t claim = {0};

    configure(&key);
    s_product.validation.max_instruction_budget = 100000000U;
    s_product.limits.init_instruction_budget = 100000000;
    s_product.limits.max_entry_duration_ms = 20;
    esp_base_storage_owner_init(&owner);
    assert(esp_base_storage_claim(&owner, &claim));
    assert(esp_base_container_product_boot(&claim, boot_id) == ESP_BASE_CONTAINER_EMPTY);
    install_context_t install = {.package = &deadline_package};
    assert(esp_base_container_with_firmware_set(&claim, ESP_BASE_OTA_FIRMWARE_CONFIRMED,
        NULL, install_signed, &install) == ECONTAINER_SLOTS_OK);
    assert(esp_base_container_with_firmware_set(&claim, ESP_BASE_OTA_FIRMWARE_CONFIRMED,
        NULL, open_expired_init, NULL) == ECONTAINER_SLOTS_OK);
    const unsigned writes_before = store.blob_writes;
    const unsigned flash_before = store.flash_writes;
    const unsigned erases_before = store.flash_erases;
    assert(esp_base_container_product_boot(&claim, boot_id) == ESP_BASE_CONTAINER_BLOCKED);
    assert(s_product.native_reclaimed && !s_product.thread_joinable &&
           !s_product.reopen_allowed && !store.locked && store.mapping == NULL);
    assert(store.blob_writes == writes_before && store.flash_writes == flash_before &&
           store.flash_erases == erases_before);
    assert(esp_base_container_product_boot(&claim, boot_id) == ESP_BASE_CONTAINER_BLOCKED);
    assert(esp_base_storage_release(&claim));
    dispose_product();
    assert(pthread_mutex_destroy(&store.mutex) == 0);

    /* A fresh boot fixture in this process must be able to open the normal guest. */
    configure(&key);
    esp_base_storage_owner_init(&owner);
    claim = (esp_base_storage_claim_t){0};
    assert(esp_base_storage_claim(&owner, &claim));
    assert(esp_base_container_product_boot(&claim, boot_id) == ESP_BASE_CONTAINER_EMPTY);
    install_context_t normal_install = {.package = &normal_package};
    assert(esp_base_container_with_firmware_set(&claim, ESP_BASE_OTA_FIRMWARE_CONFIRMED,
        NULL, install_signed, &normal_install) == ECONTAINER_SLOTS_OK);
    assert(esp_base_container_product_boot(&claim, boot_id) == ESP_BASE_CONTAINER_RUNNING);
    assert(esp_base_container_product_stop_confirmed(&claim));
    assert(esp_base_storage_release(&claim));
    dispose_product();
    assert(pthread_mutex_destroy(&store.mutex) == 0);
    free(key.bytes);
    free(deadline_package.bytes);
    free(normal_package.bytes);
    puts("container_product_deadline: signed ABI 2 expiry, cleanup, Base blocked boot and fresh-boot reopen passed");
}

static void run_event_failure_trial(const char *directory)
{
    file_t key = read_file(directory, "public.der");
    file_t normal_package = read_file(directory, "normal.pkg");
    file_t failed_package = read_file(directory, "event-loop.pkg");
    const char boot_id[] = "22222222-2222-4222-8222-222222222222";
    configure(&key);
    esp_base_storage_owner_t owner;
    esp_base_storage_owner_init(&owner);
    esp_base_storage_claim_t claim = {0};
    assert(esp_base_storage_claim(&owner, &claim));
    assert(esp_base_container_product_boot(&claim, boot_id) ==
           ESP_BASE_CONTAINER_EMPTY);
    install_context_t install = {.package = &normal_package};
    assert(esp_base_container_with_firmware_set(&claim,
        ESP_BASE_OTA_FIRMWARE_CONFIRMED, NULL, install_signed, &install) ==
        ECONTAINER_SLOTS_OK);
    assert(esp_base_container_product_boot(&claim, boot_id) ==
           ESP_BASE_CONTAINER_RUNNING);
    esp_base_container_binding_snapshot_t old = {0};
    assert(esp_base_container_product_binding_snapshot(&claim, &old) ==
           ESP_BASE_CONTAINER_BINDING_OK && old.package_present);
    const unsigned snapshot_blob_writes = store.blob_writes;
    const unsigned snapshot_flash_writes = store.flash_writes;
    esp_base_ota_receipt_snapshot_t ota_snapshot = {0};
    esp_base_ota_request_t ota_request = {
        .package_mode = ESP_BASE_OTA_PACKAGE_REUSE,
        .package_size_bytes = (uint32_t)normal_package.size,
        .guest_abi_version = 2U,
        .data_schema_version = 1U,
    };
    memcpy(ota_request.package_sha256, old.package_sha256, 32);
    memset(ota_request.trial_event_sha256, 0xe5, 32);
    assert(esp_base_container_product_snapshot_for_ota(
        &claim, &ota_request, &ota_snapshot));
    assert(ota_snapshot.container_sequence == old.container_sequence &&
           ota_snapshot.source_package_present &&
           ota_snapshot.source_package_size_bytes == (uint32_t)normal_package.size &&
           ota_snapshot.source_guest_abi_version == 2U &&
           ota_snapshot.source_data_schema_version == 1U &&
           memcmp(ota_snapshot.source_package_sha256, old.package_sha256, 32) == 0);
    ota_request.package_mode = ESP_BASE_OTA_PACKAGE_WRITE;
    assert(esp_base_container_product_snapshot_for_ota(
        &claim, &ota_request, &ota_snapshot));
    ota_request = (esp_base_ota_request_t){0};
    assert(!esp_base_container_product_snapshot_for_ota(
        &claim, &ota_request, &ota_snapshot));
    assert(store.blob_writes == snapshot_blob_writes &&
           store.flash_writes == snapshot_flash_writes);
    esp_base_container_package_request_t request = {
        .operation_id = "99999999-9999-4999-8999-999999999999",
        .expected_sequence = old.container_sequence,
        .previous_package_present = true,
        .package_size_bytes = (uint32_t)failed_package.size,
        .guest_abi_version = 2U, .data_schema_version = 1U,
    };
    memcpy(request.previous_package_sha256, old.package_sha256, 32);
    assert(SHA256(failed_package.bytes, failed_package.size,
                  request.package_sha256) != NULL);
    uint32_t prepared_sequence = 0U;
    assert(esp_base_container_product_prepare_package(&claim, &request,
        read_source, &failed_package, &prepared_sequence) ==
        ESP_BASE_CONTAINER_PREPARED);
    assert(esp_base_container_product_stop_confirmed(&claim));
    assert(esp_base_container_product_start_package_trial(&claim,
        prepared_sequence, request.operation_id, boot_id, trial_event_digest) ==
        ESP_BASE_CONTAINER_RUNNING);
    const uint8_t event[] = {1U};
    assert(offer_event_when_available(request.package_sha256,
        1U, event, sizeof event) == ESP_BASE_CONTAINER_EVENT_ACCEPTED);
    for (unsigned attempt = 0;
         attempt < 500U && esp_base_container_product_event_accepting();
         ++attempt) vTaskDelay(1U);
    assert(!esp_base_container_product_event_accepting());
    esp_base_container_event_observation_t observation = {0};
    assert(esp_base_container_product_event_observation(&observation) ==
           ESP_BASE_CONTAINER_EVENT_OBSERVED &&
           observation.event_sequence == 1U && !observation.runtime_ok);
    const uint32_t trial_sequence = prepared_sequence + 1U;
    const unsigned writes_before_wrong = store.blob_writes;
    assert(!esp_base_container_product_abandon_package_trial(&claim,
        trial_sequence - 1U, request.operation_id));
    assert(store.blob_writes == writes_before_wrong);
    const bool abandoned = esp_base_container_product_abandon_package_trial(
        &claim, trial_sequence, request.operation_id);
    if (!abandoned) fprintf(stderr,
        "event failure state: joined=%d reclaimed=%d stop=%d result=%d active=%d\n",
        !s_product.thread_joinable, s_product.native_reclaimed,
        s_product.stop_succeeded, atomic_load(&s_product.result),
        atomic_load(&s_product.instance_active));
    assert(abandoned);
    esp_base_container_binding_snapshot_t restored = {0};
    assert(esp_base_container_product_binding_snapshot(&claim, &restored) ==
           ESP_BASE_CONTAINER_BINDING_OK &&
           restored.container_sequence == trial_sequence + 1U &&
           restored.package_present &&
           memcmp(restored.package_sha256, old.package_sha256, 32) == 0);
    assert(esp_base_container_product_boot(&claim, boot_id) ==
           ESP_BASE_CONTAINER_RUNNING);
    assert(esp_base_container_product_stop_confirmed(&claim));
    assert(esp_base_storage_release(&claim));
    dispose_product();
    assert(pthread_mutex_destroy(&store.mutex) == 0);
    free(key.bytes);
    free(normal_package.bytes);
    free(failed_package.bytes);
    puts("container_product_event_failure: signed guest trap, ABORTED and old guest reopen passed");
}

static void run_confirmed_event_failure(const char *directory, bool prepare_first)
{
    file_t key = read_file(directory, "public.der");
    file_t failed_package = read_file(directory, "event-loop.pkg");
    file_t candidate_package = read_file(directory, "normal.pkg");
    const char boot_id[] = "22222222-2222-4222-8222-222222222222";
    configure(&key);
    esp_base_storage_owner_t owner;
    esp_base_storage_owner_init(&owner);
    esp_base_storage_claim_t claim = {0};
    assert(esp_base_storage_claim(&owner, &claim));
    assert(esp_base_container_product_boot(&claim, boot_id) == ESP_BASE_CONTAINER_EMPTY);
    /* Seed a confirmed binding, then let the real signed guest fail on an
     * event. This is not a public install or trial-health fixture. */
    install_context_t install = {.package = &failed_package};
    assert(esp_base_container_with_firmware_set(&claim,
        ESP_BASE_OTA_FIRMWARE_CONFIRMED, NULL, install_signed, &install) ==
        ECONTAINER_SLOTS_OK);
    assert(esp_base_container_product_boot(&claim, boot_id) == ESP_BASE_CONTAINER_RUNNING);
    esp_base_container_binding_snapshot_t before = {0};
    assert(esp_base_container_product_binding_snapshot(&claim, &before) ==
           ESP_BASE_CONTAINER_BINDING_OK && before.package_present);
    esp_base_container_package_request_t request = {
        .operation_id = "99999999-9999-4999-8999-999999999999",
        .expected_sequence = before.container_sequence,
        .previous_package_present = true,
        .package_size_bytes = (uint32_t)candidate_package.size,
        .guest_abi_version = 2U, .data_schema_version = 1U,
    };
    memcpy(request.previous_package_sha256, before.package_sha256, 32);
    assert(SHA256(candidate_package.bytes, candidate_package.size,
                  request.package_sha256) != NULL);
    uint32_t prepared_sequence = 0U;
    if (prepare_first) {
        assert(esp_base_container_product_prepare_package(&claim, &request,
            read_source, &candidate_package, &prepared_sequence) ==
            ESP_BASE_CONTAINER_PREPARED);
    }
    const unsigned blob_writes = store.blob_writes;
    const unsigned flash_writes = store.flash_writes;
    const unsigned flash_erases = store.flash_erases;
    const uint8_t event[] = {1U};
    assert(offer_event_when_available(before.package_sha256, 1U, event, sizeof event) ==
           ESP_BASE_CONTAINER_EVENT_ACCEPTED);
    for (unsigned attempt = 0; attempt < 1000U &&
         atomic_load_explicit(&s_product.result, memory_order_acquire) !=
             ESP_BASE_CONTAINER_BLOCKED; ++attempt) vTaskDelay(1U);
    assert(atomic_load_explicit(&s_product.result, memory_order_acquire) ==
           ESP_BASE_CONTAINER_BLOCKED);
    assert(!esp_base_container_product_event_accepting());
    esp_base_container_event_observation_t observation = {0};
    assert(esp_base_container_product_event_observation(&observation) ==
           ESP_BASE_CONTAINER_EVENT_OBSERVED && observation.event_sequence == 1U &&
           !observation.runtime_ok);
    assert(!esp_base_container_product_stop_confirmed(&claim));
    assert(esp_base_container_product_boot(&claim, boot_id) == ESP_BASE_CONTAINER_BLOCKED);
    assert(esp_base_container_product_set_running(&claim, true, boot_id,
        before.container_sequence, before.package_sha256) == ESP_BASE_CONTAINER_RUN_REJECTED);
    uint32_t resolved_sequence = before.container_sequence;
    if (prepare_first) {
        /* A trap between prepare and stop must never start the candidate.
         * The public worker reports unknown and retains its claim here; this
         * explicit internal abandonment only verifies the storage primitive. */
        assert(esp_base_container_product_start_package_trial(&claim,
            prepared_sequence, request.operation_id, boot_id, trial_event_digest) ==
            ESP_BASE_CONTAINER_BLOCKED);
    } else {
        prepared_sequence = UINT32_MAX;
        assert(esp_base_container_product_prepare_package(&claim, &request,
            read_source, &candidate_package, &prepared_sequence) ==
            ESP_BASE_CONTAINER_PREPARE_REJECTED && prepared_sequence == 0U);
    }
    assert(store.blob_writes == blob_writes && store.flash_writes == flash_writes &&
           store.flash_erases == flash_erases);
    if (prepare_first) {
        assert(esp_base_container_product_abandon_prepared_package(&claim,
            prepared_sequence, request.operation_id, request.package_sha256,
            &resolved_sequence) && resolved_sequence == prepared_sequence + 1U);
    }
    esp_base_container_binding_snapshot_t unchanged = {0};
    esp_base_container_active_product_t active = {0};
    assert(esp_base_container_product_status_snapshot(&claim, &unchanged, &active) ==
           ESP_BASE_CONTAINER_BINDING_OK && !active.present &&
           unchanged.container_sequence == resolved_sequence && unchanged.package_present &&
           memcmp(unchanged.package_sha256, before.package_sha256, 32) == 0);
    uint8_t *flash_before = malloc(sizeof store.flash);
    assert(flash_before != NULL);
    memcpy(flash_before, store.flash, sizeof store.flash);
    const char uninstall_id[] = "88888888-8888-4888-8888-888888888888";
    assert(esp_base_container_product_uninstall(&claim, uninstall_id,
        resolved_sequence, before.package_sha256) == ESP_BASE_CONTAINER_UNINSTALL_COMPLETE);
    assert(!s_product.thread_joinable && s_product.native_reclaimed &&
           !atomic_load_explicit(&s_product.instance_active, memory_order_acquire));
    assert(memcmp(flash_before, store.flash, sizeof store.flash) == 0);
    free(flash_before);
    assert(esp_base_container_product_boot(&claim, boot_id) == ESP_BASE_CONTAINER_EMPTY);
    assert(esp_base_storage_release(&claim));
    dispose_product();
    assert(pthread_mutex_destroy(&store.mutex) == 0);
    free(candidate_package.bytes);
    free(failed_package.bytes);
    free(key.bytes);
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
    esp_base_container_binding_snapshot_t binding = {0};
    assert(esp_base_container_product_binding_snapshot(&claim, &binding) ==
           ESP_BASE_CONTAINER_BINDING_OK && binding.container_sequence == 1U &&
           !binding.package_present && digest_zero(binding.package_sha256) &&
           memcmp(binding.firmware_sha256, physical.running_firmware_sha256, 32) == 0 &&
           binding.runtime_guest_abi_version == ECONTAINER_GUEST_ABI_VERSION &&
           binding.package_guest_abi_version == 0U && binding.package_data_schema_version == 0U);
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
    assert(esp_base_container_product_binding_snapshot(&claim, &binding) ==
           ESP_BASE_CONTAINER_BINDING_OK &&
           binding.container_sequence == before.sequence && binding.package_present &&
           memcmp(binding.package_sha256,
                  before.bindings[running_index].package_sha256, 32) == 0 &&
           memcmp(binding.firmware_sha256, physical.running_firmware_sha256, 32) == 0 &&
           binding.package_guest_abi_version == before.bindings[running_index].guest_abi_version &&
           binding.package_data_schema_version == before.bindings[running_index].data_schema_version);
    uint8_t flash_before[FLASH_BYTES];
    memcpy(flash_before, store.flash, sizeof flash_before);
    const unsigned writes_before = store.blob_writes;
    const unsigned erases_before = store.flash_erases;
    const unsigned flash_writes_before = store.flash_writes;
    const char uninstall_id[] = "33333333-3333-4333-8333-333333333333";
    assert(esp_base_container_product_reconcile_uninstall(&claim, uninstall_id,
        before.sequence, before.bindings[running_index].package_sha256) ==
        ESP_BASE_CONTAINER_UNINSTALL_NOT_COMMITTED);
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
    assert(esp_base_container_product_reconcile_uninstall(&claim, uninstall_id,
        before.sequence, before.bindings[running_index].package_sha256) ==
        ESP_BASE_CONTAINER_UNINSTALL_RECOVERED);
    assert(esp_base_container_product_reconcile_uninstall(&claim, uninstall_id,
        before.sequence - 1U, before.bindings[running_index].package_sha256) ==
        ESP_BASE_CONTAINER_UNINSTALL_RECOVERY_UNCERTAIN);
    assert(esp_base_container_product_binding_snapshot(&claim, &binding) ==
           ESP_BASE_CONTAINER_BINDING_OK &&
           binding.container_sequence == after.sequence &&
           !binding.package_present && digest_zero(binding.package_sha256));
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
               !atomic_load(&s_product.instance_active) && s_product.product_version == NULL);
        esp_base_container_binding_snapshot_t empty_binding = {0};
        esp_base_container_active_product_t inactive = {0};
        assert(esp_base_container_product_status_snapshot(&claim, &empty_binding, &inactive) ==
               ESP_BASE_CONTAINER_BINDING_OK && !inactive.present && inactive.product_version == NULL);
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

static void check_active_product(const esp_base_storage_claim_t *claim,
                                 const file_t *version, bool trial, const char *operation_id)
{
    esp_base_container_binding_snapshot_t binding = {0};
    esp_base_container_active_product_t active = {0};
    assert(esp_base_container_product_status_snapshot(claim, &binding, &active) ==
           ESP_BASE_CONTAINER_BINDING_OK);
    assert(active.present && !strcmp(active.product_id, "counter") &&
           active.product_id_size_bytes == sizeof("counter") - 1U &&
           active.product_version_size_bytes == version->size &&
           !memcmp(active.product_version, version->bytes, version->size) &&
           active.product_version[version->size] == '\0' && active.is_trial == trial &&
           active.guest_abi_version == 2U && active.data_schema_version == 1U &&
           memcmp(active.package_sha256, s_product.event_package_sha256, 32) == 0);
    assert(trial ? !strcmp(active.operation_id, operation_id) : active.operation_id[0] == '\0');
    if (!trial) assert(binding.package_present && !memcmp(active.package_sha256, binding.package_sha256, 32));
    free(active.product_version);
}

static void run_signed_reinstall_cycles(const file_t *key, const file_t *package,
                                        const char boot_id[37])
{
    configure(key);
    esp_base_storage_owner_t owner;
    esp_base_storage_owner_init(&owner);
    esp_base_storage_claim_t claim = {0};
    assert(esp_base_storage_claim(&owner, &claim));
    assert(esp_base_container_product_boot(&claim, boot_id) == ESP_BASE_CONTAINER_EMPTY);
    econtainer_slots_state_t initial = {0};
    assert(econtainer_slots_load(&io, &geometry, &initial) == ECONTAINER_SLOTS_OK);
    uint32_t last_sequence = initial.sequence;
#ifdef ESP_BASE_TEST_RESOURCE_STATS
    assert(resource_probes_calibrated());
    resource_stats_t after_ten = {0}, after_fifty = {0}, after_hundred = {0};
#endif
    for (unsigned cycle = 0; cycle < 100U; ++cycle) {
        install_context_t install = {
            .package = package, .operation_marker = (uint8_t)(0x90U + cycle)};
        assert(esp_base_container_with_firmware_set(&claim,
            ESP_BASE_OTA_FIRMWARE_CONFIRMED, NULL, install_signed,
            &install) == ECONTAINER_SLOTS_OK);
        econtainer_slots_state_t before = {0}, after = {0};
        assert(econtainer_slots_load(&io, &geometry, &before) == ECONTAINER_SLOTS_OK);
        assert(before.sequence == last_sequence + 5U);
        const int index = binding_index(&before, physical.running_firmware_sha256);
        assert(index >= 0 && before.bindings[index].package_present);
        assert(esp_base_container_product_boot(&claim, boot_id) == ESP_BASE_CONTAINER_RUNNING);
        assert(s_product.thread_joinable &&
               atomic_load(&s_product.instance_active));
        if (cycle == 0U) {
            esp_base_container_binding_snapshot_t failed_binding = {0};
            esp_base_container_active_product_t failed_active = {0};
            atomic_store(&fail_metadata_allocation, true);
            assert(esp_base_container_product_status_snapshot(&claim, &failed_binding, &failed_active) ==
                   ESP_BASE_CONTAINER_BINDING_RESOURCE_FAILURE && !failed_active.present &&
                   failed_active.product_version == NULL && failed_binding.container_sequence == 0U &&
                   atomic_load(&s_product.instance_active));
        }
        check_active_product(&claim, &expected_confirmed_version, false, NULL);

        uint8_t flash_before[FLASH_BYTES];
        memcpy(flash_before, store.flash, sizeof flash_before);
        char uninstall_id[37];
        assert(snprintf(uninstall_id, sizeof uninstall_id,
                        "90100000-0000-4000-8000-%012x", cycle + 1U) == 36);
        const unsigned erases = store.flash_erases;
        const unsigned writes = store.flash_writes;
        assert(esp_base_container_product_uninstall(&claim, uninstall_id,
            before.sequence, before.bindings[index].package_sha256) ==
            ESP_BASE_CONTAINER_UNINSTALL_COMPLETE);
        assert(!s_product.thread_joinable && s_product.native_reclaimed &&
               !atomic_load(&s_product.instance_active));
        assert(econtainer_slots_load(&io, &geometry, &after) == ECONTAINER_SLOTS_OK);
        assert(after.sequence == before.sequence + 1U &&
               !after.bindings[index].package_present);
        last_sequence = after.sequence;
        assert(store.flash_erases == erases && store.flash_writes == writes &&
               memcmp(flash_before, store.flash, sizeof flash_before) == 0);
        assert(esp_base_container_product_boot(&claim, boot_id) == ESP_BASE_CONTAINER_EMPTY);
        assert(!s_product.thread_joinable && store.mapping == NULL && !store.locked);
#ifdef ESP_BASE_TEST_RESOURCE_STATS
        if (cycle == 9U) assert(sample_resources(&after_ten));
        if (cycle == 49U) assert(sample_resources(&after_fifty));
        if (cycle == 99U) assert(sample_resources(&after_hundred));
#endif
    }
    assert(last_sequence == initial.sequence + 600U);
    fprintf(stderr, "Base product sequence 100 cycles: %u -> %u\n",
            (unsigned)initial.sequence, (unsigned)last_sequence);
#ifdef ESP_BASE_TEST_RESOURCE_STATS
    assert(resources_stable(after_ten, after_fifty, after_hundred));
#endif
    assert(esp_base_storage_release(&claim));
    dispose_product();
    assert(pthread_mutex_destroy(&store.mutex) == 0);
}

static void run_source_change_same_boot(const file_t *key, const file_t *first,
                                        const file_t *second, const char boot_id[37])
{
    assert(first->size != second->size ||
           memcmp(first->bytes, second->bytes, first->size) != 0);
    configure(key);
    esp_base_storage_owner_t owner;
    esp_base_storage_owner_init(&owner);
    esp_base_storage_claim_t claim = {0};
    assert(esp_base_storage_claim(&owner, &claim));
    assert(esp_base_container_product_boot(&claim, boot_id) == ESP_BASE_CONTAINER_EMPTY);
    const esp_base_ota_firmware_set_t original_firmware = physical;

    install_context_t v1 = {.package = first, .operation_marker = 0x31};
    assert(esp_base_container_with_firmware_set(&claim,
        ESP_BASE_OTA_FIRMWARE_CONFIRMED, NULL, install_signed, &v1) ==
        ECONTAINER_SLOTS_OK);
    econtainer_slots_state_t first_state = {0};
    assert(econtainer_slots_load(&io, &geometry, &first_state) == ECONTAINER_SLOTS_OK);
    assert(esp_base_container_product_boot(&claim, boot_id) == ESP_BASE_CONTAINER_RUNNING);
    check_active_product(&claim, &expected_candidate_version, false, NULL);
    const int first_index = binding_index(&first_state,
        physical.running_firmware_sha256);
    assert(first_index >= 0 && esp_base_container_product_event_accepting());
    const uint8_t event[] = {1, 2, 3};
    uint8_t wrong_sha256[32];
    esp_base_container_event_observation_t observed_event = {0};
    assert(esp_base_container_product_event_observation(&observed_event) ==
           ESP_BASE_CONTAINER_EVENT_NO_OBSERVATION);
    memcpy(wrong_sha256, first_state.bindings[first_index].package_sha256, 32);
    wrong_sha256[0] ^= 0xff;
    assert(offer_event(wrong_sha256, 1U, event, sizeof event) ==
           ESP_BASE_CONTAINER_EVENT_INVALID);
    assert(offer_event(
        first_state.bindings[first_index].package_sha256, 0U, event, sizeof event) ==
        ESP_BASE_CONTAINER_EVENT_INVALID);
    assert(offer_event(
        first_state.bindings[first_index].package_sha256, 1U, event, 0U) ==
        ESP_BASE_CONTAINER_EVENT_INVALID);
    assert(offer_event_when_available(
        first_state.bindings[first_index].package_sha256, 1U, event, sizeof event) == ESP_BASE_CONTAINER_EVENT_ACCEPTED);
    for (unsigned attempt = 0;
         attempt < 200U && esp_base_container_product_event_progress_count() == 0U;
         ++attempt) vTaskDelay(1U);
    assert(esp_base_container_product_event_progress_count() == 1U);
    esp_base_container_event_observation_result_t observed_state =
        ESP_BASE_CONTAINER_EVENT_OBSERVATION_BUSY;
    for (unsigned attempt = 0;
         attempt < 200U && observed_state == ESP_BASE_CONTAINER_EVENT_OBSERVATION_BUSY;
         ++attempt) {
        observed_state = esp_base_container_product_event_observation(&observed_event);
        if (observed_state == ESP_BASE_CONTAINER_EVENT_OBSERVATION_BUSY) vTaskDelay(1U);
    }
    assert(observed_state == ESP_BASE_CONTAINER_EVENT_OBSERVED &&
           observed_event.event_sequence == 1U &&
           observed_event.runtime_ok && observed_event.guest_result == 3 &&
           !memcmp(observed_event.package_sha256,
                   first_state.bindings[first_index].package_sha256, 32));
    uint8_t event_digest[32];
    assert(SHA256(event, sizeof event, event_digest) != NULL &&
           !memcmp(observed_event.event_sha256, event_digest, 32));
    assert(esp_base_container_product_stop_confirmed(&claim));
    assert(!esp_base_container_product_event_accepting());
    assert(offer_event(
        first_state.bindings[first_index].package_sha256, 2U, event, sizeof event) ==
        ESP_BASE_CONTAINER_EVENT_UNAVAILABLE);
    int32_t expected = 3;
    assert(esp_base_container_with_firmware_set(&claim,
        ESP_BASE_OTA_FIRMWARE_CONFIRMED, NULL, open_after_stop, &expected) ==
        ECONTAINER_SLOTS_OK);

    install_context_t v2 = {.package = second, .operation_marker = 0x32};
    assert(esp_base_container_with_firmware_set(&claim,
        ESP_BASE_OTA_FIRMWARE_CONFIRMED, NULL, install_signed, &v2) ==
        ECONTAINER_SLOTS_OK);
    econtainer_slots_state_t second_state = {0};
    assert(econtainer_slots_load(&io, &geometry, &second_state) == ECONTAINER_SLOTS_OK);
    assert(second_state.sequence == first_state.sequence + 5U);
    const int second_index = binding_index(&second_state,
        physical.running_firmware_sha256);
    assert(first_index >= 0 && second_index >= 0);
    assert(memcmp(first_state.bindings[first_index].package_sha256,
                  second_state.bindings[second_index].package_sha256, 32) != 0);
    assert(memcmp(&physical, &original_firmware, sizeof physical) == 0);
    assert(esp_base_container_product_boot(&claim, boot_id) == ESP_BASE_CONTAINER_RUNNING);
    check_active_product(&claim, &expected_second_version, false, NULL);
    assert(esp_base_container_product_event_progress_count() == 0U);
    assert(esp_base_container_product_event_observation(&observed_event) ==
           ESP_BASE_CONTAINER_EVENT_NO_OBSERVATION);
    assert(offer_event(
        first_state.bindings[first_index].package_sha256, 2U, event, sizeof event) ==
        ESP_BASE_CONTAINER_EVENT_INVALID);
    assert(offer_event_when_available(
        second_state.bindings[second_index].package_sha256, 2U, event, sizeof event) == ESP_BASE_CONTAINER_EVENT_ACCEPTED);
    for (unsigned attempt = 0;
         attempt < 200U && esp_base_container_product_event_progress_count() == 0U;
         ++attempt) vTaskDelay(1U);
    assert(esp_base_container_product_event_progress_count() == 1U);
    observed_state = ESP_BASE_CONTAINER_EVENT_OBSERVATION_BUSY;
    for (unsigned attempt = 0;
         attempt < 200U && observed_state == ESP_BASE_CONTAINER_EVENT_OBSERVATION_BUSY;
         ++attempt) {
        observed_state = esp_base_container_product_event_observation(&observed_event);
        if (observed_state == ESP_BASE_CONTAINER_EVENT_OBSERVATION_BUSY) vTaskDelay(1U);
    }
    assert(observed_state == ESP_BASE_CONTAINER_EVENT_OBSERVED &&
           observed_event.event_sequence == 2U &&
           observed_event.runtime_ok && observed_event.guest_result == 6 &&
           !memcmp(observed_event.package_sha256,
                   second_state.bindings[second_index].package_sha256, 32));
    assert(esp_base_container_product_stop_confirmed(&claim));
    expected = 6;
    assert(esp_base_container_with_firmware_set(&claim,
        ESP_BASE_OTA_FIRMWARE_CONFIRMED, NULL, open_after_stop, &expected) ==
        ECONTAINER_SLOTS_OK);
    assert(esp_base_storage_release(&claim));
    dispose_product();
    assert(pthread_mutex_destroy(&store.mutex) == 0);
}

static void run_prepare_preserves_confirmed(const file_t *key,
                                            const file_t *confirmed_package,
                                            const file_t *candidate_package,
                                            const file_t *conflicting_package,
                                            const char boot_id[37])
{
    configure(key);
    esp_base_storage_owner_t owner;
    esp_base_storage_owner_init(&owner);
    esp_base_storage_claim_t claim = {0};
    assert(esp_base_storage_claim(&owner, &claim));
    assert(esp_base_container_product_boot(&claim, boot_id) == ESP_BASE_CONTAINER_EMPTY);
    install_context_t install = {.package = confirmed_package,
                                 .operation_marker = 0x91};
    assert(esp_base_container_with_firmware_set(&claim,
        ESP_BASE_OTA_FIRMWARE_CONFIRMED, NULL, install_signed, &install) ==
        ECONTAINER_SLOTS_OK);
    /* Keep two real signed references: the candidate matches the fallback's
     * digest, while the running firmware's different package must be read and
     * verified through the bounded reference reader. */
    const esp_base_ota_firmware_set_t source_firmware_set = physical;
    memcpy(physical.running_firmware_sha256,
           source_firmware_set.bootable_firmware_sha256[1], 32);
    memcpy(physical.bootable_firmware_sha256[0], physical.running_firmware_sha256, 32);
    memcpy(physical.bootable_firmware_sha256[1],
           source_firmware_set.running_firmware_sha256, 32);
    install.package = candidate_package;
    install.operation_marker = 0x92;
    assert(esp_base_container_with_firmware_set(&claim,
        ESP_BASE_OTA_FIRMWARE_CONFIRMED, NULL, install_signed, &install) ==
        ECONTAINER_SLOTS_OK);
    physical = source_firmware_set;
    assert(esp_base_container_product_boot(&claim, boot_id) == ESP_BASE_CONTAINER_RUNNING);
    econtainer_slots_state_t original = {0};
    assert(econtainer_slots_load(&io, &geometry, &original) == ECONTAINER_SLOTS_OK);
    const int index = binding_index(&original, physical.running_firmware_sha256);
    assert(index >= 0 && original.bindings[index].package_present);
    const int fallback_index = binding_index(&original, physical.bootable_firmware_sha256[1]);
    assert(fallback_index >= 0 && fallback_index != index &&
           original.bindings[fallback_index].package_present);
    const uint8_t original_slot = original.bindings[index].slot;
    uint8_t *original_bytes = malloc(confirmed_package->size);
    assert(original_bytes != NULL);
    memcpy(original_bytes,
        store.flash + geometry.slots[original_slot].offset_bytes - FLASH_BASE,
        confirmed_package->size);

    esp_base_container_package_request_t request = {
        .operation_id = "99999999-9999-4999-8999-999999999991",
        .expected_sequence = original.sequence,
        .previous_package_present = true,
        .package_size_bytes = (uint32_t)candidate_package->size,
        .guest_abi_version = 2U, .data_schema_version = 1U,
    };
    memcpy(request.previous_package_sha256,
        original.bindings[index].package_sha256, 32);
    assert(SHA256(candidate_package->bytes, candidate_package->size,
                  request.package_sha256) != NULL);
    uint32_t prepared_sequence = 123U;
    if (conflicting_package != NULL) {
        esp_base_container_package_request_t conflict = request;
        conflict.package_size_bytes = (uint32_t)conflicting_package->size;
        assert(SHA256(conflicting_package->bytes, conflicting_package->size,
                      conflict.package_sha256) != NULL);
        assert(memcmp(conflict.package_sha256, request.previous_package_sha256, 32) != 0);
        begin_write_workspace_observation(false, false);
        assert(esp_base_container_product_prepare_package(&claim, &conflict,
            read_source, (void *)conflicting_package, &prepared_sequence) ==
            ESP_BASE_CONTAINER_PREPARE_REJECTED);
        end_write_workspace_observation(1U, 1U, (1U << index) | (1U << fallback_index));
        econtainer_slots_state_t conflict_state = {0};
        assert(econtainer_slots_load(&io, &geometry, &conflict_state) == ECONTAINER_SLOTS_OK &&
               conflict_state.phase == ECONTAINER_SLOT_ABORTED &&
               conflict_state.sequence == original.sequence + 2U &&
               prepared_sequence == conflict_state.sequence &&
               same_binding(&original.bindings[0], &conflict_state.bindings[0]) &&
               same_binding(&original.bindings[1], &conflict_state.bindings[1]) &&
               s_product.thread_joinable && !s_product.stop_succeeded &&
               atomic_load(&s_product.instance_active) &&
               esp_base_container_product_event_accepting());
        assert(memcmp(original_bytes,
            store.flash + geometry.slots[original_slot].offset_bytes - FLASH_BASE,
            confirmed_package->size) == 0);
        const uint8_t event[] = {1U};
        assert(offer_event_when_available(original.bindings[index].package_sha256,
            1U, event, sizeof event) == ESP_BASE_CONTAINER_EVENT_ACCEPTED);
        for (unsigned attempt = 0; attempt < 200U &&
             esp_base_container_product_event_progress_count() == 0U; ++attempt) vTaskDelay(1U);
        assert(esp_base_container_product_event_progress_count() == 1U);
        check_active_product(&claim, &expected_confirmed_version, false, NULL);
        request.expected_sequence = conflict_state.sequence;
        memcpy(request.operation_id,
               "99999999-9999-4999-8999-999999999995", sizeof request.operation_id);
    }
    const unsigned writes_before_wrong_binding = store.blob_writes;
    request.previous_package_sha256[0] ^= 0x01U;
    begin_write_workspace_observation(false, false);
    assert(esp_base_container_product_prepare_package(&claim, &request,
        read_source, (void *)candidate_package,
        &prepared_sequence) == ESP_BASE_CONTAINER_PREPARE_REJECTED);
    end_write_workspace_observation(0U, 0U, 0U);
    assert(prepared_sequence == 0U &&
           store.blob_writes == writes_before_wrong_binding);
    request.previous_package_sha256[0] ^= 0x01U;
    memcpy(request.operation_id,
           "99999999-9999-4999-8999-999999999998", sizeof request.operation_id);
    begin_write_workspace_observation(false, false);
    assert(esp_base_container_product_prepare_package(&claim, &request,
        read_failed_source, (void *)candidate_package,
        &prepared_sequence) == ESP_BASE_CONTAINER_PREPARE_REJECTED);
    end_write_workspace_observation(0U, 0U, 0U);
    econtainer_slots_state_t source_rejected = {0};
    assert(econtainer_slots_load(&io, &geometry, &source_rejected) == ECONTAINER_SLOTS_OK &&
           source_rejected.phase == ECONTAINER_SLOT_ABORTED &&
           source_rejected.sequence == request.expected_sequence + 2U &&
           prepared_sequence == source_rejected.sequence &&
           same_binding(&original.bindings[0], &source_rejected.bindings[0]) &&
           same_binding(&original.bindings[1], &source_rejected.bindings[1]) &&
           esp_base_storage_claim_active(&claim) &&
           esp_base_container_product_event_accepting());
    request.expected_sequence = source_rejected.sequence;
    memcpy(request.operation_id,
           "99999999-9999-4999-8999-999999999999", sizeof request.operation_id);
    begin_write_workspace_observation(false, false);
    assert(esp_base_container_product_prepare_package(&claim, &request,
        read_corrupt_source, (void *)candidate_package,
        &prepared_sequence) == ESP_BASE_CONTAINER_PREPARE_REJECTED);
    end_write_workspace_observation(0U, 0U, 0U);
    assert(prepared_sequence == request.expected_sequence + 2U);
    econtainer_slots_state_t rejected = {0};
    assert(econtainer_slots_load(&io, &geometry, &rejected) == ECONTAINER_SLOTS_OK);
    assert(rejected.phase == ECONTAINER_SLOT_ABORTED &&
           rejected.sequence == request.expected_sequence + 2U);
    assert(rejected.bindings[index].slot == original_slot &&
           memcmp(rejected.bindings[index].package_sha256,
                  original.bindings[index].package_sha256, 32) == 0);
    assert(memcmp(original_bytes,
        store.flash + geometry.slots[original_slot].offset_bytes - FLASH_BASE,
        confirmed_package->size) == 0);
    assert(esp_base_container_product_event_accepting());

    request.expected_sequence = rejected.sequence;
    memcpy(request.operation_id,
           "99999999-9999-4999-8999-999999999990", sizeof request.operation_id);
    begin_write_workspace_observation(false, false);
    write_workspace_observation.corrupt_reference_mask = 1U << index;
    assert(esp_base_container_product_prepare_package(&claim, &request,
        read_source, (void *)candidate_package, &prepared_sequence) ==
        ESP_BASE_CONTAINER_PREPARE_REJECTED);
    end_write_workspace_observation(1U, 1U, 1U << index);
    assert(econtainer_slots_load(&io, &geometry, &rejected) == ECONTAINER_SLOTS_OK &&
           rejected.phase == ECONTAINER_SLOT_ABORTED &&
           rejected.sequence == request.expected_sequence + 2U &&
           prepared_sequence == rejected.sequence &&
           same_binding(&original.bindings[0], &rejected.bindings[0]) &&
           same_binding(&original.bindings[1], &rejected.bindings[1]) &&
           esp_base_storage_claim_active(&claim) &&
           esp_base_container_product_event_accepting() &&
           memcmp(original_bytes,
               store.flash + geometry.slots[original_slot].offset_bytes - FLASH_BASE,
               confirmed_package->size) == 0);

    for (unsigned independent_readback_failure = 0U;
         independent_readback_failure < 2U; ++independent_readback_failure) {
        request.expected_sequence = rejected.sequence;
        memcpy(request.operation_id, independent_readback_failure == 0U ?
            "99999999-9999-4999-8999-999999999996" :
            "99999999-9999-4999-8999-999999999997", sizeof request.operation_id);
        begin_write_workspace_observation(true, independent_readback_failure != 0U);
        assert(esp_base_container_product_prepare_package(&claim, &request,
            read_source, (void *)candidate_package, &prepared_sequence) ==
            (independent_readback_failure == 0U ? ESP_BASE_CONTAINER_PREPARE_REJECTED :
                                               ESP_BASE_CONTAINER_PREPARE_UNCERTAIN));
        end_write_workspace_observation(1U, 0U, 0U);
        /* The second case fails Base's independent ABORTED readback only:
         * Container's commit readback succeeded and the durable record exists. */
        assert(econtainer_slots_load(&io, &geometry, &rejected) == ECONTAINER_SLOTS_OK &&
               rejected.phase == ECONTAINER_SLOT_ABORTED &&
               rejected.sequence == request.expected_sequence + 2U &&
               prepared_sequence == (independent_readback_failure == 0U ? rejected.sequence : 0U) &&
               same_binding(&original.bindings[0], &rejected.bindings[0]) &&
               same_binding(&original.bindings[1], &rejected.bindings[1]) &&
               esp_base_storage_claim_active(&claim) && s_product.thread_joinable &&
               !s_product.stop_succeeded && atomic_load(&s_product.instance_active) &&
               esp_base_container_product_event_accepting() &&
               memcmp(original_bytes,
                   store.flash + geometry.slots[original_slot].offset_bytes - FLASH_BASE,
                   confirmed_package->size) == 0);
        check_active_product(&claim, &expected_confirmed_version, false, NULL);
    }

    memcpy(request.operation_id,
           "99999999-9999-4999-8999-999999999992", sizeof request.operation_id);
    request.expected_sequence = rejected.sequence;
    begin_write_workspace_observation(false, false);
    assert(esp_base_container_product_prepare_package(&claim, &request,
        read_source, (void *)candidate_package,
        &prepared_sequence) == ESP_BASE_CONTAINER_PREPARED);
    end_write_workspace_observation(1U, 1U, 1U << index);
    econtainer_slots_state_t prepared = {0};
    assert(econtainer_slots_load(&io, &geometry, &prepared) == ECONTAINER_SLOTS_OK);
    assert(prepared_sequence == rejected.sequence + 2U &&
           prepared.sequence == prepared_sequence &&
           prepared.phase == ECONTAINER_SLOT_PREPARED &&
           prepared.operation.slot != original_slot &&
           prepared.bindings[index].slot == original_slot &&
           memcmp(prepared.bindings[index].package_sha256,
                  original.bindings[index].package_sha256, 32) == 0 &&
           esp_base_container_product_event_accepting());
    assert(memcmp(original_bytes,
        store.flash + geometry.slots[original_slot].offset_bytes - FLASH_BASE,
        confirmed_package->size) == 0);
    assert(esp_base_container_product_start_package_trial(&claim,
        prepared_sequence, request.operation_id, boot_id, trial_event_digest) == ESP_BASE_CONTAINER_BLOCKED);
    assert(esp_base_container_product_stop_confirmed(&claim));
    const unsigned writes_before_trial = store.blob_writes;
    assert(esp_base_container_product_start_package_trial(&claim,
        prepared_sequence - 1U, request.operation_id, boot_id, trial_event_digest) ==
           ESP_BASE_CONTAINER_BLOCKED);
    assert(esp_base_container_product_start_package_trial(&claim,
        prepared_sequence, "99999999-9999-4999-8999-999999999994", boot_id, trial_event_digest) ==
           ESP_BASE_CONTAINER_BLOCKED);
    assert(store.blob_writes == writes_before_trial);
    assert(esp_base_container_product_start_package_trial(&claim,
        prepared_sequence, request.operation_id, boot_id, trial_event_digest) == ESP_BASE_CONTAINER_RUNNING);
    econtainer_slots_state_t trial = {0};
    assert(econtainer_slots_load(&io, &geometry, &trial) == ECONTAINER_SLOTS_OK);
    assert(trial.sequence == prepared_sequence + 1U &&
           trial.phase == ECONTAINER_SLOT_TRIAL_STARTED &&
           trial.bindings[index].slot == original_slot &&
           esp_base_container_product_event_accepting());
    esp_base_container_binding_snapshot_t pending_binding = {0};
    assert(esp_base_container_product_binding_snapshot(&claim, &pending_binding) ==
           ESP_BASE_CONTAINER_BINDING_OK &&
           pending_binding.container_sequence == trial.sequence &&
           pending_binding.package_present &&
           memcmp(pending_binding.package_sha256,
                  original.bindings[index].package_sha256, 32) == 0);
    check_active_product(&claim, &expected_candidate_version, true, request.operation_id);
    s_product.event_package_sha256[0] ^= 1U;
    esp_base_container_active_product_t invalid_active = {0};
    assert(esp_base_container_product_status_snapshot(&claim, &pending_binding, &invalid_active) ==
           ESP_BASE_CONTAINER_BINDING_UNCERTAIN && !invalid_active.present && invalid_active.product_version == NULL);
    s_product.event_package_sha256[0] ^= 1U;
    s_product.package_trial_operation_id[0] ^= 1U;
    assert(esp_base_container_product_binding_snapshot(&claim, &pending_binding) ==
           ESP_BASE_CONTAINER_BINDING_UNCERTAIN);
    s_product.package_trial_operation_id[0] ^= 1U;
    assert(!esp_base_container_product_confirm_firmware(&claim));
    const uint8_t trial_event[] = {1U, 2U, 3U};
    assert(offer_event(
        original.bindings[index].package_sha256, 1U,
        trial_event, sizeof trial_event) == ESP_BASE_CONTAINER_EVENT_INVALID);
    assert(offer_event_when_available(
        request.package_sha256, 1U, trial_event,
        sizeof trial_event) == ESP_BASE_CONTAINER_EVENT_ACCEPTED);
    for (unsigned attempt = 0;
         attempt < 200U && esp_base_container_product_event_progress_count() == 0U;
         ++attempt) vTaskDelay(1U);
    assert(esp_base_container_product_event_progress_count() == 1U);
    esp_base_container_event_observation_t trial_observation = {0};
    assert(esp_base_container_product_event_observation(&trial_observation) ==
           ESP_BASE_CONTAINER_EVENT_OBSERVED);
    assert(trial_observation.event_sequence == 1U && trial_observation.runtime_ok &&
           trial_observation.guest_result == 3 &&
           !memcmp(trial_observation.package_sha256, request.package_sha256, 32));
    uint8_t trial_event_digest[32];
    assert(SHA256(trial_event, sizeof trial_event, trial_event_digest) != NULL &&
           !memcmp(trial_observation.event_sha256, trial_event_digest, 32));
    assert(econtainer_slots_load(&io, &geometry, &trial) == ECONTAINER_SLOTS_OK &&
           trial.phase == ECONTAINER_SLOT_TRIAL_STARTED);
    assert(esp_base_container_product_abandon_package_trial(
        &claim, trial.sequence, request.operation_id));
    econtainer_slots_state_t abandoned = {0};
    assert(econtainer_slots_load(&io, &geometry, &abandoned) == ECONTAINER_SLOTS_OK &&
           abandoned.phase == ECONTAINER_SLOT_ABORTED &&
           abandoned.sequence == trial.sequence + 1U &&
           abandoned.bindings[index].slot == original_slot);
    assert(esp_base_container_product_boot(&claim, boot_id) == ESP_BASE_CONTAINER_RUNNING);
    check_active_product(&claim, &expected_confirmed_version, false, NULL);

    memcpy(request.operation_id,
           "99999999-9999-4999-8999-999999999993", sizeof request.operation_id);
    request.expected_sequence = abandoned.sequence;
    prepared_sequence = 123U;
    const unsigned erases_before_uncertain = store.flash_erases;
    const unsigned writes_before_uncertain = store.flash_writes;
    store.fail_read_after_next_write = 1U;
    assert(esp_base_container_product_prepare_package(&claim, &request,
        read_source, (void *)candidate_package,
        &prepared_sequence) == ESP_BASE_CONTAINER_PREPARE_UNCERTAIN);
    assert(prepared_sequence == 0U && esp_base_storage_claim_active(&claim) &&
           store.flash_erases == erases_before_uncertain &&
           store.flash_writes == writes_before_uncertain);
    econtainer_slots_state_t unresolved = {0};
    assert(econtainer_slots_load(&io, &geometry, &unresolved) == ECONTAINER_SLOTS_OK);
    assert(unresolved.sequence == abandoned.sequence + 1U &&
           unresolved.phase == ECONTAINER_SLOT_WRITING &&
           unresolved.bindings[index].slot == original_slot &&
           esp_base_container_product_event_accepting());
    assert(econtainer_slots_abandon(&io, &geometry, unresolved.sequence,
        s_product.boot_id, NULL, NULL, &abandoned) == ECONTAINER_SLOTS_OK);
    assert(esp_base_container_product_stop_confirmed(&claim));
    assert(esp_base_storage_release(&claim));
    free(original_bytes);
    dispose_product();
    assert(pthread_mutex_destroy(&store.mutex) == 0);
}

static void run_package_trial_confirmation(const file_t *key,
                                           const file_t *candidate_package,
                                           const char boot_id[37])
{
    configure(key);
    esp_base_storage_owner_t owner;
    esp_base_storage_owner_init(&owner);
    esp_base_storage_claim_t claim = {0};
    assert(esp_base_storage_claim(&owner, &claim));
    assert(esp_base_container_product_boot(&claim, boot_id) ==
           ESP_BASE_CONTAINER_EMPTY);
    econtainer_slots_state_t initial = {0};
    assert(econtainer_slots_load(&io, &geometry, &initial) == ECONTAINER_SLOTS_OK);
    const int index = binding_index(&initial, physical.running_firmware_sha256);
    assert(index >= 0 && !initial.bindings[index].package_present);
    esp_base_container_package_request_t request = {
        .operation_id = "99999999-9999-4999-8999-999999999997",
        .expected_sequence = initial.sequence,
        .package_size_bytes = (uint32_t)candidate_package->size,
        .guest_abi_version = 2U, .data_schema_version = 1U,
    };
    assert(SHA256(candidate_package->bytes, candidate_package->size,
                  request.package_sha256) != NULL);
    uint32_t prepared_sequence = 0U;
    assert(esp_base_container_product_prepare_package(&claim, &request,
        read_source, (void *)candidate_package, &prepared_sequence) ==
        ESP_BASE_CONTAINER_PREPARED);
    assert(esp_base_container_product_start_package_trial(&claim,
        prepared_sequence, request.operation_id, boot_id, trial_event_digest) ==
        ESP_BASE_CONTAINER_RUNNING);
    econtainer_slots_state_t trial = {0};
    assert(econtainer_slots_load(&io, &geometry, &trial) == ECONTAINER_SLOTS_OK &&
           trial.phase == ECONTAINER_SLOT_TRIAL_STARTED &&
           trial.sequence == prepared_sequence + 1U);
    check_active_product(&claim, &expected_candidate_version, true, request.operation_id);
    esp_base_container_binding_snapshot_t pending_binding = {0};
    assert(esp_base_container_product_binding_snapshot(&claim, &pending_binding) ==
           ESP_BASE_CONTAINER_BINDING_OK &&
           pending_binding.container_sequence == trial.sequence &&
           !pending_binding.package_present &&
           digest_zero(pending_binding.package_sha256));
    uint32_t confirmed_sequence = 123U;
    const unsigned before_event = store.blob_writes;
    const uint8_t event[] = {1U, 2U, 3U};
    uint8_t event_digest[32];
    assert(SHA256(event, sizeof event, event_digest) != NULL);
    assert(esp_base_container_product_confirm_package_trial(&claim,
        trial.sequence, request.operation_id, 1U, event_digest, 0U,
        &confirmed_sequence) == ESP_BASE_CONTAINER_CONFIRM_NOT_STARTED);
    assert(confirmed_sequence == 0U && store.blob_writes == before_event &&
           esp_base_container_product_event_accepting());
    assert(offer_event_when_available(request.package_sha256, 1U,
        event, sizeof event) == ESP_BASE_CONTAINER_EVENT_ACCEPTED);
    for (unsigned attempt = 0;
         attempt < 200U && esp_base_container_product_event_progress_count() == 0U;
         ++attempt) vTaskDelay(1U);
    esp_base_container_event_observation_t observation = {0};
    esp_base_container_event_observation_result_t observed =
        ESP_BASE_CONTAINER_EVENT_OBSERVATION_BUSY;
    for (unsigned attempt = 0;
         attempt < 200U && observed == ESP_BASE_CONTAINER_EVENT_OBSERVATION_BUSY;
         ++attempt) {
        observed = esp_base_container_product_event_observation(&observation);
        if (observed == ESP_BASE_CONTAINER_EVENT_OBSERVATION_BUSY) vTaskDelay(1U);
    }
    assert(observed == ESP_BASE_CONTAINER_EVENT_OBSERVED &&
           observation.event_sequence == 1U && observation.runtime_ok &&
           observation.guest_result == 3);
    esp_base_container_trial_event_snapshot_t trial_events = {0};
    assert(wait_trial_snapshot(&trial_events) &&
           trial_events.representative_event_sequence == 1U &&
           trial_events.failure_count == 0U &&
           memcmp(trial_events.package_sha256, request.package_sha256, 32) == 0);
    const uint8_t later_event[] = {4U, 5U, 6U};
    assert(offer_event_when_available(request.package_sha256, 2U, later_event,
                       sizeof later_event) == ESP_BASE_CONTAINER_EVENT_ACCEPTED);
    for (unsigned attempt = 0;
         attempt < 200U && esp_base_container_product_event_progress_count() < 2U;
         ++attempt) vTaskDelay(1U);
    observed = ESP_BASE_CONTAINER_EVENT_OBSERVATION_BUSY;
    for (unsigned attempt = 0;
         attempt < 200U && observed == ESP_BASE_CONTAINER_EVENT_OBSERVATION_BUSY;
         ++attempt) {
        observed = esp_base_container_product_event_observation(&observation);
        if (observed == ESP_BASE_CONTAINER_EVENT_OBSERVATION_BUSY) vTaskDelay(1U);
    }
    assert(observed == ESP_BASE_CONTAINER_EVENT_OBSERVED &&
           observation.event_sequence == 2U && observation.runtime_ok &&
           observation.guest_result >= 0);
    assert(wait_trial_snapshot(&trial_events) &&
           trial_events.representative_event_sequence == 1U &&
           trial_events.failure_count == 0U);
    const unsigned before_wrong_id = store.blob_writes;
    assert(esp_base_container_product_confirm_package_trial(&claim,
        trial.sequence, "99999999-9999-4999-8999-999999999998", 1U,
        event_digest, 0U, &confirmed_sequence) == ESP_BASE_CONTAINER_CONFIRM_NOT_STARTED);
    assert(esp_base_container_product_confirm_package_trial(&claim,
        trial.sequence - 1U, request.operation_id, 1U,
        event_digest, 0U, &confirmed_sequence) == ESP_BASE_CONTAINER_CONFIRM_NOT_STARTED);
    assert(esp_base_container_product_confirm_package_trial(&claim,
        trial.sequence, request.operation_id, 2U,
        event_digest, 0U, &confirmed_sequence) == ESP_BASE_CONTAINER_CONFIRM_NOT_STARTED);
    uint8_t wrong_event_digest[32];
    memcpy(wrong_event_digest, event_digest, sizeof wrong_event_digest);
    wrong_event_digest[0] ^= 1U;
    assert(esp_base_container_product_confirm_package_trial(&claim,
        trial.sequence, request.operation_id, 1U,
        wrong_event_digest, 0U, &confirmed_sequence) == ESP_BASE_CONTAINER_CONFIRM_NOT_STARTED);
    assert(store.blob_writes == before_wrong_id && confirmed_sequence == 0U);
    esp_base_container_trial_confirm_result_t confirmation =
        ESP_BASE_CONTAINER_CONFIRM_NOT_STARTED;
    for (unsigned attempt = 0;
         attempt < 200U && confirmation == ESP_BASE_CONTAINER_CONFIRM_NOT_STARTED;
         ++attempt) {
        vTaskDelay(1U);
        confirmation = esp_base_container_product_confirm_package_trial(&claim,
            trial.sequence, request.operation_id, 1U, event_digest, 0U,
            &confirmed_sequence);
    }
    assert(confirmation == ESP_BASE_CONTAINER_CONFIRM_CONFIRMED);
    check_active_product(&claim, &expected_candidate_version, false, NULL);
    econtainer_slots_state_t confirmed = {0};
    assert(econtainer_slots_load(&io, &geometry, &confirmed) == ECONTAINER_SLOTS_OK &&
           confirmed.sequence == trial.sequence + 2U &&
           confirmed_sequence == confirmed.sequence &&
           confirmed.phase == ECONTAINER_SLOT_CONFIRMED &&
           confirmed.bindings[index].package_present &&
           memcmp(confirmed.bindings[index].package_sha256,
                  request.package_sha256, 32) == 0 &&
           store.blob_writes == before_wrong_id + 2U &&
           esp_base_container_product_event_accepting());
    assert(esp_base_container_product_confirm_package_trial(&claim,
        trial.sequence, request.operation_id, 1U, event_digest, 0U,
        &confirmed_sequence) == ESP_BASE_CONTAINER_CONFIRM_NOT_STARTED);
    assert(esp_base_container_product_stop_confirmed(&claim));
    assert(esp_base_container_product_boot(&claim, boot_id) ==
           ESP_BASE_CONTAINER_RUNNING);
    assert(esp_base_container_product_stop_confirmed(&claim));
    assert(esp_base_storage_release(&claim));
    dispose_product();
    assert(pthread_mutex_destroy(&store.mutex) == 0);
}

static void run_prepared_package_abandon(const file_t *key,
                                         const file_t *candidate_package,
                                         const char boot_id[37])
{
    configure(key);
    esp_base_storage_owner_t owner;
    esp_base_storage_owner_init(&owner);
    esp_base_storage_claim_t claim = {0};
    assert(esp_base_storage_claim(&owner, &claim));
    assert(esp_base_container_product_boot(&claim, boot_id) ==
           ESP_BASE_CONTAINER_EMPTY);
    econtainer_slots_state_t initial = {0};
    assert(econtainer_slots_load(&io, &geometry, &initial) == ECONTAINER_SLOTS_OK);
    esp_base_container_package_request_t request = {
        .operation_id = "99999999-9999-4999-8999-999999999989",
        .expected_sequence = initial.sequence,
        .package_size_bytes = (uint32_t)candidate_package->size,
        .guest_abi_version = 2U, .data_schema_version = 1U,
    };
    assert(SHA256(candidate_package->bytes, candidate_package->size,
                  request.package_sha256) != NULL);
    uint32_t prepared_sequence = 0U;
    assert(esp_base_container_product_prepare_package(&claim, &request,
        read_source, (void *)candidate_package, &prepared_sequence) ==
        ESP_BASE_CONTAINER_PREPARED);
    uint32_t aborted_sequence = 123U;
    const unsigned writes_before = store.blob_writes;
    assert(!esp_base_container_product_abandon_prepared_package(&claim,
        prepared_sequence - 1U, request.operation_id, request.package_sha256,
        &aborted_sequence));
    assert(!esp_base_container_product_abandon_prepared_package(&claim,
        prepared_sequence, "99999999-9999-4999-8999-999999999988",
        request.package_sha256, &aborted_sequence));
    uint8_t wrong_sha256[32];
    memcpy(wrong_sha256, request.package_sha256, sizeof wrong_sha256);
    wrong_sha256[0] ^= 1U;
    assert(!esp_base_container_product_abandon_prepared_package(&claim,
        prepared_sequence, request.operation_id, wrong_sha256,
        &aborted_sequence));
    assert(aborted_sequence == 0U && store.blob_writes == writes_before);
    assert(esp_base_container_product_abandon_prepared_package(&claim,
        prepared_sequence, request.operation_id, request.package_sha256,
        &aborted_sequence));
    econtainer_slots_state_t aborted = {0};
    assert(econtainer_slots_load(&io, &geometry, &aborted) == ECONTAINER_SLOTS_OK &&
           aborted.sequence == prepared_sequence + 1U &&
           aborted.sequence == aborted_sequence &&
           aborted.phase == ECONTAINER_SLOT_ABORTED &&
           store.blob_writes == writes_before + 1U);
    for (unsigned index = 0; index < ECONTAINER_SLOT_BINDING_COUNT; ++index)
        assert(same_binding(&initial.bindings[index], &aborted.bindings[index]));
    assert(!esp_base_container_product_abandon_prepared_package(&claim,
        prepared_sequence, request.operation_id, request.package_sha256,
        &aborted_sequence));
    assert(aborted_sequence == 0U && store.blob_writes == writes_before + 1U);
    assert(esp_base_container_product_boot(&claim, boot_id) ==
           ESP_BASE_CONTAINER_EMPTY);
    assert(esp_base_storage_release(&claim));
    dispose_product();
    assert(pthread_mutex_destroy(&store.mutex) == 0);
}

static void run_package_trial_confirmation_uncertain(
    const file_t *key, const file_t *candidate_package,
    const char boot_id[37], unsigned failed_read,
    econtainer_slot_phase_t expected_phase)
{
    configure(key);
    esp_base_storage_owner_t owner;
    esp_base_storage_owner_init(&owner);
    esp_base_storage_claim_t claim = {0};
    assert(esp_base_storage_claim(&owner, &claim));
    assert(esp_base_container_product_boot(&claim, boot_id) ==
           ESP_BASE_CONTAINER_EMPTY);
    econtainer_slots_state_t initial = {0};
    assert(econtainer_slots_load(&io, &geometry, &initial) == ECONTAINER_SLOTS_OK);
    esp_base_container_package_request_t request = {
        .operation_id = "99999999-9999-4999-8999-999999999990",
        .expected_sequence = initial.sequence,
        .package_size_bytes = (uint32_t)candidate_package->size,
        .guest_abi_version = 2U, .data_schema_version = 1U,
    };
    assert(SHA256(candidate_package->bytes, candidate_package->size,
                  request.package_sha256) != NULL);
    uint32_t prepared_sequence = 0U;
    assert(esp_base_container_product_prepare_package(&claim, &request,
        read_source, (void *)candidate_package, &prepared_sequence) ==
        ESP_BASE_CONTAINER_PREPARED);
    assert(esp_base_container_product_start_package_trial(&claim,
        prepared_sequence, request.operation_id, boot_id, trial_event_digest) ==
        ESP_BASE_CONTAINER_RUNNING);
    const uint8_t event[] = {1U, 2U, 3U};
    uint8_t event_digest[32];
    assert(SHA256(event, sizeof event, event_digest) != NULL);
    assert(offer_event_when_available(request.package_sha256, 1U,
        event, sizeof event) == ESP_BASE_CONTAINER_EVENT_ACCEPTED);
    for (unsigned attempt = 0;
         attempt < 200U && esp_base_container_product_event_progress_count() == 0U;
         ++attempt) vTaskDelay(1U);
    assert(esp_base_container_product_event_progress_count() == 1U);
    store.fail_read_after_next_write = failed_read;
    uint32_t confirmed_sequence = 123U;
    esp_base_container_trial_confirm_result_t confirmation =
        ESP_BASE_CONTAINER_CONFIRM_NOT_STARTED;
    for (unsigned attempt = 0;
         attempt < 200U && confirmation == ESP_BASE_CONTAINER_CONFIRM_NOT_STARTED;
         ++attempt) {
        vTaskDelay(1U);
        confirmation = esp_base_container_product_confirm_package_trial(&claim,
            prepared_sequence + 1U, request.operation_id, 1U,
            event_digest, 0U, &confirmed_sequence);
    }
    assert(confirmation == ESP_BASE_CONTAINER_CONFIRM_UNCERTAIN);
    assert(confirmed_sequence == 0U && esp_base_storage_claim_active(&claim) &&
           !esp_base_container_product_event_accepting());
    econtainer_slots_state_t persisted = {0};
    assert(econtainer_slots_load(&io, &geometry, &persisted) == ECONTAINER_SLOTS_OK &&
           persisted.phase == expected_phase &&
           persisted.sequence == prepared_sequence +
               (expected_phase == ECONTAINER_SLOT_CONFIRMED ? 3U : 2U));
    assert(esp_base_container_product_stop_trial(&claim));
    assert(esp_base_storage_release(&claim));
    dispose_product();
    configure_product(key); /* New boot retains the durable ECS2 and package bytes. */
    assert(esp_base_storage_claim(&owner, &claim));
    const char new_boot_id[] = "77777777-7777-4777-8777-777777777779";
    uint32_t resolved_sequence = 123U;
    const unsigned writes_before = store.blob_writes;
    assert(esp_base_container_product_recover_pending_package(&claim,
        new_boot_id, "99999999-9999-4999-8999-999999999991",
        request.expected_sequence, request.package_sha256, &resolved_sequence) ==
        ESP_BASE_CONTAINER_PACKAGE_RECOVERY_UNCERTAIN);
    assert(resolved_sequence == 0U && store.blob_writes == writes_before);
    if (expected_phase == ECONTAINER_SLOT_CONFIRMED) {
        uint8_t wrong_sha256[32];
        memcpy(wrong_sha256, request.package_sha256, sizeof wrong_sha256);
        wrong_sha256[0] ^= 1U;
        assert(esp_base_container_product_recover_pending_package(&claim,
            new_boot_id, request.operation_id, request.expected_sequence,
            wrong_sha256, &resolved_sequence) ==
            ESP_BASE_CONTAINER_PACKAGE_RECOVERY_UNCERTAIN);
        assert(esp_base_container_product_recover_pending_package(&claim,
            boot_id, request.operation_id, request.expected_sequence,
            request.package_sha256, &resolved_sequence) ==
            ESP_BASE_CONTAINER_PACKAGE_RECOVERY_UNCERTAIN);
        assert(resolved_sequence == 0U && store.blob_writes == writes_before);
        const uint32_t offset = geometry.slots[persisted.operation.slot].offset_bytes -
            FLASH_BASE;
        store.flash[offset] ^= 1U;
        assert(esp_base_container_product_recover_pending_package(&claim,
            new_boot_id, request.operation_id, request.expected_sequence,
            request.package_sha256, &resolved_sequence) ==
            ESP_BASE_CONTAINER_PACKAGE_RECOVERY_UNCERTAIN);
        assert(resolved_sequence == 0U && store.blob_writes == writes_before);
        store.flash[offset] ^= 1U;
    }
    const esp_base_container_package_recovery_t outcome =
        esp_base_container_product_recover_pending_package(&claim,
            new_boot_id, request.operation_id, request.expected_sequence,
            request.package_sha256, &resolved_sequence);
    assert(outcome == (expected_phase == ECONTAINER_SLOT_CONFIRMED ?
           ESP_BASE_CONTAINER_PACKAGE_RECOVERY_SUCCEEDED :
           ESP_BASE_CONTAINER_PACKAGE_RECOVERY_FAILED));
    assert(resolved_sequence == persisted.sequence +
           (expected_phase == ECONTAINER_SLOT_CONFIRMED ? 0U : 1U));
    assert(store.blob_writes == writes_before +
           (expected_phase == ECONTAINER_SLOT_CONFIRMED ? 0U : 1U));
    assert(esp_base_container_product_boot(&claim, new_boot_id) ==
           (expected_phase == ECONTAINER_SLOT_CONFIRMED ?
            ESP_BASE_CONTAINER_RUNNING : ESP_BASE_CONTAINER_EMPTY));
    if (expected_phase == ECONTAINER_SLOT_CONFIRMED)
        assert(esp_base_container_product_stop_confirmed(&claim));
    assert(esp_base_storage_release(&claim));
    dispose_product();
    assert(pthread_mutex_destroy(&store.mutex) == 0);
}

static void run_package_trial_cold_recovery(const file_t *key,
                                            const file_t *confirmed_package,
                                            const file_t *candidate_package,
                                            const char old_boot_id[37])
{
    const char new_boot_id[] = "77777777-7777-4777-8777-777777777777";
    configure(key);
    esp_base_storage_owner_t owner;
    esp_base_storage_owner_init(&owner);
    esp_base_storage_claim_t claim = {0};
    assert(esp_base_storage_claim(&owner, &claim));
    assert(esp_base_container_product_boot(&claim, old_boot_id) ==
           ESP_BASE_CONTAINER_EMPTY);
    install_context_t install = {.package = confirmed_package,
                                 .operation_marker = 0x95};
    assert(esp_base_container_with_firmware_set(&claim,
        ESP_BASE_OTA_FIRMWARE_CONFIRMED, NULL, install_signed, &install) ==
        ECONTAINER_SLOTS_OK);
    assert(esp_base_container_product_boot(&claim, old_boot_id) ==
           ESP_BASE_CONTAINER_RUNNING);
    econtainer_slots_state_t original = {0};
    assert(econtainer_slots_load(&io, &geometry, &original) == ECONTAINER_SLOTS_OK);
    const int index = binding_index(&original, physical.running_firmware_sha256);
    assert(index >= 0 && original.bindings[index].package_present);
    esp_base_container_package_request_t request = {
        .operation_id = "99999999-9999-4999-8999-999999999995",
        .expected_sequence = original.sequence,
        .previous_package_present = true,
        .package_size_bytes = (uint32_t)candidate_package->size,
        .guest_abi_version = 2U, .data_schema_version = 1U,
    };
    memcpy(request.previous_package_sha256,
        original.bindings[index].package_sha256, 32);
    assert(SHA256(candidate_package->bytes, candidate_package->size,
                  request.package_sha256) != NULL);
    uint32_t prepared_sequence = 0U;
    assert(esp_base_container_product_prepare_package(&claim, &request,
        read_source, (void *)candidate_package, &prepared_sequence) ==
        ESP_BASE_CONTAINER_PREPARED);
    assert(esp_base_container_product_stop_confirmed(&claim));
    assert(esp_base_container_product_start_package_trial(&claim,
        prepared_sequence, request.operation_id, old_boot_id, trial_event_digest) ==
        ESP_BASE_CONTAINER_RUNNING);
    econtainer_slots_state_t trial = {0};
    assert(econtainer_slots_load(&io, &geometry, &trial) == ECONTAINER_SLOTS_OK &&
           trial.phase == ECONTAINER_SLOT_TRIAL_STARTED);
    assert(esp_base_container_product_stop_trial(&claim));
    assert(esp_base_storage_release(&claim));
    dispose_product(); /* Simulate a new boot; retain the same Flash and NVS. */
    configure_product(key);
    store.flash[geometry.slots[trial.operation.slot].offset_bytes - FLASH_BASE] ^= 1U;
    econtainer_slot_firmware_set_t firmware_set = {.bootable_count = 2};
    memcpy(firmware_set.running_firmware_sha256,
           physical.running_firmware_sha256, 32);
    memcpy(firmware_set.bootable_firmware_sha256,
           physical.bootable_firmware_sha256,
           sizeof firmware_set.bootable_firmware_sha256);
    econtainer_slot_boot_decision_t decision = ECONTAINER_SLOT_BOOT_BLOCKED;
    econtainer_slots_state_t inspected = {0};
    assert(econtainer_slots_reconcile(&io, &geometry, &firmware_set,
        &inspected, &decision) == ECONTAINER_SLOTS_UNTRUSTED &&
        decision == ECONTAINER_SLOT_BOOT_RECOVER_CONFIRMED_CANDIDATE_INVALID);
    assert(esp_base_storage_claim(&owner, &claim));
    uint32_t resolved_sequence = 123U;
    const unsigned writes_before = store.blob_writes;
    assert(esp_base_container_product_recover_pending_package(&claim,
        new_boot_id, "99999999-9999-4999-8999-999999999996",
        original.sequence, request.package_sha256, &resolved_sequence) ==
        ESP_BASE_CONTAINER_PACKAGE_RECOVERY_UNCERTAIN);
    assert(esp_base_container_product_recover_pending_package(&claim,
        old_boot_id, request.operation_id, original.sequence,
        request.package_sha256, &resolved_sequence) ==
        ESP_BASE_CONTAINER_PACKAGE_RECOVERY_UNCERTAIN);
    assert(store.blob_writes == writes_before && resolved_sequence == 0U);
    assert(esp_base_container_product_recover_pending_package(&claim,
        new_boot_id, request.operation_id, original.sequence,
        request.package_sha256, &resolved_sequence) ==
        ESP_BASE_CONTAINER_PACKAGE_RECOVERY_FAILED);
    assert(resolved_sequence == trial.sequence + 1U);
    econtainer_slots_state_t abandoned = {0};
    assert(econtainer_slots_load(&io, &geometry, &abandoned) == ECONTAINER_SLOTS_OK &&
           abandoned.phase == ECONTAINER_SLOT_ABORTED &&
           abandoned.sequence == resolved_sequence &&
           same_binding(&abandoned.bindings[index], &original.bindings[index]));
    const unsigned writes_after_recovery = store.blob_writes;
    assert(esp_base_container_product_recover_pending_package(&claim,
        new_boot_id, request.operation_id, original.sequence,
        request.package_sha256, &resolved_sequence) ==
        ESP_BASE_CONTAINER_PACKAGE_RECOVERY_FAILED);
    assert(resolved_sequence == abandoned.sequence &&
           store.blob_writes == writes_after_recovery);
    assert(esp_base_container_product_boot(&claim, new_boot_id) ==
           ESP_BASE_CONTAINER_RUNNING);
    assert(esp_base_container_product_stop_confirmed(&claim));
    assert(esp_base_storage_release(&claim));
    dispose_product();
    assert(pthread_mutex_destroy(&store.mutex) == 0);
}

static void run_package_intent_without_reservation(const file_t *key,
                                                   const file_t *confirmed_package,
                                                   const char old_boot_id[37])
{
    const char new_boot_id[] = "77777777-7777-4777-8777-777777777778";
    const char operation_id[] = "99999999-9999-4999-8999-999999999997";
    configure(key);
    esp_base_storage_owner_t owner;
    esp_base_storage_owner_init(&owner);
    esp_base_storage_claim_t claim = {0};
    assert(esp_base_storage_claim(&owner, &claim));
    assert(esp_base_container_product_boot(&claim, old_boot_id) ==
           ESP_BASE_CONTAINER_EMPTY);
    install_context_t install = {.package = confirmed_package,
                                 .operation_marker = 0x96};
    assert(esp_base_container_with_firmware_set(&claim,
        ESP_BASE_OTA_FIRMWARE_CONFIRMED, NULL, install_signed, &install) ==
        ECONTAINER_SLOTS_OK);
    assert(esp_base_container_product_boot(&claim, old_boot_id) ==
           ESP_BASE_CONTAINER_RUNNING);
    econtainer_slots_state_t before = {0};
    assert(econtainer_slots_load(&io, &geometry, &before) == ECONTAINER_SLOTS_OK);
    assert(esp_base_container_product_stop_confirmed(&claim));
    assert(esp_base_storage_release(&claim));
    dispose_product();
    configure_product(key);
    assert(esp_base_storage_claim(&owner, &claim));
    const unsigned writes_before = store.blob_writes;
    const uint8_t candidate_sha256[32] = {0xab};
    uint32_t resolved_sequence = 0U;
    assert(esp_base_container_product_recover_pending_package(&claim,
        new_boot_id, operation_id, before.sequence, candidate_sha256,
        &resolved_sequence) == ESP_BASE_CONTAINER_PACKAGE_RECOVERY_FAILED);
    assert(resolved_sequence == before.sequence &&
           store.blob_writes == writes_before);
    assert(esp_base_container_product_boot(&claim, new_boot_id) ==
           ESP_BASE_CONTAINER_RUNNING);
    assert(esp_base_container_product_stop_confirmed(&claim));
    assert(esp_base_storage_release(&claim));
    dispose_product();
    assert(pthread_mutex_destroy(&store.mutex) == 0);
}

static void run_firmware_empty_stage(const file_t *key, const file_t *package,
                                     const char boot_id[37],
                                     esp_base_ota_package_mode_t mode,
                                     unsigned trial_case)
{
    configure(key);
    esp_base_storage_owner_t owner;
    esp_base_storage_owner_init(&owner);
    esp_base_storage_claim_t claim = {0};
    assert(esp_base_storage_claim(&owner, &claim));
    assert(esp_base_container_product_boot(&claim, boot_id) ==
           ESP_BASE_CONTAINER_EMPTY);
    econtainer_slots_state_t state = {0};
    assert(econtainer_slots_load(&io, &geometry, &state) == ECONTAINER_SLOTS_OK);
    const uint32_t original_sequence = state.sequence;
    econtainer_slot_firmware_set_t a_only = {.bootable_count = 1U};
    memcpy(a_only.running_firmware_sha256,
           physical.running_firmware_sha256, 32);
    memcpy(a_only.bootable_firmware_sha256[0],
           physical.running_firmware_sha256, 32);
    uint8_t old_b[32];
    memcpy(old_b, physical.bootable_firmware_sha256[1], 32);
    physical.bootable_count = 1U;
    memset(physical.bootable_firmware_sha256[1], 0, 32);
    assert(econtainer_slots_retire_inactive_firmware(&io, &geometry,
        state.sequence, &a_only, old_b, &state) == ECONTAINER_SLOTS_OK);
    eota_prepared_t prepared = {.image_size_bytes = 512U};
    memset(prepared.sha256, 0xc3, 32);
    esp_base_ota_receipt_recovery_t receipt = {
        .status = ESP_BASE_OTA_RECEIPT_PREPARED,
        .container_enabled = true,
        .container_sequence = original_sequence,
        .package_mode = mode,
        .image_size_bytes = prepared.image_size_bytes,
    };
    if (mode == ESP_BASE_OTA_PACKAGE_WRITE) {
        assert(package != NULL && package->size <= UINT32_MAX);
        receipt.package_size_bytes = (uint32_t)package->size;
        receipt.guest_abi_version = 2U;
        receipt.data_schema_version = 1U;
        assert(SHA256(package->bytes, package->size, receipt.package_sha256) != NULL);
        memset(receipt.trial_event_sha256, 0xe5, 32);
    } else {
        assert(mode == ESP_BASE_OTA_NO_PACKAGE);
    }
    strcpy(receipt.operation_id, "99999999-9999-4999-8999-999999999998");
    memcpy(receipt.source_sha256, physical.running_firmware_sha256, 32);
    memcpy(receipt.inactive_sha256, old_b, 32);
    memcpy(receipt.candidate_sha256, prepared.sha256, 32);
    const unsigned erases = store.flash_erases;
    const unsigned writes = store.flash_writes;
    assert(esp_base_container_product_stage_firmware(&claim, &prepared, &receipt) ==
           (mode == ESP_BASE_OTA_PACKAGE_WRITE ?
            ESP_BASE_CONTAINER_STAGE_WRITING :
            ESP_BASE_CONTAINER_STAGE_PREPARED));
    assert(econtainer_slots_load(&io, &geometry, &state) == ECONTAINER_SLOTS_OK &&
           state.sequence == original_sequence + 2U &&
           state.phase == (mode == ESP_BASE_OTA_PACKAGE_WRITE ?
                           ECONTAINER_SLOT_WRITING : ECONTAINER_SLOT_PREPARED) &&
           state.operation.kind == (mode == ESP_BASE_OTA_PACKAGE_WRITE ?
                                    ECONTAINER_SLOT_PACKAGE_WRITE :
                                    ECONTAINER_SLOT_NO_PACKAGE) &&
           store.flash_erases == erases && store.flash_writes == writes);
    if (mode == ESP_BASE_OTA_PACKAGE_WRITE) {
        physical.bootable_count = 2U;
        memcpy(physical.bootable_firmware_sha256[0], prepared.sha256, 32);
        memcpy(physical.bootable_firmware_sha256[1], receipt.source_sha256, 32);
        memcpy(physical.running_firmware_sha256, prepared.sha256, 32);
        const unsigned writing_blob_writes = store.blob_writes;
        const unsigned writing_flash_erases = store.flash_erases;
        const unsigned writing_flash_writes = store.flash_writes;
        assert(!esp_base_container_product_reconcile_selected_ota(
            &claim, &receipt, EOTA_STATE_PENDING_VERIFY));
        assert(store.blob_writes == writing_blob_writes &&
               store.flash_erases == writing_flash_erases &&
               store.flash_writes == writing_flash_writes);
        physical.bootable_count = 1U;
        memcpy(physical.running_firmware_sha256, receipt.source_sha256, 32);
        memcpy(physical.bootable_firmware_sha256[0], receipt.source_sha256, 32);
        memset(physical.bootable_firmware_sha256[1], 0, 32);
        const uint32_t target_offset = geometry.slots[state.operation.slot].offset_bytes -
                                       FLASH_BASE;
        assert(esp_base_container_product_write_staged_firmware_package(
            &claim, &prepared, &receipt, read_source, (void *)package) ==
            ESP_BASE_CONTAINER_STAGE_PREPARED);
        assert(econtainer_slots_load(&io, &geometry, &state) == ECONTAINER_SLOTS_OK &&
               state.phase == ECONTAINER_SLOT_PREPARED &&
               state.sequence == original_sequence + 3U &&
               memcmp(store.flash + target_offset, package->bytes,
                      package->size) == 0);
        physical.bootable_count = 2U;
        memcpy(physical.bootable_firmware_sha256[0], prepared.sha256, 32);
        memcpy(physical.bootable_firmware_sha256[1], receipt.source_sha256, 32);
        memcpy(physical.running_firmware_sha256, prepared.sha256, 32);
        dispose_product();
        configure_product(key);
        const unsigned selected_blob_writes = store.blob_writes;
        const unsigned selected_flash_erases = store.flash_erases;
        const unsigned selected_flash_writes = store.flash_writes;
        assert(esp_base_container_product_reconcile_selected_ota(
            &claim, &receipt, EOTA_STATE_PENDING_VERIFY));
        assert(store.blob_writes == selected_blob_writes &&
               store.flash_erases == selected_flash_erases &&
               store.flash_writes == selected_flash_writes);
        if (trial_case == 2U) {
            const uint8_t event[] = {1U, 2U, 3U};
            memcpy(receipt.trial_event_sha256, trial_event_digest, 32);
            assert(esp_base_container_product_start_firmware_package_trial(
                &claim, &receipt, boot_id) == ESP_BASE_CONTAINER_RUNNING);
            assert(offer_event_when_available(receipt.package_sha256, 1U, event, sizeof event) == ESP_BASE_CONTAINER_EVENT_ACCEPTED);
            for (unsigned attempt = 0;
                 attempt < 200U && esp_base_container_product_event_progress_count() == 0U;
                 ++attempt) vTaskDelay(1U);
            esp_base_container_trial_health_result_t health = ESP_BASE_CONTAINER_HEALTH_NOT_STARTED;
            for (unsigned attempt = 0;
                 attempt < 200U && health == ESP_BASE_CONTAINER_HEALTH_NOT_STARTED;
                 ++attempt) {
                vTaskDelay(1U);
                health = esp_base_container_product_verify_firmware_package_health(
                    &claim, &receipt, 1U, 0U);
            }
            assert(health == ESP_BASE_CONTAINER_HEALTH_VERIFIED);
            assert(esp_base_container_product_confirm_firmware(&claim));
            assert(esp_base_container_product_stop_confirmed(&claim));
            assert(econtainer_slots_load(&io, &geometry, &state) == ECONTAINER_SLOTS_OK &&
                   state.phase == ECONTAINER_SLOT_CONFIRMED &&
                   state.sequence == original_sequence + 6U);
            const int empty_source = binding_index(&state, receipt.source_sha256);
            assert(empty_source >= 0 && !state.bindings[empty_source].package_present);
            goto empty_finished;
        }
        if (trial_case == 1U) {
            uint8_t trial_boot_id[ECONTAINER_SLOT_BOOT_ID_BYTES];
            assert(decode_uuid(boot_id, trial_boot_id));
            assert(econtainer_slots_begin_trial(&io, &geometry, state.sequence,
                prepared.sha256, trial_boot_id, &state) == ECONTAINER_SLOTS_OK);
            assert(econtainer_slots_mark_healthy(&io, &geometry, state.sequence,
                trial_boot_id, &state) == ECONTAINER_SLOTS_OK);
            dispose_product();
            configure_product(key);
            assert(esp_base_container_product_reconcile_selected_ota(
                &claim, &receipt, EOTA_STATE_VALID));
            assert(econtainer_slots_load(&io, &geometry, &state) == ECONTAINER_SLOTS_OK &&
                   state.phase == ECONTAINER_SLOT_CONFIRMED &&
                   state.sequence == original_sequence + 6U &&
                   store.flash_erases == selected_flash_erases &&
                   store.flash_writes == selected_flash_writes);
            const int empty_source = binding_index(&state, receipt.source_sha256);
            assert(empty_source >= 0 && !state.bindings[empty_source].package_present);
            assert(esp_base_container_product_boot(
                &claim, "88888888-8888-4888-8888-888888888888") == ESP_BASE_CONTAINER_RUNNING);
            assert(esp_base_container_product_stop_confirmed(&claim));
            goto empty_finished;
        }
        physical.bootable_count = 1U;
        memcpy(physical.running_firmware_sha256, receipt.source_sha256, 32);
        memcpy(physical.bootable_firmware_sha256[0], receipt.source_sha256, 32);
        memset(physical.bootable_firmware_sha256[1], 0, 32);
        dispose_product();
        configure_product(key);
        uint8_t recovery_boot_id[ECONTAINER_SLOT_BOOT_ID_BYTES];
        assert(decode_uuid("88888888-8888-4888-8888-888888888888",
                           recovery_boot_id));
        assert(econtainer_slots_abandon(&io, &geometry, state.sequence,
                   recovery_boot_id, NULL, NULL, &state) == ECONTAINER_SLOTS_OK &&
               state.phase == ECONTAINER_SLOT_ABORTED);
        const unsigned recovery_flash_erases = store.flash_erases;
        const unsigned recovery_flash_writes = store.flash_writes;
        assert(esp_base_container_product_recover_retired_firmware(
            &claim, &receipt, "88888888-8888-4888-8888-888888888888") ==
            ESP_BASE_CONTAINER_RETIRE_COMPLETE);
        assert(econtainer_slots_load(&io, &geometry, &state) == ECONTAINER_SLOTS_OK &&
               state.phase == ECONTAINER_SLOT_IDLE &&
               state.sequence == original_sequence + 5U &&
               store.flash_erases == recovery_flash_erases &&
               store.flash_writes == recovery_flash_writes);
    } else if (trial_case == 2U) {
        physical.bootable_count = 2U;
        memcpy(physical.bootable_firmware_sha256[0], prepared.sha256, 32);
        memcpy(physical.bootable_firmware_sha256[1], receipt.source_sha256, 32);
        memcpy(physical.running_firmware_sha256, prepared.sha256, 32);
        dispose_product();
        configure_product(key);
        assert(esp_base_container_product_start_trial(&claim, boot_id) ==
               ESP_BASE_CONTAINER_EMPTY);
        assert(esp_base_container_product_mark_healthy(&claim));
        assert(esp_base_container_product_confirm_firmware(&claim));
        assert(!s_product.trial_mode && s_product.reopen_allowed &&
               !s_product.thread_joinable && s_product.native_reclaimed);
        const unsigned confirmed_writes = store.blob_writes;
        assert(esp_base_container_product_boot(&claim, boot_id) == ESP_BASE_CONTAINER_EMPTY);
        assert(store.blob_writes == confirmed_writes);
    }
empty_finished:
    assert(esp_base_storage_release(&claim));
    dispose_product();
    assert(pthread_mutex_destroy(&store.mutex) == 0);
}

static void run_firmware_package_stage_internal(const file_t *key, const file_t *package,
                                       const file_t *conflicting_package,
                                       const char boot_id[37],
                                       esp_base_ota_package_mode_t mode,
                                       bool corrupt_write, bool recover_before_stage,
                                       bool recover_valid, unsigned live_trial, bool source_only,
                                       unsigned write_failure)
{
    assert(write_failure <= 2U && (write_failure == 0U ||
        (mode == ESP_BASE_OTA_PACKAGE_WRITE && conflicting_package == NULL &&
         !corrupt_write && !recover_before_stage && !recover_valid && live_trial == 0U)));
    configure(key);
    if (source_only) {
        physical.bootable_count = 1U;
        memset(physical.bootable_firmware_sha256[1], 0, 32);
    }
    esp_base_storage_owner_t owner;
    esp_base_storage_owner_init(&owner);
    esp_base_storage_claim_t claim = {0};
    assert(esp_base_storage_claim(&owner, &claim));
    assert(esp_base_container_product_boot(&claim, boot_id) ==
           ESP_BASE_CONTAINER_EMPTY);
    install_context_t install = {.package = package, .operation_marker = 0x97};
    assert(esp_base_container_with_firmware_set(&claim,
        ESP_BASE_OTA_FIRMWARE_CONFIRMED, NULL, install_signed, &install) ==
        ECONTAINER_SLOTS_OK);
    assert(esp_base_container_product_boot(&claim, boot_id) ==
           ESP_BASE_CONTAINER_RUNNING);
    econtainer_slots_state_t state = {0};
    assert(econtainer_slots_load(&io, &geometry, &state) == ECONTAINER_SLOTS_OK);
    const uint32_t original_sequence = state.sequence;
    const uint32_t retired_sequence = original_sequence + (source_only ? 0U : 1U);
    const int source_index = binding_index(&state, physical.running_firmware_sha256);
    assert(source_index >= 0 && state.bindings[source_index].package_present);
    const econtainer_slot_binding_t source = state.bindings[source_index];
    const uint8_t candidate[32] = {[0] = 0xc3};
    eota_prepared_t prepared = {.image_size_bytes = 512U};
    memcpy(prepared.sha256, candidate, 32);
    esp_base_ota_receipt_recovery_t receipt = {
        .status = ESP_BASE_OTA_RECEIPT_PREPARED,
        .container_enabled = true,
        .container_sequence = original_sequence,
        .package_mode = mode,
        .source_package_present = true,
        .source_package_size_bytes = source.package_size_bytes,
        .source_guest_abi_version = source.guest_abi_version,
        .source_data_schema_version = source.data_schema_version,
        .package_size_bytes = (uint32_t)package->size,
        .guest_abi_version = source.guest_abi_version,
        .data_schema_version = source.data_schema_version,
        .image_size_bytes = prepared.image_size_bytes,
    };
    strcpy(receipt.operation_id, "99999999-9999-4999-8999-999999999997");
    memcpy(receipt.source_sha256, physical.running_firmware_sha256, 32);
    memcpy(receipt.inactive_sha256, physical.bootable_firmware_sha256[1], 32);
    memcpy(receipt.candidate_sha256, candidate, 32);
    memcpy(receipt.source_package_sha256, source.package_sha256, 32);
    memcpy(receipt.package_sha256, source.package_sha256, 32);
    if (conflicting_package != NULL) {
        assert(mode == ESP_BASE_OTA_PACKAGE_WRITE && !corrupt_write && !recover_before_stage);
        receipt.package_size_bytes = (uint32_t)conflicting_package->size;
        assert(SHA256(conflicting_package->bytes, conflicting_package->size,
                      receipt.package_sha256) != NULL &&
               memcmp(receipt.package_sha256, source.package_sha256, 32) != 0);
    }
    memcpy(receipt.trial_event_sha256, trial_event_digest, 32);
    const unsigned before_stop_writes = store.blob_writes;
    assert(esp_base_container_product_stage_firmware(&claim, &prepared, &receipt) ==
           ESP_BASE_CONTAINER_STAGE_REJECTED &&
           store.blob_writes == before_stop_writes);
    if (!recover_before_stage) {
        physical.bootable_count = 1U;
        memset(physical.bootable_firmware_sha256[1], 0, 32);
        esp_base_ota_receipt_recovery_t wrong_source = receipt;
        wrong_source.source_package_sha256[0] ^= 1U;
        if (mode == ESP_BASE_OTA_PACKAGE_REUSE) wrong_source.package_sha256[0] ^= 1U;
        const unsigned before_retirement = store.blob_writes;
        assert(esp_base_container_product_retire_inactive(&claim, &wrong_source) ==
               ESP_BASE_CONTAINER_RETIRE_BLOCKED && store.blob_writes == before_retirement);
        assert(esp_base_container_product_retire_inactive(&claim, &receipt) ==
               ESP_BASE_CONTAINER_RETIRE_COMPLETE);
        assert(atomic_load(&s_product.instance_active));
    }
    assert(esp_base_container_product_stop_confirmed(&claim));
    physical.bootable_count = 1U;
    memset(physical.bootable_firmware_sha256[1], 0, 32);
    if (recover_before_stage) {
        dispose_product();
        configure_product(key);
        const unsigned recovery_flash_erases = store.flash_erases;
        const unsigned recovery_flash_writes = store.flash_writes;
        assert(esp_base_container_product_recover_retired_firmware(
            &claim, &receipt, "88888888-8888-4888-8888-888888888888") ==
            ESP_BASE_CONTAINER_RETIRE_COMPLETE);
        assert(econtainer_slots_load(&io, &geometry, &state) == ECONTAINER_SLOTS_OK &&
               state.phase == (source_only ? ECONTAINER_SLOT_CONFIRMED : ECONTAINER_SLOT_IDLE) &&
               state.sequence == retired_sequence &&
               store.flash_erases == recovery_flash_erases &&
               store.flash_writes == recovery_flash_writes);
        const int recovered_index = binding_index(&state, receipt.source_sha256);
        assert(recovered_index >= 0 &&
               state.bindings[recovered_index].package_present &&
               memcmp(state.bindings[recovered_index].package_sha256,
                      receipt.source_package_sha256, 32) == 0);
        goto finished;
    }
    assert(econtainer_slots_load(&io, &geometry, &state) == ECONTAINER_SLOTS_OK &&
           state.sequence == retired_sequence);
    const unsigned before_stage_writes = store.blob_writes;
    const unsigned before_stage_erases = store.flash_erases;
    const unsigned before_stage_flash_writes = store.flash_writes;
    --receipt.container_sequence;
    assert(esp_base_container_product_stage_firmware(&claim, &prepared, &receipt) ==
           ESP_BASE_CONTAINER_STAGE_REJECTED &&
           store.blob_writes == before_stage_writes);
    ++receipt.container_sequence;
    const uint32_t source_offset = geometry.slots[source.slot].offset_bytes -
                                   FLASH_BASE;
    store.flash[source_offset] ^= 1U;
    assert(esp_base_container_product_stage_firmware(&claim, &prepared, &receipt) ==
           ESP_BASE_CONTAINER_STAGE_REJECTED &&
           store.blob_writes == before_stage_writes);
    store.flash[source_offset] ^= 1U;
    assert(stage_receipt_valid(&receipt));
    assert(atomic_load(&s_product.result) == ESP_BASE_CONTAINER_STOPPED &&
           !s_product.thread_joinable && s_product.stop_succeeded &&
           s_product.native_reclaimed && !atomic_load(&s_product.instance_active));
    const esp_base_container_stage_result_t stage_result =
        esp_base_container_product_stage_firmware(&claim, &prepared, &receipt);
    assert(stage_result ==
           (mode == ESP_BASE_OTA_PACKAGE_REUSE ?
            ESP_BASE_CONTAINER_STAGE_PREPARED :
            ESP_BASE_CONTAINER_STAGE_WRITING));
    uint8_t expected_operation_id[ECONTAINER_SLOT_OPERATION_ID_BYTES];
    assert(decode_uuid(receipt.operation_id, expected_operation_id));
    assert(econtainer_slots_load(&io, &geometry, &state) == ECONTAINER_SLOTS_OK &&
           state.sequence == retired_sequence + 1U &&
           state.phase == (mode == ESP_BASE_OTA_PACKAGE_REUSE ?
                           ECONTAINER_SLOT_PREPARED : ECONTAINER_SLOT_WRITING) &&
           state.operation.kind == (mode == ESP_BASE_OTA_PACKAGE_REUSE ?
                                    ECONTAINER_SLOT_PACKAGE_REUSE :
                                    ECONTAINER_SLOT_PACKAGE_WRITE) &&
           (mode == ESP_BASE_OTA_PACKAGE_REUSE ?
            state.operation.slot == source.slot :
            state.operation.slot != source.slot) &&
           memcmp(state.operation.operation_id, expected_operation_id,
                  sizeof expected_operation_id) == 0 &&
           memcmp(state.operation.package_sha256, receipt.package_sha256, 32) == 0 &&
           store.blob_writes == before_stage_writes + 1U &&
           store.flash_erases == before_stage_erases &&
           store.flash_writes == before_stage_flash_writes);
    assert(esp_base_container_product_stage_firmware(&claim, &prepared, &receipt) ==
           ESP_BASE_CONTAINER_STAGE_REJECTED &&
           store.blob_writes == before_stage_writes + 1U);
    if (mode == ESP_BASE_OTA_PACKAGE_WRITE) {
        const unsigned before_write_erases = store.flash_erases;
        const unsigned before_write_flash_writes = store.flash_writes;
        esp_base_ota_receipt_recovery_t wrong = receipt;
        strcpy(wrong.operation_id, "99999999-9999-4999-8999-999999999996");
        begin_write_workspace_observation(false, false);
        assert(esp_base_container_product_write_staged_firmware_package(
            &claim, &prepared, &wrong, read_source, (void *)package) ==
            ESP_BASE_CONTAINER_STAGE_REJECTED);
        end_write_workspace_observation(0U, 0U, 0U);
        assert(store.flash_erases == before_write_erases &&
               store.flash_writes == before_write_flash_writes);
        const uint32_t target_offset = geometry.slots[state.operation.slot].offset_bytes -
                                       FLASH_BASE;
        if (conflicting_package != NULL) {
            /* WRITE has already stopped/reclaimed the source by its existing
             * contract. The new admission gate must leave WRITING unresolved,
             * preserve the source and use the original recovery path. */
            begin_write_workspace_observation(false, false);
            assert(esp_base_container_product_write_staged_firmware_package(
                &claim, &prepared, &receipt, read_source, (void *)conflicting_package) ==
                ESP_BASE_CONTAINER_STAGE_UNCERTAIN);
            end_write_workspace_observation(1U, 1U, 1U << source_index);
            assert(econtainer_slots_load(&io, &geometry, &state) == ECONTAINER_SLOTS_OK &&
                   state.phase == ECONTAINER_SLOT_WRITING &&
                   state.sequence == retired_sequence + 1U &&
                   same_binding(&source, &state.bindings[source_index]) &&
                   !s_product.thread_joinable && s_product.native_reclaimed &&
                   !atomic_load(&s_product.instance_active) &&
                   store.flash_erases == before_write_erases + 1U &&
                   store.flash_writes > before_write_flash_writes &&
                   memcmp(store.flash + source_offset, package->bytes, package->size) == 0 &&
                   memcmp(store.flash + target_offset, conflicting_package->bytes,
                          conflicting_package->size) == 0);
            const unsigned erases = store.flash_erases;
            const unsigned writes = store.flash_writes;
            dispose_product();
            configure_product(key);
            assert(esp_base_container_product_recover_retired_firmware(
                &claim, &receipt, "88888888-8888-4888-8888-888888888888") ==
                ESP_BASE_CONTAINER_RETIRE_COMPLETE);
            assert(econtainer_slots_load(&io, &geometry, &state) == ECONTAINER_SLOTS_OK &&
                   state.phase == ECONTAINER_SLOT_IDLE &&
                   state.sequence == retired_sequence + 3U &&
                   store.flash_erases == erases && store.flash_writes == writes &&
                   memcmp(store.flash + source_offset, package->bytes, package->size) == 0);
            const int retained = binding_index(&state, receipt.source_sha256);
            assert(retained >= 0 && same_binding(&source, &state.bindings[retained]));
            const unsigned recovered_writes = store.blob_writes;
            assert(esp_base_container_product_recover_retired_firmware(
                &claim, &receipt, "88888888-8888-4888-8888-888888888888") ==
                ESP_BASE_CONTAINER_RETIRE_COMPLETE && store.blob_writes == recovered_writes);
            dispose_product();
            configure_product(key);
            assert(esp_base_container_product_without_ota_receipt(&claim));
            assert(esp_base_container_product_boot(&claim, boot_id) == ESP_BASE_CONTAINER_RUNNING);
            check_active_product(&claim, &expected_confirmed_version, false, NULL);
            assert(esp_base_container_product_stop_confirmed(&claim));
            goto finished;
        }
        if (corrupt_write || write_failure != 0U) {
            begin_write_workspace_observation(write_failure == 1U, false);
            assert(esp_base_container_product_write_staged_firmware_package(
                &claim, &prepared, &receipt,
                write_failure == 1U ? read_source :
                write_failure == 2U ? read_failed_source : read_corrupt_source,
                (void *)package) ==
                ESP_BASE_CONTAINER_STAGE_UNCERTAIN);
            end_write_workspace_observation(write_failure == 1U ? 1U : 0U, 0U, 0U);
            assert(econtainer_slots_load(&io, &geometry, &state) == ECONTAINER_SLOTS_OK &&
                   state.phase == ECONTAINER_SLOT_WRITING &&
                   state.sequence == retired_sequence + 1U &&
                   same_binding(&source, &state.bindings[source_index]) &&
                   esp_base_storage_claim_active(&claim) &&
                   store.flash_erases == before_write_erases + 1U &&
                   store.flash_writes > before_write_flash_writes &&
                   memcmp(store.flash + source_offset, package->bytes,
                          package->size) == 0);
            dispose_product();
            configure_product(key);
            const unsigned recovery_flash_erases = store.flash_erases;
            const unsigned recovery_flash_writes = store.flash_writes;
            assert(esp_base_container_product_recover_retired_firmware(
                &claim, &receipt, "88888888-8888-4888-8888-888888888888") ==
                ESP_BASE_CONTAINER_RETIRE_COMPLETE);
            assert(econtainer_slots_load(&io, &geometry, &state) == ECONTAINER_SLOTS_OK &&
                   state.phase == ECONTAINER_SLOT_IDLE &&
                   state.sequence == retired_sequence + 3U &&
                   store.flash_erases == recovery_flash_erases &&
                   store.flash_writes == recovery_flash_writes &&
                   memcmp(store.flash + source_offset, package->bytes,
                          package->size) == 0);
            const int retained = binding_index(&state, receipt.source_sha256);
            assert(retained >= 0 && same_binding(&source, &state.bindings[retained]) &&
                   esp_base_storage_claim_active(&claim));
            assert(esp_base_container_product_without_ota_receipt(&claim));
            assert(esp_base_container_product_boot(&claim, boot_id) == ESP_BASE_CONTAINER_RUNNING);
            check_active_product(&claim, &expected_confirmed_version, false, NULL);
            assert(esp_base_container_product_stop_confirmed(&claim));
            goto finished;
        }
        begin_write_workspace_observation(false, false);
        assert(esp_base_container_product_write_staged_firmware_package(
            &claim, &prepared, &receipt, read_source, (void *)package) ==
            ESP_BASE_CONTAINER_STAGE_PREPARED);
        end_write_workspace_observation(1U, 1U, 0U);
        assert(econtainer_slots_load(&io, &geometry, &state) == ECONTAINER_SLOTS_OK &&
               state.phase == ECONTAINER_SLOT_PREPARED &&
               state.sequence == retired_sequence + 2U &&
               store.flash_erases == before_write_erases + 1U &&
               store.flash_writes > before_write_flash_writes &&
               memcmp(store.flash + target_offset, package->bytes,
                      package->size) == 0 &&
               memcmp(store.flash + source_offset, package->bytes,
                      package->size) == 0);
        const unsigned completed_writes = store.flash_writes;
        assert(esp_base_container_product_write_staged_firmware_package(
            &claim, &prepared, &receipt, read_source, (void *)package) ==
            ESP_BASE_CONTAINER_STAGE_REJECTED &&
            store.flash_writes == completed_writes);
    }
    physical.bootable_count = 2U;
    memcpy(physical.bootable_firmware_sha256[0], candidate, 32);
    memcpy(physical.bootable_firmware_sha256[1], receipt.source_sha256, 32);
    memcpy(physical.running_firmware_sha256, candidate, 32);
    dispose_product();
    configure_product(key);
    const unsigned selected_blob_writes = store.blob_writes;
    const unsigned selected_flash_erases = store.flash_erases;
    const unsigned selected_flash_writes = store.flash_writes;
    assert(esp_base_container_product_reconcile_selected_ota(
        &claim, &receipt, EOTA_STATE_PENDING_VERIFY));
    assert(!esp_base_container_product_reconcile_selected_ota(
        &claim, &receipt, EOTA_STATE_VALID));
    esp_base_ota_receipt_recovery_t wrong_selected = receipt;
    strcpy(wrong_selected.operation_id, "99999999-9999-4999-8999-999999999996");
    assert(!esp_base_container_product_reconcile_selected_ota(
        &claim, &wrong_selected, EOTA_STATE_PENDING_VERIFY));
    wrong_selected = receipt;
    ++wrong_selected.package_size_bytes;
    assert(!esp_base_container_product_reconcile_selected_ota(
        &claim, &wrong_selected, EOTA_STATE_PENDING_VERIFY));
    wrong_selected = receipt;
    wrong_selected.source_package_sha256[0] ^= 1U;
    assert(!esp_base_container_product_reconcile_selected_ota(
        &claim, &wrong_selected, EOTA_STATE_PENDING_VERIFY));
    assert(econtainer_slots_load(&io, &geometry, &state) == ECONTAINER_SLOTS_OK);
    const uint32_t selected_offset =
        geometry.slots[state.operation.slot].offset_bytes - FLASH_BASE;
    store.flash[selected_offset] ^= 1U;
    assert(!esp_base_container_product_reconcile_selected_ota(
        &claim, &receipt, EOTA_STATE_PENDING_VERIFY));
    store.flash[selected_offset] ^= 1U;
    assert(esp_base_container_product_reconcile_selected_ota(
        &claim, &receipt, EOTA_STATE_PENDING_VERIFY));
    assert(store.blob_writes == selected_blob_writes &&
           store.flash_erases == selected_flash_erases &&
           store.flash_writes == selected_flash_writes);
    if (live_trial != 0U) {
        reject_confirmed_observation = true; /* Fake otadata is still pending C. */
        assert(esp_base_container_product_start_firmware_package_trial(
            &claim, &wrong_selected, boot_id) == ESP_BASE_CONTAINER_BLOCKED);
        assert(store.blob_writes == selected_blob_writes);
        assert(esp_base_container_product_start_firmware_package_trial(
            &claim, &receipt, boot_id) == ESP_BASE_CONTAINER_RUNNING);
        assert(econtainer_slots_load(&io, &geometry, &state) == ECONTAINER_SLOTS_OK &&
               state.phase == ECONTAINER_SLOT_TRIAL_STARTED &&
               state.sequence == retired_sequence - 1U +
                   (mode == ESP_BASE_OTA_PACKAGE_WRITE ? 4U : 3U));
        assert(!esp_base_container_product_mark_healthy(&claim));
        assert(!esp_base_container_product_confirm_firmware(&claim));
        const unsigned before_health = store.blob_writes;
        if (live_trial == 4U) {
            const uint8_t trapped_event[] = {1U};
            assert(offer_event_when_available(receipt.package_sha256, 1U, trapped_event,
                sizeof trapped_event) == ESP_BASE_CONTAINER_EVENT_ACCEPTED);
            for (unsigned attempt = 0;
                 attempt < 500U && esp_base_container_product_event_accepting();
                 ++attempt) vTaskDelay(1U);
            assert(!esp_base_container_product_event_accepting());
            assert(esp_base_container_product_verify_firmware_package_health(
                &claim, &receipt, 1U, 0U) == ESP_BASE_CONTAINER_HEALTH_NOT_STARTED);
            assert(esp_base_container_product_stop_trial(&claim) &&
                   s_product.native_reclaimed && !s_product.thread_joinable &&
                   s_product.trial_failure_count == 1U &&
                   s_product.representative_event_sequence == 0U);
            assert(store.blob_writes == before_health);
            reject_confirmed_observation = false;
            goto rollback_live_trial;
        }
        assert(esp_base_container_product_verify_firmware_package_health(
            &claim, &receipt, 1U, 0U) == ESP_BASE_CONTAINER_HEALTH_NOT_STARTED);
        const uint8_t wrong_event[] = {4U, 5U, 6U};
        const uint8_t representative[] = {1U, 2U, 3U};
        assert(offer_event_when_available(receipt.package_sha256, 1U, wrong_event, sizeof wrong_event) == ESP_BASE_CONTAINER_EVENT_ACCEPTED);
        for (unsigned attempt = 0;
             attempt < 200U && esp_base_container_product_event_progress_count() < 1U;
             ++attempt) vTaskDelay(1U);
        esp_base_container_trial_event_snapshot_t snapshot = {0};
        assert(wait_trial_snapshot(&snapshot) &&
               snapshot.representative_event_sequence == 0U);
        assert(esp_base_container_product_verify_firmware_package_health(
            &claim, &receipt, 1U, 0U) == ESP_BASE_CONTAINER_HEALTH_NOT_STARTED);
        assert(offer_event_when_available(receipt.package_sha256, 2U, representative, sizeof representative) == ESP_BASE_CONTAINER_EVENT_ACCEPTED);
        for (unsigned attempt = 0;
             attempt < 200U && esp_base_container_product_event_progress_count() < 2U;
             ++attempt) vTaskDelay(1U);
        assert(wait_trial_snapshot(&snapshot) &&
               snapshot.representative_event_sequence == 2U && snapshot.failure_count == 0U);
        assert(esp_base_container_product_verify_firmware_package_health(
            &claim, &receipt, 1U, 0U) == ESP_BASE_CONTAINER_HEALTH_NOT_STARTED);
        assert(esp_base_container_product_verify_firmware_package_health(
            &claim, &receipt, 2U, 1U) == ESP_BASE_CONTAINER_HEALTH_NOT_STARTED);
        wrong_selected = receipt;
        wrong_selected.trial_event_sha256[0] ^= 1U;
        assert(esp_base_container_product_verify_firmware_package_health(
            &claim, &wrong_selected, 2U, 0U) == ESP_BASE_CONTAINER_HEALTH_NOT_STARTED);
        assert(store.blob_writes == before_health);
        if (live_trial == 2U) store.fail_read_after_next_write = 1U;
        esp_base_container_trial_health_result_t health = ESP_BASE_CONTAINER_HEALTH_NOT_STARTED;
        for (unsigned attempt = 0;
             attempt < 200U && health == ESP_BASE_CONTAINER_HEALTH_NOT_STARTED; ++attempt) {
            vTaskDelay(1U);
            health = esp_base_container_product_verify_firmware_package_health(
                &claim, &receipt, 2U, 0U);
        }
        assert(health == (live_trial == 2U ? ESP_BASE_CONTAINER_HEALTH_UNCERTAIN :
                                          ESP_BASE_CONTAINER_HEALTH_VERIFIED));
        assert(econtainer_slots_load(&io, &geometry, &state) == ECONTAINER_SLOTS_OK &&
               state.phase == ECONTAINER_SLOT_HEALTH_VERIFIED &&
               store.blob_writes == before_health + 1U &&
               !esp_base_container_product_event_accepting());
        assert(offer_event(receipt.package_sha256, 3U, representative, sizeof representative) ==
               ESP_BASE_CONTAINER_EVENT_UNAVAILABLE);
        assert(store.flash_erases == selected_flash_erases &&
               store.flash_writes == selected_flash_writes);
        reject_confirmed_observation = false; /* Base observes OTA VALID before confirm. */
        if (live_trial != 2U) {
            if (live_trial == 3U) store.fail_read_after_next_write = 1U;
            assert(esp_base_container_product_confirm_firmware(&claim) == (live_trial != 3U));
            assert(econtainer_slots_load(&io, &geometry, &state) == ECONTAINER_SLOTS_OK &&
                   state.phase == ECONTAINER_SLOT_CONFIRMED &&
                   store.blob_writes == before_health + 2U);
            if (live_trial == 3U) {
                assert(!esp_base_container_product_event_accepting());
                assert(esp_base_container_product_stop_trial(&claim));
                dispose_product();
                configure_product(key);
                const unsigned committed_writes = store.blob_writes;
                assert(esp_base_container_product_reconcile_selected_ota(
                    &claim, &receipt, EOTA_STATE_VALID) &&
                    store.blob_writes == committed_writes);
                assert(esp_base_container_product_boot(
                    &claim, "88888888-8888-4888-8888-888888888888") == ESP_BASE_CONTAINER_RUNNING);
            } else {
                assert(esp_base_container_product_event_accepting() &&
                       !esp_base_container_product_trial_event_snapshot(&snapshot));
                assert(offer_event_when_available(receipt.package_sha256, 3U, representative,
                    sizeof representative) == ESP_BASE_CONTAINER_EVENT_ACCEPTED);
            }
            assert(esp_base_container_product_stop_confirmed(&claim));
            goto finished;
        }
        assert(esp_base_container_product_stop_trial(&claim));
        /* HEALTH_VERIFIED readback was uncertain: remain pending and return
         * to A, never call firmware confirm with unproved health. */
    }
    if (live_trial == 0U && (mode == ESP_BASE_OTA_PACKAGE_REUSE || recover_valid)) {
        uint8_t trial_boot_id[ECONTAINER_SLOT_BOOT_ID_BYTES];
        assert(decode_uuid(boot_id, trial_boot_id));
        assert(econtainer_slots_begin_trial(&io, &geometry, state.sequence,
                   candidate, trial_boot_id, &state) == ECONTAINER_SLOTS_OK);
        if (recover_valid) {
            assert(!esp_base_container_product_reconcile_selected_ota(
                &claim, &receipt, EOTA_STATE_VALID));
        }
        assert(econtainer_slots_mark_healthy(&io, &geometry, state.sequence,
                   trial_boot_id, &state) == ECONTAINER_SLOTS_OK &&
               state.phase == ECONTAINER_SLOT_HEALTH_VERIFIED);
    }
    if (recover_valid) {
        /* Model a cold boot after OTA VALID. HEALTH_VERIFIED was persisted
         * by the prior trial; this checks commit recovery, not MQTT health. */
        dispose_product();
        configure_product(key);
        const unsigned before_confirm = store.blob_writes;
        wrong_selected = receipt;
        strcpy(wrong_selected.operation_id, "99999999-9999-4999-8999-999999999996");
        assert(!esp_base_container_product_reconcile_selected_ota(
            &claim, &wrong_selected, EOTA_STATE_VALID));
        wrong_selected = receipt;
        wrong_selected.package_sha256[0] ^= 1U;
        assert(!esp_base_container_product_reconcile_selected_ota(
            &claim, &wrong_selected, EOTA_STATE_VALID));
        wrong_selected = receipt;
        wrong_selected.source_package_sha256[0] ^= 1U;
        assert(!esp_base_container_product_reconcile_selected_ota(
            &claim, &wrong_selected, EOTA_STATE_VALID));
        store.flash[selected_offset] ^= 1U;
        assert(!esp_base_container_product_reconcile_selected_ota(
            &claim, &receipt, EOTA_STATE_VALID));
        store.flash[selected_offset] ^= 1U;
        assert(store.blob_writes == before_confirm);
        store.fail_read_after_next_write = 1U;
        assert(!esp_base_container_product_reconcile_selected_ota(
            &claim, &receipt, EOTA_STATE_VALID));
        assert(econtainer_slots_load(&io, &geometry, &state) == ECONTAINER_SLOTS_OK &&
               state.phase == ECONTAINER_SLOT_CONFIRMED &&
               state.sequence == retired_sequence - 1U +
                   (mode == ESP_BASE_OTA_PACKAGE_WRITE ? 6U : 5U) &&
               store.blob_writes == before_confirm + 1U);
        const unsigned confirmed_writes = store.blob_writes;
        assert(esp_base_container_product_reconcile_selected_ota(
            &claim, &receipt, EOTA_STATE_VALID));
        receipt.status = ESP_BASE_OTA_RECEIPT_SUCCEEDED;
        assert(esp_base_container_product_reconcile_selected_ota(
            &claim, &receipt, EOTA_STATE_VALID));
        assert(store.blob_writes == confirmed_writes &&
               store.flash_erases == selected_flash_erases &&
               store.flash_writes == selected_flash_writes);
        const int candidate_index = binding_index(&state, candidate);
        const int old_index = binding_index(&state, receipt.source_sha256);
        assert(candidate_index >= 0 && old_index >= 0 &&
               state.bindings[candidate_index].package_present &&
               state.bindings[old_index].package_present &&
               memcmp(state.bindings[candidate_index].package_sha256,
                      receipt.package_sha256, 32) == 0 &&
               memcmp(state.bindings[old_index].package_sha256,
                      receipt.source_package_sha256, 32) == 0);
        assert(esp_base_container_product_boot(
            &claim, "88888888-8888-4888-8888-888888888888") ==
            ESP_BASE_CONTAINER_RUNNING);
        assert(esp_base_container_product_stop_confirmed(&claim));
        assert(store.blob_writes == confirmed_writes);
        /* Historical success remains usable after a real product-only
         * replacement has advanced C's durable operation. */
        install_context_t later = {.package = package, .operation_marker = 0x98};
        assert(esp_base_container_with_firmware_set(&claim,
            ESP_BASE_OTA_FIRMWARE_CONFIRMED, NULL, install_signed, &later) ==
            ECONTAINER_SLOTS_OK);
        dispose_product();
        configure_product(key);
        assert(esp_base_container_product_reconcile_selected_ota(
            &claim, &receipt, EOTA_STATE_VALID));
        receipt.status = ESP_BASE_OTA_RECEIPT_PREPARED;
        assert(!esp_base_container_product_reconcile_selected_ota(
            &claim, &receipt, EOTA_STATE_VALID));
        receipt.status = ESP_BASE_OTA_RECEIPT_SUCCEEDED;
        assert(econtainer_slots_load(&io, &geometry, &state) == ECONTAINER_SLOTS_OK);
        const uint32_t later_offset = geometry.slots[state.operation.slot].offset_bytes - FLASH_BASE;
        store.flash[later_offset] ^= 1U;
        assert(!esp_base_container_product_reconcile_selected_ota(
            &claim, &receipt, EOTA_STATE_VALID));
        store.flash[later_offset] ^= 1U;
        goto finished;
    }
rollback_live_trial:
    physical.bootable_count = 1U;
    memcpy(physical.running_firmware_sha256, receipt.source_sha256, 32);
    memcpy(physical.bootable_firmware_sha256[0], receipt.source_sha256, 32);
    memset(physical.bootable_firmware_sha256[1], 0, 32);
    dispose_product();
    configure_product(key);
    const unsigned recovery_blob_writes = store.blob_writes;
    esp_base_ota_receipt_recovery_t wrong_recovery = receipt;
    wrong_recovery.source_package_sha256[0] ^= 1U;
    assert(esp_base_container_product_recover_retired_firmware(
        &claim, &wrong_recovery, "88888888-8888-4888-8888-888888888888") ==
        ESP_BASE_CONTAINER_RETIRE_BLOCKED);
    assert(store.blob_writes == recovery_blob_writes);
    store.flash[source_offset] ^= 1U;
    assert(esp_base_container_product_recover_retired_firmware(
        &claim, &receipt, "88888888-8888-4888-8888-888888888888") ==
        ESP_BASE_CONTAINER_RETIRE_BLOCKED);
    store.flash[source_offset] ^= 1U;
    assert(store.blob_writes == recovery_blob_writes);
    if (mode == ESP_BASE_OTA_PACKAGE_REUSE) {
        assert(esp_base_container_product_recover_retired_firmware(
            &claim, &receipt, boot_id) == ESP_BASE_CONTAINER_RETIRE_BLOCKED);
        assert(store.blob_writes == recovery_blob_writes);
    }
    if (mode == ESP_BASE_OTA_PACKAGE_WRITE)
        store.flash[selected_offset] ^= 1U;
    const unsigned recovery_flash_erases = store.flash_erases;
    const unsigned recovery_flash_writes = store.flash_writes;
    assert(esp_base_container_product_recover_retired_firmware(
        &claim, &receipt, "88888888-8888-4888-8888-888888888888") ==
        ESP_BASE_CONTAINER_RETIRE_COMPLETE);
    assert(econtainer_slots_load(&io, &geometry, &state) == ECONTAINER_SLOTS_OK &&
           state.phase == ECONTAINER_SLOT_IDLE &&
           state.sequence == retired_sequence - 1U +
                             (mode == ESP_BASE_OTA_PACKAGE_WRITE ?
                              (live_trial == 4U ? 6U : live_trial != 0U ? 7U : 5U) :
                              (live_trial == 4U ? 5U : 6U)) &&
           store.flash_erases == recovery_flash_erases &&
           store.flash_writes == recovery_flash_writes &&
           memcmp(store.flash + source_offset, package->bytes,
                  package->size) == 0);
    const unsigned recovered_blob_writes = store.blob_writes;
    assert(esp_base_container_product_recover_retired_firmware(
        &claim, &receipt, "88888888-8888-4888-8888-888888888888") ==
        ESP_BASE_CONTAINER_RETIRE_COMPLETE);
    assert(store.blob_writes == recovered_blob_writes);
finished:
    assert(esp_base_storage_release(&claim));
    dispose_product();
    assert(pthread_mutex_destroy(&store.mutex) == 0);
}

static void run_firmware_package_stage(const file_t *key, const file_t *package,
                                       const char boot_id[37],
                                       esp_base_ota_package_mode_t mode,
                                       bool corrupt_write, bool recover_before_stage,
                                       bool recover_valid, unsigned live_trial, bool source_only)
{
    run_firmware_package_stage_internal(key, package, NULL, boot_id, mode,
        corrupt_write, recover_before_stage, recover_valid, live_trial, source_only, 0U);
}

static void run_package_identity_fallback(const file_t *key, const file_t *confirmed,
                                           const file_t *conflicting, const char boot_id[37])
{
    /* Prove the re-signed candidate itself passes the real validator before
     * testing its conflict against a protected reference. */
    configure(key);
    esp_base_storage_owner_t owner;
    esp_base_storage_owner_init(&owner);
    esp_base_storage_claim_t claim = {0};
    assert(esp_base_storage_claim(&owner, &claim));
    assert(esp_base_container_product_boot(&claim, boot_id) == ESP_BASE_CONTAINER_EMPTY);
    install_context_t resigned = {.package = conflicting, .operation_marker = 0xa1};
    assert(esp_base_container_with_firmware_set(&claim,
        ESP_BASE_OTA_FIRMWARE_CONFIRMED, NULL, install_signed, &resigned) == ECONTAINER_SLOTS_OK);
    assert(esp_base_storage_release(&claim));
    dispose_product();
    assert(pthread_mutex_destroy(&store.mutex) == 0);

    configure(key);
    esp_base_storage_owner_init(&owner);
    assert(esp_base_storage_claim(&owner, &claim));
    assert(esp_base_container_product_boot(&claim, boot_id) == ESP_BASE_CONTAINER_EMPTY);
    install_context_t install = {.package = confirmed, .operation_marker = 0xa2};
    assert(esp_base_container_with_firmware_set(&claim,
        ESP_BASE_OTA_FIRMWARE_CONFIRMED, NULL, install_signed, &install) == ECONTAINER_SLOTS_OK);
    assert(esp_base_container_product_boot(&claim, boot_id) == ESP_BASE_CONTAINER_RUNNING);
    assert(esp_base_container_product_stop_confirmed(&claim));
    dispose_product();
    uint8_t source_firmware[32];
    memcpy(source_firmware, physical.running_firmware_sha256, sizeof source_firmware);
    memcpy(physical.running_firmware_sha256, physical.bootable_firmware_sha256[1], 32);
    memcpy(physical.bootable_firmware_sha256[0], physical.running_firmware_sha256, 32);
    memcpy(physical.bootable_firmware_sha256[1], source_firmware, 32);
    configure_product(key);
    assert(esp_base_container_product_boot(&claim, boot_id) == ESP_BASE_CONTAINER_EMPTY);
    econtainer_slots_state_t before = {0}, after = {0};
    assert(econtainer_slots_load(&io, &geometry, &before) == ECONTAINER_SLOTS_OK);
    const int fallback = binding_index(&before, source_firmware);
    const int current = binding_index(&before, physical.running_firmware_sha256);
    assert(fallback >= 0 && current >= 0 && before.bindings[fallback].package_present &&
           !before.bindings[current].package_present);
    esp_base_container_package_request_t request = {
        .operation_id = "99999999-9999-4999-8999-9999999999a3",
        .expected_sequence = before.sequence,
        .package_size_bytes = (uint32_t)conflicting->size,
        .guest_abi_version = 2U, .data_schema_version = 1U,
    };
    assert(SHA256(conflicting->bytes, conflicting->size, request.package_sha256) != NULL &&
           memcmp(request.package_sha256, before.bindings[fallback].package_sha256, 32) != 0);
    uint32_t aborted_sequence = UINT32_MAX;
    assert(esp_base_container_product_prepare_package(&claim, &request,
        read_source, (void *)conflicting, &aborted_sequence) == ESP_BASE_CONTAINER_PREPARE_REJECTED);
    assert(econtainer_slots_load(&io, &geometry, &after) == ECONTAINER_SLOTS_OK &&
           after.phase == ECONTAINER_SLOT_ABORTED && after.sequence == before.sequence + 2U &&
           aborted_sequence == after.sequence &&
           same_binding(&before.bindings[0], &after.bindings[0]) &&
           same_binding(&before.bindings[1], &after.bindings[1]) &&
           !s_product.thread_joinable && !atomic_load(&s_product.instance_active));
    const size_t source_offset = geometry.slots[before.bindings[fallback].slot].offset_bytes - FLASH_BASE;
    assert(memcmp(store.flash + source_offset, confirmed->bytes, confirmed->size) == 0);
    /* The original failure cleanup must still allow reopening the exact old
     * confirmed guest when its own firmware is selected again. */
    dispose_product();
    memcpy(physical.bootable_firmware_sha256[1], physical.running_firmware_sha256, 32);
    memcpy(physical.bootable_firmware_sha256[0], source_firmware, 32);
    memcpy(physical.running_firmware_sha256, source_firmware, 32);
    configure_product(key);
    assert(esp_base_container_product_boot(&claim, boot_id) == ESP_BASE_CONTAINER_RUNNING);
    check_active_product(&claim, &expected_confirmed_version, false, NULL);
    assert(esp_base_container_product_stop_confirmed(&claim));
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


static void run_public_product_lifecycle(const file_t *key, const file_t *package,
    const char boot_id[37])
{
    configure(key);
    esp_base_storage_owner_t owner;
    esp_base_storage_owner_init(&owner);
    esp_base_storage_claim_t claim = {0};
    assert(esp_base_storage_claim(&owner, &claim));
    assert(esp_base_container_product_boot(&claim, boot_id) == ESP_BASE_CONTAINER_EMPTY);
    uint8_t sha[32];
    assert(SHA256(package->bytes, package->size, sha) != NULL);
    assert(esp_base_container_product_set_running(&claim, true, boot_id, 1U, sha) ==
        ESP_BASE_CONTAINER_RUN_REJECTED);
    install_context_t install = {.package = package};
    assert(esp_base_container_with_firmware_set(&claim, ESP_BASE_OTA_FIRMWARE_CONFIRMED,
        NULL, install_signed, &install) == ECONTAINER_SLOTS_OK);
    assert(esp_base_container_product_boot(&claim, boot_id) == ESP_BASE_CONTAINER_RUNNING);
    esp_base_container_binding_snapshot_t binding = {0};
    assert(esp_base_container_product_binding_snapshot(&claim, &binding) ==
        ESP_BASE_CONTAINER_BINDING_OK && memcmp(binding.package_sha256, sha, 32U) == 0);
    const unsigned blob_writes = store.blob_writes, flash_writes = store.flash_writes,
        flash_erases = store.flash_erases;
    uint8_t blob[sizeof store.blob], flash[sizeof store.flash];
    memcpy(blob, store.blob, sizeof blob);
    memcpy(flash, store.flash, sizeof flash);
    uint8_t wrong_sha[32]; memcpy(wrong_sha, sha, sizeof sha); wrong_sha[0] ^= 1U;
    assert(esp_base_container_product_set_running(NULL, false, boot_id,
        binding.container_sequence, sha) == ESP_BASE_CONTAINER_RUN_BUSY);
    assert(esp_base_container_product_set_running(&claim, false, boot_id,
        binding.container_sequence + 1U, sha) == ESP_BASE_CONTAINER_RUN_REJECTED);
    assert(esp_base_container_product_set_running(&claim, false, boot_id,
        binding.container_sequence, wrong_sha) == ESP_BASE_CONTAINER_RUN_REJECTED);
    assert(esp_base_container_product_set_running(&claim, false,
        "33333333-3333-4333-8333-333333333333", binding.container_sequence, sha) ==
        ESP_BASE_CONTAINER_RUN_REJECTED);
    s_product.trial_mode = true;
    assert(esp_base_container_product_set_running(&claim, false, boot_id,
        binding.container_sequence, sha) == ESP_BASE_CONTAINER_RUN_REJECTED);
    s_product.trial_mode = false;
    for (unsigned cycle = 0; cycle < 3U; ++cycle) {
        assert(esp_base_container_product_set_running(&claim, true, boot_id,
            binding.container_sequence, sha) == ESP_BASE_CONTAINER_RUN_COMPLETE);
        check_active_product(&claim, &expected_confirmed_version, false, NULL);
        assert(esp_base_container_product_set_running(&claim, false, boot_id,
            binding.container_sequence, sha) == ESP_BASE_CONTAINER_RUN_COMPLETE);
        assert(confirmed_stopped() && s_product.event_count == 0U &&
            !s_product.guest_call_processing && store.mapping == NULL && !store.locked);
        assert(esp_base_container_product_set_running(&claim, false, boot_id,
            binding.container_sequence, sha) == ESP_BASE_CONTAINER_RUN_COMPLETE);
        assert(esp_base_container_product_set_running(&claim, true, boot_id,
            binding.container_sequence, sha) == ESP_BASE_CONTAINER_RUN_COMPLETE);
        const uint8_t event[] = {1U, 2U, 3U};
        assert(offer_event_when_available(sha, 1U, event, sizeof event) ==
            ESP_BASE_CONTAINER_EVENT_ACCEPTED);
        for (unsigned attempt = 0; attempt < 200U &&
             esp_base_container_product_event_progress_count() == 0U; ++attempt)
            vTaskDelay(1U);
        esp_base_container_event_observation_t observation = {0};
        assert(esp_base_container_product_event_observation(&observation) ==
            ESP_BASE_CONTAINER_EVENT_OBSERVED && observation.runtime_ok &&
            observation.guest_result == 3);
    }
    assert(esp_base_container_product_set_running(&claim, false, boot_id,
        binding.container_sequence, sha) == ESP_BASE_CONTAINER_RUN_COMPLETE);
    /* The package bytes and hash are unchanged. A different valid RSA modulus
     * must fail the fresh signature verification, proving start does not reuse
     * the previous open's validation or Wasm instance. */
    uint8_t *wrong_key = malloc(key->size);
    assert(wrong_key != NULL);
    memcpy(wrong_key, key->bytes, key->size);
    wrong_key[key->size / 2U] ^= 1U;
    s_product.validation.public_key_rsa_der = wrong_key;
    assert(esp_base_container_product_set_running(&claim, true, boot_id,
        binding.container_sequence, sha) == ESP_BASE_CONTAINER_RUN_UNCERTAIN);
    s_product.validation.public_key_rsa_der = key->bytes;
    free(wrong_key);
    assert(atomic_load(&s_product.result) == ESP_BASE_CONTAINER_BLOCKED &&
        !s_product.reopen_allowed && !s_product.thread_joinable &&
        !atomic_load(&s_product.instance_active) && !esp_base_container_product_event_accepting());
    assert(esp_base_container_product_set_running(&claim, true, boot_id,
        binding.container_sequence, sha) == ESP_BASE_CONTAINER_RUN_REJECTED);
    assert(memcmp(blob, store.blob, sizeof blob) == 0 &&
        memcmp(flash, store.flash, sizeof flash) == 0 && store.blob_writes == blob_writes &&
        store.flash_writes == flash_writes && store.flash_erases == flash_erases);
    assert(esp_base_storage_release(&claim));
    dispose_product();
    configure_product(key); /* A new boot uses the exact retained confirmed package. */
    esp_base_storage_owner_init(&owner);
    assert(esp_base_storage_claim(&owner, &claim));
    assert(esp_base_container_product_boot(&claim,
        "33333333-3333-4333-8333-333333333333") == ESP_BASE_CONTAINER_RUNNING);
    assert(esp_base_container_product_stop_confirmed(&claim));
    assert(esp_base_storage_release(&claim));
    dispose_product();
    assert(pthread_mutex_destroy(&store.mutex) == 0);
    puts("container_public_lifecycle: exact boot/package/sequence, stop/close/join, fresh RSA verification, events and no persistent writes passed");
}

#include "historical_ota_startup_test.h"

bool esp_base_wifi_ready(void) { return true; }
bool esp_base_time_ready(void) { return true; }
bool esp_base_mqtt_owner_ready(void) { return true; }

static int32_t timer_business_event(const uint8_t package_sha256[32],
    uint64_t sequence, uint8_t byte)
{
    assert(offer_event_when_available(package_sha256, sequence, &byte, 1U) ==
        ESP_BASE_CONTAINER_EVENT_ACCEPTED);
    for (unsigned attempt = 0; attempt < 200U; ++attempt) {
        esp_base_container_event_observation_t observation = {0};
        if (esp_base_container_product_event_observation(&observation) ==
                ESP_BASE_CONTAINER_EVENT_OBSERVED &&
            observation.event_sequence == sequence) {
            assert(observation.runtime_ok);
            return observation.guest_result;
        }
        vTaskDelay(1U);
    }
    assert(!"real guest event did not complete");
    return 0;
}

static void run_timer_business_trial(const char *directory, int32_t timer_result)
{
    const bool negative = timer_result < 0;
    file_t key = read_file(directory, "public.der");
    file_t package = read_file(directory,
        negative ? "timer-negative.pkg" :
        timer_result == 0 ? "timer-zero.pkg" : "timer-positive.pkg");
    configure(&key);
    esp_base_storage_owner_t owner;
    esp_base_storage_owner_init(&owner);
    esp_base_storage_claim_t claim = {0};
    assert(esp_base_storage_claim(&owner, &claim));
    const char boot_id[] = "22222222-2222-4222-8222-222222222222";
    assert(esp_base_container_product_boot(&claim, boot_id) == ESP_BASE_CONTAINER_EMPTY);
    econtainer_slots_state_t initial = {0};
    assert(econtainer_slots_load(&io, &geometry, &initial) == ECONTAINER_SLOTS_OK);
    esp_base_container_package_request_t request = {
        .operation_id = "99999999-9999-4999-8999-999999999997",
        .expected_sequence = initial.sequence,
        .package_size_bytes = (uint32_t)package.size,
        .guest_abi_version = 2U, .data_schema_version = 1U,
    };
    assert(SHA256(package.bytes, package.size, request.package_sha256) != NULL);
    const uint8_t representative = 'R';
    assert(SHA256(&representative, 1U, trial_event_digest) != NULL);
    uint32_t prepared_sequence = 0U;
    assert(esp_base_container_product_prepare_package(&claim, &request,
        read_source, &package, &prepared_sequence) == ESP_BASE_CONTAINER_PREPARED);
    assert(esp_base_container_product_start_package_trial(&claim,
        prepared_sequence, request.operation_id, boot_id, trial_event_digest) ==
        ESP_BASE_CONTAINER_RUNNING);
    const uint32_t trial_sequence = prepared_sequence + 1U;
    uint64_t sequence = 1U;
    assert(timer_business_event(request.package_sha256, sequence, 'R') == 7);
    uint64_t window_event = 0U, window_failure = 0U, stable_since = 0U, last_poll = 0U;
    for (uint64_t now = 1000U; now <= 10000U; now += 1000U)
        assert(!historical_product_health_window(now, request.package_sha256,
            &window_event, &window_failure, &stable_since, &last_poll));
    assert(window_event == 1U && stable_since == 1000U && window_failure == 0U);
    assert(timer_business_event(request.package_sha256, ++sequence, 'T') == 0);
    int32_t timer_count = 0;
    for (unsigned attempt = 0; attempt < 200U && timer_count == 0; ++attempt) {
        timer_count = timer_business_event(request.package_sha256, ++sequence, 'Q');
        if (timer_count == 0) vTaskDelay(1U);
    }
    assert(timer_count == 1);
    esp_base_container_trial_event_snapshot_t snapshot = {0};
    assert(wait_trial_snapshot(&snapshot));
    assert(atomic_load(&s_product.result) == ESP_BASE_CONTAINER_RUNNING &&
        atomic_load(&s_product.instance_active) &&
        esp_base_container_product_event_accepting());
    const unsigned writes_before = store.blob_writes;
    uint32_t confirmed_sequence = 0U;
    esp_base_container_trial_confirm_result_t stale = ESP_BASE_CONTAINER_CONFIRM_NOT_STARTED;
    for (unsigned attempt = 0; attempt < 200U &&
         stale == ESP_BASE_CONTAINER_CONFIRM_NOT_STARTED; ++attempt) {
        stale = esp_base_container_product_confirm_package_trial(&claim,
            trial_sequence, request.operation_id, 1U, trial_event_digest, 0U,
            &confirmed_sequence);
        if (stale == ESP_BASE_CONTAINER_CONFIRM_NOT_STARTED) vTaskDelay(1U);
    }
    fprintf(stderr, "timer_business result=%d runtime_running=1 calls=%d representative=%llu failures=%llu stale_confirm=%d writes=%u\n",
        timer_result, timer_count,
        (unsigned long long)snapshot.representative_event_sequence,
        (unsigned long long)snapshot.failure_count, (int)stale,
        store.blob_writes - writes_before);
    if (negative) {
        if (stale != ESP_BASE_CONTAINER_CONFIRM_NOT_STARTED ||
            confirmed_sequence != 0U || store.blob_writes != writes_before) {
            fprintf(stderr, "container_timer_business: FAIL negative timer retained stale health and allowed confirmation\n");
            exit(1);
        }
        assert(snapshot.representative_event_sequence == 0U && snapshot.failure_count == 1U);
        assert(!historical_product_health_window(11000U, request.package_sha256,
            &window_event, &window_failure, &stable_since, &last_poll));
        assert(window_event == 0U && stable_since == 0U && window_failure == 1U);
        assert(timer_business_event(request.package_sha256, ++sequence, 'R') == 7);
        assert(wait_trial_snapshot(&snapshot) && snapshot.failure_count == 1U &&
            snapshot.representative_event_sequence == sequence);
        for (uint64_t now = 12000U; now < 42000U; now += 1000U)
            assert(!historical_product_health_window(now, request.package_sha256,
                &window_event, &window_failure, &stable_since, &last_poll));
        assert(stable_since == 12000U && window_event == sequence);
        bool healthy = false;
        for (unsigned attempt = 0; attempt < 200U && !healthy; ++attempt) {
            healthy = historical_product_health_window(42000U,
                request.package_sha256, &window_event, &window_failure,
                &stable_since, &last_poll);
            if (!healthy) vTaskDelay(1U);
        }
        assert(healthy && window_failure == 1U);
        for (unsigned attempt = 0; attempt < 200U &&
             stale == ESP_BASE_CONTAINER_CONFIRM_NOT_STARTED; ++attempt) {
            stale = esp_base_container_product_confirm_package_trial(&claim,
                trial_sequence, request.operation_id, window_event,
                trial_event_digest, window_failure, &confirmed_sequence);
            if (stale == ESP_BASE_CONTAINER_CONFIRM_NOT_STARTED) vTaskDelay(1U);
        }
    } else {
        assert(snapshot.representative_event_sequence == 1U && snapshot.failure_count == 0U);
    }
    assert(stale == ESP_BASE_CONTAINER_CONFIRM_CONFIRMED &&
        confirmed_sequence == trial_sequence + 2U);
    assert(esp_base_container_product_stop_confirmed(&claim));
    assert(esp_base_storage_release(&claim));
    dispose_product();
    assert(pthread_mutex_destroy(&store.mutex) == 0);
    free(package.bytes);
    free(key.bytes);
}

int main(int argc, char **argv)
{
    check_event_admission();
    if (argc == 2 && strcmp(argv[1], "event-admission") == 0) return 0;
    const uint8_t representative_event[] = {1U, 2U, 3U};
    assert(SHA256(representative_event, sizeof representative_event,
                  trial_event_digest) != NULL);
    if (argc == 3 && strcmp(argv[2], "timer-business") == 0) {
        run_timer_business_trial(argv[1], -7);
        run_timer_business_trial(argv[1], 0);
        run_timer_business_trial(argv[1], 3);
        puts("container_timer_business: real signed negative timer, trial timer failure accounting while runtime RUNNING, stale confirmation refusal, fresh 30-second policy window and zero/positive timer passed");
        return 0;
    }
    if (argc == 3 && strcmp(argv[2], "deadline") == 0) {
        run_deadline_product(argv[1]);
        return 0;
    }
    if (argc == 3 && strcmp(argv[2], "cancel") == 0) {
        run_cancel_product(argv[1], "cancel-init.pkg", true, false, true);
        run_cancel_product(argv[1], "cancel.pkg", false, false, true);
        run_cancel_product(argv[1], "cancel.pkg", false, true, true);
        run_cancel_product(argv[1], "cancel-stop-fail.pkg", false, false, false);
        run_cancel_product(argv[1], "cancel-stop-loop.pkg", false, false, false);
        puts("container_product_cancel: signed init/event/timer cancellation, actual stop, reclaim and stop-failure blocking passed");
        return 0;
    }
    if (argc == 3 && strcmp(argv[2], "event-failure") == 0) {
        run_event_failure_trial(argv[1]);
        run_confirmed_event_failure(argv[1], false);
        run_confirmed_event_failure(argv[1], true);
        puts("container_confirmed_event_failure: real guest failure, replacement rejection and uninstall reclaim passed");
        file_t key = read_file(argv[1], "public.der");
        file_t failed_package = read_file(argv[1], "event-loop.pkg");
        const char boot_id[] = "22222222-2222-4222-8222-222222222222";
        run_firmware_package_stage(&key, &failed_package, boot_id,
            ESP_BASE_OTA_PACKAGE_REUSE, false, false, false, 4U, false);
        run_firmware_package_stage(&key, &failed_package, boot_id,
            ESP_BASE_OTA_PACKAGE_WRITE, false, false, false, 4U, false);
        free(failed_package.bytes);
        free(key.bytes);
        puts("container_firmware_package_failure: real guest trap, native reclaim and A rollback passed");
        return 0;
    }
    if (argc == 3 && strcmp(argv[2], "identity") == 0) {
        file_t key = read_file(argv[1], "public.der");
        file_t confirmed = read_file(argv[1], "normal.pkg");
        file_t conflicting = read_file(argv[1], "identity.pkg");
        file_t newer = read_file(argv[1], "newer.pkg");
        expected_confirmed_version = read_file(argv[1], "normal-version.txt");
        expected_candidate_version = read_file(argv[1], "newer-version.txt");
        const char boot_id[] = "22222222-2222-4222-8222-222222222222";
        run_package_identity_fallback(&key, &confirmed, &conflicting, boot_id);
        run_prepare_preserves_confirmed(&key, &confirmed, &newer, &conflicting, boot_id);
        run_firmware_package_stage_internal(&key, &confirmed, &conflicting, boot_id,
            ESP_BASE_OTA_PACKAGE_WRITE, false, false, false, 0U, true, 0U);
        free(expected_confirmed_version.bytes);
        free(expected_candidate_version.bytes);
        free(newer.bytes);
        free(conflicting.bytes);
        free(confirmed.bytes);
        free(key.bytes);
        puts("container_package_identity: real PSS conflict, live confirmed guest, fallback reference and WRITE recovery passed");
        return 0;
    }
    assert(argc == 2);
    file_t key = read_file(argv[1], "public.der");
    file_t package = read_file(argv[1], "p0.pkg");
    file_t package_v1 = read_file(argv[1], "p1.pkg");
    expected_confirmed_version = read_file(argv[1], "p0-version.txt");
    expected_candidate_version = read_file(argv[1], "p1-version.txt");
    expected_second_version = read_file(argv[1], "p2-version.txt");
    file_t package_v2 = read_file(argv[1], "p2.pkg");
    for (esp_base_ota_package_mode_t mode = ESP_BASE_OTA_NO_PACKAGE;
         mode <= ESP_BASE_OTA_PACKAGE_WRITE; ++mode) {
        for (unsigned phase = ECONTAINER_SLOT_PREPARED;
             phase <= ECONTAINER_SLOT_HEALTH_VERIFIED; ++phase)
            run_historical_ota_startup(&key, &package, &package_v1, "22222222-2222-4222-8222-222222222222",
                mode, phase, 0U);
    }
    for (esp_base_ota_package_mode_t mode = ESP_BASE_OTA_NO_PACKAGE;
         mode <= ESP_BASE_OTA_PACKAGE_WRITE; ++mode) {
        for (unsigned negative = 1U; negative <= 11U; ++negative) {
            if (negative == 4U && mode == ESP_BASE_OTA_NO_PACKAGE) continue;
            run_historical_ota_startup(&key, &package, &package_v1,
                "22222222-2222-4222-8222-222222222222", mode,
                ECONTAINER_SLOT_TRIAL_STARTED, negative);
        }
    }
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
    uint8_t confirmed_flash[FLASH_BYTES];
    memcpy(confirmed_flash, store.flash, sizeof confirmed_flash);

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
    assert(memcmp(store.flash, confirmed_flash, sizeof confirmed_flash) == 0);
    assert(store.blob_writes == blob_writes && store.flash_erases == flash_erases &&
           store.flash_writes == flash_writes);
    esp_base_container_binding_snapshot_t stopped_binding = {0};
    esp_base_container_active_product_t stopped_active = {0};
    assert(esp_base_container_product_status_snapshot(
        &claim, &stopped_binding, &stopped_active) == ESP_BASE_CONTAINER_BINDING_OK);
    assert(stopped_binding.package_present && !stopped_active.present &&
           stopped_active.product_version == NULL && s_product.native_reclaimed &&
           !s_product.thread_joinable &&
           !atomic_load(&s_product.instance_active));
    assert(esp_base_storage_release(&claim));
    assert(!store.locked && store.mapping == NULL);
    dispose_product();

    /* A stop belongs to the old boot. Reset only RAM owners, retaining the
     * exact confirmed ECS2 and package Flash; normal startup must reopen it. */
    configure_product(&key);
    esp_base_storage_owner_init(&owner);
    const char next_boot_id[] = "33333333-3333-4333-8333-333333333333";
    assert(esp_base_storage_claim(&owner, &claim));
    assert(esp_base_container_product_boot(&claim, next_boot_id) ==
           ESP_BASE_CONTAINER_RUNNING);
    check_active_product(&claim, &expected_confirmed_version, false, NULL);
    assert(esp_base_container_product_event_accepting());
    assert(memcmp(store.blob, confirmed_blob, sizeof confirmed_blob) == 0 &&
           memcmp(store.flash, confirmed_flash, sizeof confirmed_flash) == 0);
    assert(store.blob_writes == blob_writes && store.flash_erases == flash_erases &&
           store.flash_writes == flash_writes);
    assert(esp_base_container_product_stop_confirmed(&claim));
    assert(esp_base_storage_release(&claim));
    dispose_product();
    assert(pthread_mutex_destroy(&store.mutex) == 0);
    run_public_product_lifecycle(&key, &package, boot_id);
    run_uninstall_with_fallback(&key, &package, boot_id);
    run_uninstall_uncertain(&key, &package, boot_id, 1U);
    run_uninstall_uncertain(&key, &package, boot_id, 2U);
    run_uninstall_lost_state(&key, &package, boot_id);
    run_uninstall_reclaimed_guest(&key, &package, boot_id, false);
    run_uninstall_reclaimed_guest(&key, &package, boot_id, true);
    run_uninstall_stop_timeout(&key, &package, boot_id);
    run_signed_reinstall_cycles(&key, &package, boot_id);
    run_prepare_preserves_confirmed(&key, &package, &package_v1, NULL, boot_id);
    run_prepared_package_abandon(&key, &package_v1, boot_id);
    run_package_trial_confirmation(&key, &package_v1, boot_id);
    run_package_trial_confirmation_uncertain(&key, &package_v1, boot_id,
        1U, ECONTAINER_SLOT_HEALTH_VERIFIED);
    run_package_trial_confirmation_uncertain(&key, &package_v1, boot_id,
        4U, ECONTAINER_SLOT_CONFIRMED);
    run_package_trial_cold_recovery(&key, &package, &package_v1, boot_id);
    run_package_intent_without_reservation(&key, &package, boot_id);
    run_source_change_same_boot(&key, &package_v1, &package_v2, boot_id);
    run_firmware_empty_stage(&key, NULL, boot_id, ESP_BASE_OTA_NO_PACKAGE, 0U);
    run_firmware_empty_stage(&key, NULL, boot_id, ESP_BASE_OTA_NO_PACKAGE, 2U);
    run_firmware_empty_stage(&key, &package, boot_id, ESP_BASE_OTA_PACKAGE_WRITE, 0U);
    run_firmware_empty_stage(&key, &package, boot_id, ESP_BASE_OTA_PACKAGE_WRITE, 1U);
    run_firmware_empty_stage(&key, &package, boot_id, ESP_BASE_OTA_PACKAGE_WRITE, 2U);
    run_firmware_package_stage(&key, &package, boot_id,
                               ESP_BASE_OTA_PACKAGE_REUSE, false, true, false, 0U, false);
    run_firmware_package_stage(&key, &package, boot_id,
                               ESP_BASE_OTA_PACKAGE_WRITE, false, true, false, 0U, false);
    run_firmware_package_stage(&key, &package, boot_id,
                               ESP_BASE_OTA_PACKAGE_REUSE, false, false, false, 0U, false);
    run_firmware_package_stage(&key, &package, boot_id,
                               ESP_BASE_OTA_PACKAGE_WRITE, false, false, false, 0U, false);
    run_firmware_package_stage(&key, &package, boot_id,
                               ESP_BASE_OTA_PACKAGE_WRITE, true, false, false, 0U, false);
    for (unsigned write_failure = 1U; write_failure <= 2U; ++write_failure)
        run_firmware_package_stage_internal(&key, &package, NULL, boot_id,
            ESP_BASE_OTA_PACKAGE_WRITE, false, false, false, 0U, false, write_failure);
    run_firmware_package_stage(&key, &package, boot_id,
                               ESP_BASE_OTA_PACKAGE_REUSE, false, false, true, 0U, false);
    run_firmware_package_stage(&key, &package, boot_id,
                               ESP_BASE_OTA_PACKAGE_WRITE, false, false, true, 0U, false);
    for (esp_base_ota_package_mode_t mode = ESP_BASE_OTA_PACKAGE_REUSE;
         mode <= ESP_BASE_OTA_PACKAGE_WRITE; ++mode) {
        run_firmware_package_stage(&key, &package, boot_id,
            mode, false, false, false, 0U, true);
        run_firmware_package_stage(&key, &package, boot_id,
            mode, false, true, false, 0U, true);
        run_firmware_package_stage(&key, &package, boot_id,
            mode, false, false, false, 1U, true);
    }
    for (unsigned live_trial = 1U; live_trial <= 3U; ++live_trial) {
        run_firmware_package_stage(&key, &package, boot_id,
            ESP_BASE_OTA_PACKAGE_REUSE, false, false, false, live_trial, false);
        run_firmware_package_stage(&key, &package, boot_id,
            ESP_BASE_OTA_PACKAGE_WRITE, false, false, false, live_trial, false);
    }
    run_success_receipt_replay(&key, &package, boot_id);
    free(expected_confirmed_version.bytes);
    free(expected_candidate_version.bytes);
    free(expected_second_version.bytes);
    free(package_v1.bytes);
    free(package_v2.bytes);
    free(package.bytes);
    free(key.bytes);
    puts("container_product_lifecycle: signed stop/uninstall/readback, 100 reinstall cycles, same-boot v1/v2 behavior and V2 replay passed");
    return 0;
}

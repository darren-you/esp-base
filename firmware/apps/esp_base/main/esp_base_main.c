#include <inttypes.h>
#include <stdbool.h>
#include <string.h>

#include "sdkconfig.h"
#include "esp_app_desc.h"
#include "esp_idf_version.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_base_identity.h"
#include "esp_base_ota_firmware.h"
#if CONFIG_ESP_BASE_FRP_SCRATCH_ENABLED
#include "esp_frp_idf_flash_store.h"
#endif
#include "eota.h"
#include "esp_base_ota_policy.h"
#include "esp_base_ota_receipt.h"
#include "esp_base_protocol.h"
#include "esp_base_remote_config.h"
#include "esp_base_storage_owner.h"
#include "esp_base_safety.h"
#include "esp_base_time.h"

static const char *TAG = "esp_base";
static esp_base_storage_owner_t s_storage_owner;
static esp_base_storage_claim_t s_boot_storage_claim;
static esp_base_storage_owner_t s_flash_io_owner;
static esp_base_storage_claim_t s_ota_flash_claim;
static esp_base_ota_receipt_recovery_t s_boot_ota_receipt;

#define ESP_BASE_FLASH_IO_WAIT_US INT64_C(500000)

static bool claim_flash_io(esp_base_storage_owner_t *owner,
                           esp_base_storage_claim_t *claim)
{
    if (owner == NULL || claim == NULL) return false;
    const int64_t started_us = esp_timer_get_time();
    if (started_us < 0) return false;
    do {
        if (esp_base_storage_claim(owner, claim)) return true;
        if (esp_timer_get_time() - started_us >= ESP_BASE_FLASH_IO_WAIT_US) return false;
        vTaskDelay(1);
    } while (true);
}

static bool ota_flash_acquire(void *context)
{
    return context == &s_flash_io_owner &&
           claim_flash_io(context, &s_ota_flash_claim);
}

static bool ota_flash_release(void *context)
{
    return context == &s_flash_io_owner &&
           esp_base_storage_release(&s_ota_flash_claim);
}

static esp_err_t inspect_with_flash_io(eota_current_t *ota)
{
    if (!ota_flash_acquire(&s_flash_io_owner)) return ESP_ERR_TIMEOUT;
    const esp_err_t result = eota_inspect(ota);
    return ota_flash_release(&s_flash_io_owner) ? result : ESP_FAIL;
}

static esp_err_t confirm_pending_with_flash_io(eota_current_t *ota)
{
    if (!ota_flash_acquire(&s_flash_io_owner)) return ESP_ERR_TIMEOUT;
    const esp_err_t result = eota_confirm_pending(ota);
    return ota_flash_release(&s_flash_io_owner) ? result : ESP_FAIL;
}

static esp_err_t reject_pending_with_flash_io(eota_current_t *ota)
{
    if (!ota_flash_acquire(&s_flash_io_owner)) return ESP_ERR_TIMEOUT;
    /* SDK success reboots. A returned error still needs the short claim freed. */
    const esp_err_t result = eota_reject_pending(ota);
    return ota_flash_release(&s_flash_io_owner) ? result : ESP_FAIL;
}
#if CONFIG_ESP_BASE_FRP_SCRATCH_ENABLED
/* A scratch read/write must not inherit the OTA transaction's lifetime.
 * The provider retains its own record lease between these short I/O claims. */
static efrp_idf_flash_store_t s_frp_scratch;

static efrp_result_t frp_scratch_with_owner(
    void *owner_context, efrp_result_t (*operation)(void *), void *operation_context)
{
    if (owner_context == NULL || operation == NULL) return EFRP_INVALID_ARGUMENT;
    esp_base_storage_claim_t claim = {0};
    if (!claim_flash_io(owner_context, &claim)) return EFRP_STORAGE_ERROR;
    const efrp_result_t result = operation(operation_context);
    return esp_base_storage_release(&claim) ? result : EFRP_STORAGE_ERROR;
}
#endif

#define ESP_BASE_CONTROL_START_TIMEOUT_MS UINT64_C(5000)
#define ESP_BASE_OTA_STABLE_WINDOW_MS UINT64_C(30000)

static esp_err_t initialise_nvs(void)
{
    esp_base_storage_claim_t claim = {0};
    if (!claim_flash_io(&s_flash_io_owner, &claim)) return ESP_ERR_TIMEOUT;
    const esp_err_t result = nvs_flash_init();
    return esp_base_storage_release(&claim) ? result : ESP_FAIL;
}

static esp_err_t read_identity_with_flash_io(esp_base_identity_t *identity)
{
    esp_base_storage_claim_t claim = {0};
    if (!claim_flash_io(&s_flash_io_owner, &claim)) return ESP_ERR_TIMEOUT;
    const esp_err_t result = esp_base_identity_read(identity);
    return esp_base_storage_release(&claim) ? result : ESP_FAIL;
}

static uint64_t uptime_ms(void)
{
    return (uint64_t)esp_timer_get_time() / 1000;
}

static void stop_after_local_failure(eota_current_t *ota, bool pending_boot,
                                     const char *check, esp_err_t failure)
{
    ESP_LOGE(TAG, "ESP_BASE_LOCAL_CHECK_FAILED check=%s error=%s", check, esp_err_to_name(failure));
    if (!pending_boot) {
        return;
    }
    if (ota->state != EOTA_STATE_PENDING_VERIFY) {
        ESP_LOGE(TAG, "ESP_BASE_OTA_RECOVERY_REQUIRED slot=%s state=%s rollback=not_safe",
                 ota->running_partition ? ota->running_partition : "unknown", eota_state_name(ota->state));
        return;
    }
    ESP_LOGE(TAG, "Pending OTA slot %s failed %s; requesting IDF rollback", ota->running_partition, check);
    const esp_err_t rollback_status = reject_pending_with_flash_io(ota);
    /* ESP_OK never returns from the IDF rollback API. If it does return,
     * preserve this boot rather than force a reset without a viable slot. */
    ESP_LOGE(TAG, "ESP_BASE_OTA_RECOVERY_REQUIRED slot=%s state=%s rollback_error=%s",
             ota->running_partition ? ota->running_partition : "unknown",
             eota_state_name(ota->state), esp_err_to_name(rollback_status));
}

static esp_err_t wait_for_control_start(void)
{
    const uint64_t started_ms = uptime_ms();
    while (!esp_base_protocol_control_healthy()) {
        const uint64_t now_ms = uptime_ms();
        if (now_ms < started_ms || now_ms - started_ms >= ESP_BASE_CONTROL_START_TIMEOUT_MS) {
            return ESP_ERR_TIMEOUT;
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    return ESP_OK;
}

/* Only the durable original intent authorizes target cleanup after reset.
 * The startup claim excludes both OTA entry points until reconciliation. */
static bool reconcile_interrupted_ota(const char *device_id,
                                     bool *needs_success_receipt,
                                     esp_base_ota_receipt_recovery_t *receipt)
{
    *needs_success_receipt = false;
    *receipt = (esp_base_ota_receipt_recovery_t){0};
    if (!eota_available()) return true;
    const esp_base_ota_receipt_result_t loaded =
        esp_base_ota_receipt_load_for_recovery(device_id, receipt);
    if (loaded == ESP_BASE_OTA_RECEIPT_NOT_FOUND) {
        esp_base_ota_firmware_set_t firmware = {0};
        return esp_base_ota_observe_firmware_set(
            ESP_BASE_OTA_FIRMWARE_CONFIRMED, NULL, &firmware) ==
            ESP_BASE_OTA_FIRMWARE_OK;
    }
    if (loaded != ESP_BASE_OTA_RECEIPT_OK) return false;
    const eota_policy_t policy = esp_base_ota_policy(false);
    eota_slots_t slots = {0};
    if (eota_observe_slots(&policy, &slots) != EOTA_UPDATE_OK ||
        slots.running_subtype != slots.boot_subtype ||
        slots.running_address_bytes != slots.boot_address_bytes) {
        return false;
    }
    if (receipt->status == ESP_BASE_OTA_RECEIPT_FAILED) {
        esp_base_ota_receipt_view_t view = {0};
        return esp_base_ota_receipt_query(device_id, receipt->operation_id, false, &view) ==
            ESP_BASE_OTA_RECEIPT_OK && view.state == ESP_BASE_OTA_OPERATION_FAILED;
    }
    if (slots.running_subtype == receipt->target_subtype) {
        if (slots.running_state == EOTA_STATE_VALID &&
            wait_for_control_start() != ESP_OK) return false;
        /* A selected C belongs to pending trial or confirmed recovery. Never
         * erase it as though it were an interrupted inactive download. */
        uint8_t digest[EOTA_SHA256_BYTES] = {0};
        esp_base_ota_firmware_set_t firmware = {0};
        if ((slots.running_state != EOTA_STATE_PENDING_VERIFY &&
             slots.running_state != EOTA_STATE_VALID) ||
            (receipt->status == ESP_BASE_OTA_RECEIPT_SUCCEEDED &&
             slots.running_state != EOTA_STATE_VALID) ||
            eota_sha256_running(&policy, receipt->image_size_bytes, digest) !=
                EOTA_UPDATE_OK ||
            memcmp(digest, receipt->candidate_sha256, sizeof digest) != 0 ||
            esp_base_ota_observe_firmware_set(
                slots.running_state == EOTA_STATE_PENDING_VERIFY ?
                ESP_BASE_OTA_FIRMWARE_PENDING_TRIAL : ESP_BASE_OTA_FIRMWARE_CONFIRMED,
                NULL, &firmware) != ESP_BASE_OTA_FIRMWARE_OK ||
            firmware.bootable_count != 2U ||
            memcmp(firmware.running_firmware_sha256, receipt->candidate_sha256, 32) != 0 ||
            memcmp(firmware.bootable_firmware_sha256[1], receipt->source_sha256, 32) != 0) {
            return false;
        }
        *needs_success_receipt =
            receipt->status == ESP_BASE_OTA_RECEIPT_PREPARED;
        return true;
    }
    if (receipt->status != ESP_BASE_OTA_RECEIPT_PREPARED ||
        slots.running_subtype != receipt->source_subtype ||
        slots.running_state != EOTA_STATE_VALID ||
        slots.target_subtype != receipt->target_subtype ||
        eota_retire_inactive(&policy, receipt->target_subtype,
                             receipt->source_sha256) != EOTA_UPDATE_OK) {
        return false;
    }
    return esp_base_ota_receipt_record_failure(
        device_id, receipt->operation_id,
        EOTA_UPDATE_RESOURCE_FAILURE) == ESP_BASE_OTA_RECEIPT_OK;
}

void app_main(void)
{
    /* Startup recovery uses the same upgrade owner as both OTA entries. */
    esp_base_storage_owner_init(&s_storage_owner);
    esp_base_storage_owner_init(&s_flash_io_owner);
    s_ota_flash_claim = (esp_base_storage_claim_t){0};
    if (!esp_base_ota_policy_bind_flash_io((eota_flash_io_t){
            .acquire = ota_flash_acquire,
            .release = ota_flash_release,
            .context = &s_flash_io_owner,
        })) {
        ESP_LOGE(TAG, "ESP_BASE_OTA_RECOVERY_REQUIRED Flash I/O gate unavailable");
        return;
    }
#if CONFIG_ESP_BASE_FRP_SCRATCH_ENABLED
    /* Recover interrupted ciphertext before any pending OTA slot can be
     * confirmed. Keep physical I/O distinct from the long upgrade claim. */
    const efrp_idf_flash_store_config_t scratch = {
        .partition_label = CONFIG_ESP_BASE_FRP_SCRATCH_LABEL,
        .partition_type = ESP_PARTITION_TYPE_DATA,
        .partition_subtype = ESP_PARTITION_SUBTYPE_DATA_UNDEFINED,
        .partition_offset_bytes = CONFIG_ESP_BASE_FRP_SCRATCH_OFFSET,
        .partition_size_bytes = UINT32_C(0x10000),
        .owner_context = &s_flash_io_owner,
        .with_owner = frp_scratch_with_owner,
    };
    if (!efrp_idf_flash_store_bind(&s_frp_scratch, &scratch) ||
        efrp_aead_flash_store_recover(
            efrp_idf_flash_store_callbacks(&s_frp_scratch)) != EFRP_OK) {
        ESP_LOGE(TAG, "ESP_BASE_FRP_SCRATCH_BLOCKED partition or boot recovery failed");
        return;
    }
#endif
    s_boot_storage_claim = (esp_base_storage_claim_t){0};
    if (!esp_base_storage_claim(&s_storage_owner, &s_boot_storage_claim)) {
        ESP_LOGE(TAG, "ESP_BASE_OTA_RECOVERY_REQUIRED storage owner unavailable");
        return;
    }
    eota_current_t ota = {0};
    const esp_err_t ota_status = inspect_with_flash_io(&ota);
    if (ota_status != ESP_OK) {
        ESP_LOGE(TAG, "OTA slot state unavailable (%s); initialization stopped", esp_err_to_name(ota_status));
        ESP_LOGE(TAG, "ESP_BASE_OTA_RECOVERY_REQUIRED slot=%s state=%s rollback=not_safe",
                 ota.running_partition ? ota.running_partition : "unknown", eota_state_name(ota.state));
        return;
    }
    const bool pending_boot = ota.state == EOTA_STATE_PENDING_VERIFY;
    /* The control task may start before durable receipt recovery finishes.
     * Keep configuration writes and network owners gated until reconciliation. */
    esp_base_protocol_set_ota_verification_pending(true);

    const esp_err_t storage_status = initialise_nvs();
    if (storage_status != ESP_OK) {
        ESP_LOGE(TAG, "NVS unavailable (%s); storage preserved, initialization stopped", esp_err_to_name(storage_status));
        stop_after_local_failure(&ota, pending_boot, "nvs", storage_status);
        return;
    }

    static esp_base_identity_t identity = {0};
    const esp_err_t identity_status = read_identity_with_flash_io(&identity);
    if (identity_status != ESP_OK) {
        ESP_LOGE(TAG, "Identity unavailable (%s); initialization stopped", esp_err_to_name(identity_status));
        stop_after_local_failure(&ota, pending_boot, "identity", identity_status);
        return;
    }

    esp_base_safety_t safety = {0};
    const esp_err_t safety_status = esp_base_safety_start(&safety);
    if (safety_status != ESP_OK) {
        stop_after_local_failure(&ota, pending_boot, "safety", safety_status);
        return;
    }

    uint32_t config_revision = 0;
    const esp_err_t config_status = esp_base_protocol_load_config(
        &config_revision, &s_flash_io_owner);
    if (config_status != ESP_OK) {
        ESP_LOGE(TAG, "Configuration unavailable (%s); storage preserved, initialization stopped", esp_err_to_name(config_status));
        stop_after_local_failure(&ota, pending_boot, "config", config_status);
        return;
    }

    const esp_app_desc_t *app = esp_app_get_description();
    ESP_LOGI(TAG,
             "ESP_BASE_BOOT project=ESP Base device_id=%s version=%s idf=%s chip=%s revision=%d.%d flash=%" PRIu32
             " slot=%s reset=%s config_generation=%" PRIu32,
             identity.device_id,
             app->version,
             esp_get_idf_version(),
             identity.model,
             identity.revision / 100,
             identity.revision % 100,
             identity.flash_size_bytes,
             ota.running_partition,
             safety.reset_reason,
             config_revision);

    const esp_base_protocol_context_t protocol = {
        .device_id = identity.device_id,
        .firmware_version = app->version,
        .chip_model = identity.model,
        .flash_size_bytes = identity.flash_size_bytes,
        .reset_reason = safety.reset_reason,
        .storage_owner = &s_storage_owner,
        .flash_io_owner = &s_flash_io_owner,
#if CONFIG_ESP_BASE_FRP_SCRATCH_ENABLED
        .frp_flash_store = efrp_idf_flash_store_callbacks(&s_frp_scratch),
#endif
    };
    const esp_err_t protocol_status = esp_base_protocol_start(&protocol);
    if (protocol_status != ESP_OK) {
        ESP_LOGE(TAG, "Control unavailable (%s); initialization stopped", esp_err_to_name(protocol_status));
        stop_after_local_failure(&ota, pending_boot, "control_start", protocol_status);
        return;
    }

    bool needs_success_receipt = false;
    if (!reconcile_interrupted_ota(identity.device_id,
                                   &needs_success_receipt, &s_boot_ota_receipt)) {
        ESP_LOGE(TAG, "ESP_BASE_OTA_RECOVERY_REQUIRED original receipt or slot recovery uncertain");
        stop_after_local_failure(&ota, pending_boot, "ota_recovery",
                                 ESP_ERR_INVALID_STATE);
        return;
    }
    /* Time is needed by future strict TLS consumers, but it is not part of
     * the pending slot's local self-test and must not block USB control. */
    const esp_err_t time_status = esp_base_time_start(CONFIG_ESP_BASE_TIME_SERVER);
    if (time_status != ESP_OK) {
        ESP_LOGW(TAG, "ESP_BASE_TIME_UNAVAILABLE error=%s", esp_err_to_name(time_status));
    }

    if (pending_boot) {
        /* Startup checks above are local: no Broker, FRPS or Wi-Fi connection is
         * required. The control task must also make progress throughout this boot. */
        const esp_err_t control_status = wait_for_control_start();
        if (control_status != ESP_OK) {
            stop_after_local_failure(&ota, pending_boot, "control_ready", control_status);
            return;
        }
        ESP_LOGI(TAG, "pending OTA slot %s: waiting %" PRIu64 " ms of local stability",
                 ota.running_partition, ESP_BASE_OTA_STABLE_WINDOW_MS);
        const uint64_t stable_started_ms = uptime_ms();
        uint64_t now_ms;
        for (;;) {
            if (!esp_base_protocol_control_healthy()) {
                stop_after_local_failure(&ota, pending_boot, "control_progress", ESP_ERR_TIMEOUT);
                return;
            }
            now_ms = uptime_ms();
            if (now_ms < stable_started_ms) {
                stop_after_local_failure(&ota, pending_boot, "stable_clock", ESP_ERR_INVALID_STATE);
                return;
            }
            if (now_ms - stable_started_ms >= ESP_BASE_OTA_STABLE_WINDOW_MS) {
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
        /* A heartbeat from before the 30-second boundary is insufficient:
         * require one complete control pass afterwards before confirming. */
        const uint32_t boundary_progress = esp_base_protocol_control_progress_count();
        const uint64_t boundary_ms = now_ms;
        while (esp_base_protocol_control_progress_count() == boundary_progress) {
            if (!esp_base_protocol_control_healthy()) {
                stop_after_local_failure(&ota, pending_boot, "control_progress", ESP_ERR_TIMEOUT);
                return;
            }
            now_ms = uptime_ms();
            if (now_ms < boundary_ms || now_ms - boundary_ms >= ESP_BASE_CONTROL_START_TIMEOUT_MS) {
                stop_after_local_failure(&ota, pending_boot, "control_boundary", ESP_ERR_TIMEOUT);
                return;
            }
            vTaskDelay(pdMS_TO_TICKS(100));
        }
        if (!esp_base_protocol_control_healthy()) {
            stop_after_local_failure(&ota, pending_boot, "control_boundary", ESP_ERR_TIMEOUT);
            return;
        }
        now_ms = uptime_ms();
        const esp_err_t confirm_status = now_ms >= stable_started_ms &&
            now_ms - stable_started_ms >= ESP_BASE_OTA_STABLE_WINDOW_MS ?
            confirm_pending_with_flash_io(&ota) : ESP_ERR_NOT_FINISHED;
        if (confirm_status != ESP_OK) {
            stop_after_local_failure(&ota, pending_boot, "confirm", confirm_status);
            return;
        }
        eota_current_t confirmed = {0};
        if (inspect_with_flash_io(&confirmed) != ESP_OK ||
            confirmed.state != EOTA_STATE_VALID ||
            confirmed.running_partition == NULL || ota.running_partition == NULL ||
            strcmp(confirmed.running_partition, ota.running_partition) != 0) {
            ESP_LOGE(TAG, "ESP_BASE_OTA_RECOVERY_REQUIRED VALID confirmation readback incomplete");
            return;
        }
    }
    if (needs_success_receipt &&
        esp_base_ota_receipt_record_success(identity.device_id) !=
            ESP_BASE_OTA_RECEIPT_OK) {
        ESP_LOGE(TAG, "ESP_BASE_OTA_RECOVERY_REQUIRED success receipt not durable");
        return;
    }
    if (!esp_base_storage_release(&s_boot_storage_claim)) {
        ESP_LOGE(TAG, "ESP_BASE_OTA_RECOVERY_REQUIRED storage owner release failed");
        return;
    }
    esp_base_protocol_set_ota_verification_pending(false);
    ESP_LOGI(TAG, "ESP_BASE_READY hardware_outputs=untouched provisioning=required business=native");
}

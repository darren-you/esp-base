#include "esp_base_ota_update.h"

#include <stddef.h>
#include <string.h>

#include "sdkconfig.h"

#if defined(CONFIG_SECURE_SIGNED_APPS_NO_SECURE_BOOT) && \
    defined(CONFIG_SECURE_SIGNED_ON_UPDATE_NO_SECURE_BOOT) && \
    defined(CONFIG_SECURE_SIGNED_APPS_RSA_SCHEME) && \
    defined(CONFIG_MBEDTLS_CERTIFICATE_BUNDLE) && \
    defined(CONFIG_MBEDTLS_HAVE_TIME_DATE) && \
    defined(CONFIG_ESP_HTTP_CLIENT_ENABLE_HTTPS) && \
    defined(CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE) && \
    !defined(CONFIG_BOOTLOADER_APP_ANTI_ROLLBACK)
#define ESP_BASE_SIGNED_OTA_ENABLED 1
#else
#define ESP_BASE_SIGNED_OTA_ENABLED 0
#endif

#if ESP_BASE_SIGNED_OTA_ENABLED
#include "esp_app_desc.h"
#include "esp_app_format.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_timer.h"
#include "psa/crypto.h"

#define ESP_BASE_OTA_TOTAL_TIMEOUT_US INT64_C(300000000)
#define ESP_BASE_OTA_IDLE_TIMEOUT_US INT64_C(30000000)
/* A small SDK read limits one returned body chunk; SDK calls can still loop
 * internally on a continuously trickling peer, so these are return-point
 * deadlines rather than a strict wall-clock cancellation guarantee. */
#define ESP_BASE_OTA_READ_BYTES 64
#define ESP_BASE_OTA_PREFIX_BYTES \
    (sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t) + sizeof(esp_app_desc_t))

static bool within_download_deadline(int64_t started_us, int64_t last_progress_us)
{
    const int64_t now_us = esp_timer_get_time();
    return now_us >= started_us && now_us - started_us < ESP_BASE_OTA_TOTAL_TIMEOUT_US &&
           now_us >= last_progress_us && now_us - last_progress_us < ESP_BASE_OTA_IDLE_TIMEOUT_US;
}

static bool same_partition(const esp_partition_t *a, const esp_partition_t *b)
{
    return a != NULL && b != NULL && a->type == b->type && a->subtype == b->subtype &&
           a->address == b->address && a->size == b->size;
}

static bool expected_slot(const esp_partition_t *partition)
{
    if (partition == NULL || partition->type != ESP_PARTITION_TYPE_APP || partition->size != 0x1e0000) {
        return false;
    }
    return (partition->subtype == ESP_PARTITION_SUBTYPE_APP_OTA_0 && partition->address == 0x20000) ||
           (partition->subtype == ESP_PARTITION_SUBTYPE_APP_OTA_1 && partition->address == 0x200000);
}

static bool valid_url(const char *url)
{
    if (url == NULL || strncmp(url, "https://", 8) != 0) return false;
    size_t length = strnlen(url, ESP_BASE_OTA_URL_BYTES + 1);
    if (length <= 8 || length > ESP_BASE_OTA_URL_BYTES) return false;
    const char *authority_end = strpbrk(url + 8, "/?#");
    if (authority_end == NULL) authority_end = url + length;
    if (authority_end == url + 8 || *authority_end == '?' || *authority_end == '#') return false;
    for (const char *p = url + 8; p < authority_end; ++p) if (*p == '@') return false;
    for (const char *p = url + 8; p < url + length; ++p) {
        if ((unsigned char)*p <= 0x20 || *p == '#') return false;
    }
    return true;
}

static bool valid_request(const esp_base_ota_update_request_t *request)
{
    return request != NULL && valid_url(request->image_url) && request->image_size_bytes > 0;
}

static esp_base_ota_update_result_t hash_partition(const esp_partition_t *partition,
    uint32_t size, const uint8_t expected[32])
{
    uint8_t buffer[1024], actual[32];
    psa_hash_operation_t hash = PSA_HASH_OPERATION_INIT;
    if (psa_hash_setup(&hash, PSA_ALG_SHA_256) != PSA_SUCCESS) return ESP_BASE_OTA_UPDATE_DOWNLOAD_FAILED;
    for (uint32_t offset = 0; offset < size;) {
        const size_t chunk = size - offset < sizeof buffer ? size - offset : sizeof buffer;
        if (esp_partition_read(partition, offset, buffer, chunk) != ESP_OK ||
            psa_hash_update(&hash, buffer, chunk) != PSA_SUCCESS) {
            (void)psa_hash_abort(&hash);
            return ESP_BASE_OTA_UPDATE_DOWNLOAD_FAILED;
        }
        offset += (uint32_t)chunk;
    }
    size_t actual_size = 0;
    const psa_status_t status = psa_hash_finish(&hash, actual, sizeof actual, &actual_size);
    if (status != PSA_SUCCESS || actual_size != sizeof actual) return ESP_BASE_OTA_UPDATE_DOWNLOAD_FAILED;
    return memcmp(actual, expected, sizeof actual) == 0 ? ESP_BASE_OTA_UPDATE_OK :
           ESP_BASE_OTA_UPDATE_HASH_MISMATCH;
}
#endif

bool esp_base_ota_update_available(void)
{
    return ESP_BASE_SIGNED_OTA_ENABLED;
}

const char *esp_base_ota_update_error(esp_base_ota_update_result_t result)
{
    switch (result) {
    case ESP_BASE_OTA_UPDATE_UNSUPPORTED: return "ota_signing_unavailable";
    case ESP_BASE_OTA_UPDATE_INVALID_REQUEST: return "invalid_request";
    case ESP_BASE_OTA_UPDATE_SLOT_UNAVAILABLE: return "ota_slot_unavailable";
    case ESP_BASE_OTA_UPDATE_TOO_LARGE: return "ota_image_too_large";
    case ESP_BASE_OTA_UPDATE_WRONG_TARGET: return "ota_wrong_target";
    case ESP_BASE_OTA_UPDATE_DOWNLOAD_FAILED: return "ota_download_failed";
    case ESP_BASE_OTA_UPDATE_HASH_MISMATCH: return "ota_hash_mismatch";
    case ESP_BASE_OTA_UPDATE_SIGNATURE_INVALID: return "ota_signature_invalid";
    case ESP_BASE_OTA_UPDATE_BOOT_STATE_UNKNOWN: return "ota_boot_state_unknown";
    case ESP_BASE_OTA_UPDATE_RESOURCE_FAILURE: return "resource_failure";
    default: return NULL;
    }
}

esp_base_ota_update_result_t esp_base_ota_update_run(
    const esp_base_ota_update_request_t *request, esp_base_ota_progress_t progress, void *context)
{
#if !ESP_BASE_SIGNED_OTA_ENABLED
    (void)request;
    (void)progress;
    (void)context;
    return ESP_BASE_OTA_UPDATE_UNSUPPORTED;
#else
    if (!valid_request(request)) return ESP_BASE_OTA_UPDATE_INVALID_REQUEST;
    const esp_partition_t *running = esp_ota_get_running_partition();
    const esp_partition_t *boot = esp_ota_get_boot_partition();
    const esp_partition_t *target = esp_ota_get_next_update_partition(NULL);
    if (running == NULL || boot == NULL || target == NULL || !same_partition(running, boot) ||
        same_partition(running, target) || !expected_slot(running) || !expected_slot(target) ||
        !((running->subtype == ESP_PARTITION_SUBTYPE_APP_OTA_0 && target->subtype == ESP_PARTITION_SUBTYPE_APP_OTA_1) ||
          (running->subtype == ESP_PARTITION_SUBTYPE_APP_OTA_1 && target->subtype == ESP_PARTITION_SUBTYPE_APP_OTA_0))) {
        return ESP_BASE_OTA_UPDATE_SLOT_UNAVAILABLE;
    }
    esp_ota_img_states_t state;
    if (esp_ota_get_state_partition(running, &state) != ESP_OK || state != ESP_OTA_IMG_VALID) {
        return ESP_BASE_OTA_UPDATE_SLOT_UNAVAILABLE;
    }
    const esp_err_t target_state_error = esp_ota_get_state_partition(target, &state);
    if ((target_state_error != ESP_OK && target_state_error != ESP_ERR_NOT_FOUND) ||
        (target_state_error == ESP_OK && state != ESP_OTA_IMG_UNDEFINED &&
         state != ESP_OTA_IMG_VALID && state != ESP_OTA_IMG_INVALID &&
         state != ESP_OTA_IMG_ABORTED)) {
        return ESP_BASE_OTA_UPDATE_SLOT_UNAVAILABLE;
    }
    if (request->image_size_bytes > target->size) return ESP_BASE_OTA_UPDATE_TOO_LARGE;

    const esp_http_client_config_t http = {
        .url = request->image_url,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .disable_auto_redirect = true,
        .timeout_ms = 5000,
        .buffer_size = 1024,
    };
    esp_http_client_handle_t client = esp_http_client_init(&http);
    if (client == NULL) return ESP_BASE_OTA_UPDATE_DOWNLOAD_FAILED;
    esp_ota_handle_t handle = 0;
    bool ota_started = false;
    esp_base_ota_update_result_t result = ESP_BASE_OTA_UPDATE_DOWNLOAD_FAILED;
    const int64_t started_us = esp_timer_get_time();
    int64_t last_progress_us = started_us;
    if (!within_download_deadline(started_us, last_progress_us) ||
        esp_http_client_open(client, 0) != ESP_OK ||
        !within_download_deadline(started_us, last_progress_us)) goto abort;
    if (esp_http_client_set_timeout_ms(client, 1000) != ESP_OK) goto abort;
    int64_t content_length;
    do {
        if (!within_download_deadline(started_us, last_progress_us)) goto abort;
        content_length = esp_http_client_fetch_headers(client);
        if (!within_download_deadline(started_us, last_progress_us)) goto abort;
    } while (content_length == -ESP_ERR_HTTP_EAGAIN);
    if (content_length != request->image_size_bytes ||
        esp_http_client_get_status_code(client) != 200 ||
        esp_http_client_is_chunked_response(client) ||
        esp_http_client_get_content_length(client) != request->image_size_bytes ||
        request->image_size_bytes < ESP_BASE_OTA_PREFIX_BYTES) goto abort;

    uint8_t buffer[ESP_BASE_OTA_READ_BYTES];
    uint8_t prefix[ESP_BASE_OTA_PREFIX_BYTES];
    uint8_t write_buffer[1024];
    size_t write_used = 0;
    uint32_t received = 0;
    while (received < request->image_size_bytes) {
        if (!within_download_deadline(started_us, last_progress_us)) goto abort;
        const uint32_t left = request->image_size_bytes - received;
        size_t wanted = left < sizeof buffer ? left : sizeof buffer;
        if (received < sizeof prefix && wanted > sizeof prefix - received) {
            wanted = sizeof prefix - received;
        } else if (received >= sizeof prefix && wanted > sizeof write_buffer - write_used) {
            wanted = sizeof write_buffer - write_used;
        }
        const int count = esp_http_client_read(client, (char *)buffer, (int)wanted);
        if (!within_download_deadline(started_us, last_progress_us)) goto abort;
        if (count == -ESP_ERR_HTTP_EAGAIN) continue;
        if (count <= 0 || (size_t)count > wanted) goto abort;
        if (received < sizeof prefix) {
            memcpy(prefix + received, buffer, (size_t)count);
        } else {
            memcpy(write_buffer + write_used, buffer, (size_t)count);
            write_used += (size_t)count;
            if (write_used == sizeof write_buffer) {
                if (esp_ota_write(handle, write_buffer, write_used) != ESP_OK) goto abort;
                write_used = 0;
            }
        }
        received += (uint32_t)count;
        last_progress_us = esp_timer_get_time();
        if (!within_download_deadline(started_us, last_progress_us)) goto abort;
        if (received == sizeof prefix) {
            esp_image_header_t image_header;
            esp_app_desc_t image;
            memcpy(&image_header, prefix, sizeof image_header);
            memcpy(&image, prefix + sizeof image_header + sizeof(esp_image_segment_header_t), sizeof image);
            if (image_header.magic != ESP_IMAGE_HEADER_MAGIC || image.magic_word != ESP_APP_DESC_MAGIC_WORD ||
                strncmp(image.project_name, "esp_base", sizeof image.project_name) != 0) {
                result = ESP_BASE_OTA_UPDATE_WRONG_TARGET;
                goto abort;
            }
            if (esp_ota_check_image_validity(ESP_PARTITION_TYPE_APP, &image_header, &image) != ESP_OK) {
                result = ESP_BASE_OTA_UPDATE_WRONG_TARGET;
                goto abort;
            }
            if (esp_ota_begin(target, request->image_size_bytes, &handle) != ESP_OK) goto abort;
            ota_started = true;
            memcpy(write_buffer, prefix, sizeof prefix);
            write_used = sizeof prefix;
        }
        if (progress != NULL) progress(received, request->image_size_bytes, context);
    }
    if (!esp_http_client_is_complete_data_received(client)) goto abort;
    if (write_used > 0 && esp_ota_write(handle, write_buffer, write_used) != ESP_OK) goto abort;
    if (!within_download_deadline(started_us, last_progress_us)) goto abort;
    result = hash_partition(target, request->image_size_bytes, request->sha256);
    if (result != ESP_BASE_OTA_UPDATE_OK) goto abort;
    /* esp_ota_end validates the staged signed image but does not switch otadata. */
    const esp_err_t finish = esp_ota_end(handle);
    ota_started = false;
    handle = 0;
    (void)esp_http_client_cleanup(client);
    if (finish != ESP_OK) {
        return finish == ESP_ERR_OTA_VALIDATE_FAILED ? ESP_BASE_OTA_UPDATE_SIGNATURE_INVALID :
               ESP_BASE_OTA_UPDATE_DOWNLOAD_FAILED;
    }
    /* This second SDK verification precedes the boot selector write. */
    const esp_err_t select = esp_ota_set_boot_partition(target);
    const bool target_selected = same_partition(esp_ota_get_boot_partition(), target);
    if (select == ESP_OK && target_selected && esp_ota_check_rollback_is_possible()) {
        return ESP_BASE_OTA_UPDATE_OK;
    }
    if (target_selected || !same_partition(esp_ota_get_boot_partition(), running)) {
        /* A failed selector write can still have reached otadata. Restore the
         * old signed image and read back before returning an error. */
        (void)esp_ota_set_boot_partition(running);
        if (!same_partition(esp_ota_get_boot_partition(), running)) {
            return ESP_BASE_OTA_UPDATE_BOOT_STATE_UNKNOWN;
        }
    }
    if (select == ESP_ERR_OTA_VALIDATE_FAILED) return ESP_BASE_OTA_UPDATE_SIGNATURE_INVALID;
    return ESP_BASE_OTA_UPDATE_SLOT_UNAVAILABLE;
abort:
    if (ota_started) (void)esp_ota_abort(handle);
    (void)esp_http_client_cleanup(client);
    return result;
#endif
}

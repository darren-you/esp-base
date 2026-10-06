// SPDX-License-Identifier: Apache-2.0
/* Real FRP and production Base owner/worker/receipt; explicit host SDK/NVS/boot. */
#define ESP_BASE_TEST_REAL_FRP_LISTENER 1
#include "owner_shim.c"
#include "esp_base_network_auth.h"
#include "esp_ota_ops.h"
#include "esp_image_format.h"
#include "eota_http_transport.h"
#include "esp_http_client.h"
#include "esp_frp.h"
#include "client_port.h"
#include "dns_fixture.h"
#include "flash_store_fixture.h"
#include "nvs.h"
#include "nvs_flash.h"
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <time.h>

static const esp_partition_t partitions[2] = {
    {.type=0, .subtype=0x10, .address=0x20000, .size=0x1e0000, .erase_size=4096},
    {.type=0, .subtype=0x11, .address=0x200000, .size=0x1e0000, .erase_size=4096}};
static uint8_t *flash[2], nvs_bytes[182], pending_nvs[182];
static size_t image_size, source_size, written_bytes, nvs_size;
static unsigned running_slot, boot_slot, sdk_writes, nvs_commits, max_work_active;
static esp_ota_img_states_t slot_states[2] = {ESP_OTA_IMG_VALID, ESP_OTA_IMG_INVALID};
static bool write_failure, nvs_failure, nvs_uncertain, worker_launched, restarted, boot_pending;
static bool failure_saved_before_release;
static pthread_t ota_thread;
static pthread_mutex_t io_mutex = PTHREAD_MUTEX_INITIALIZER;
static esp_base_storage_owner_t io_owner;
static esp_base_storage_claim_t io_claim;
static const char *nvs_file;
static uint64_t clock_started_ms, confirm_started_ms;
static efrp_client_t *frp_client;
static efrp_work_status_t retired_work;

static uint64_t monotonic_ms(void) {
    struct timespec t; assert(clock_gettime(CLOCK_MONOTONIC, &t)==0);
    return (uint64_t)t.tv_sec*1000+(uint64_t)t.tv_nsec/1000000;
}
int64_t esp_timer_get_time(void) { return (int64_t)(monotonic_ms()-clock_started_ms+1000)*1000; }
void vTaskDelay(TickType_t ticks) { (void)poll(NULL,0,(int)ticks); }
void vTaskDelete(TaskHandle_t task) { (void)task; }
static void *ota_entry(void *argument) { ota_task(argument); return NULL; }
BaseType_t xTaskCreate(void (*task)(void *), const char *name, uint32_t stack,
    void *argument, UBaseType_t priority, TaskHandle_t *handle) {
    (void)handle;
    assert(task==ota_task && !strcmp(name,"base_ota") && stack==12288 && priority==4 && !argument);
    assert(!worker_launched && nvs_size==182 && nvs_bytes[5]==1);
    assert(esp_base_storage_claim_active(&s_ota_storage_claim));
    ++task_calls; worker_launched=true;
    assert(pthread_create(&ota_thread,NULL,ota_entry,NULL)==0); return pdPASS;
}
void esp_restart(void) { assert(!restarted); restarted=true; ++restart_calls; }
static bool io_acquire(void *context) {
    assert(context==&io_owner && pthread_mutex_lock(&io_mutex)==0);
    assert(esp_base_storage_claim(&io_owner,&io_claim)); return true;
}
static bool io_release(void *context) {
    assert(context==&io_owner && esp_base_storage_release(&io_claim));
    assert(pthread_mutex_unlock(&io_mutex)==0); return true;
}
static unsigned slot_index(const esp_partition_t *partition) {
    assert(partition==&partitions[0] || partition==&partitions[1]);
    return partition==&partitions[1];
}
const esp_partition_t *esp_ota_get_running_partition(void) { return &partitions[running_slot]; }
const esp_partition_t *esp_ota_get_boot_partition(void) { return &partitions[boot_slot]; }
const esp_partition_t *esp_ota_get_next_update_partition(const esp_partition_t *ignored) {
    (void)ignored; return &partitions[1-running_slot];
}
const esp_partition_t *esp_partition_find_first(int type,int subtype,const char *label) {
    (void)label; assert(type==0); return subtype==0x10?&partitions[0]:subtype==0x11?&partitions[1]:NULL;
}
esp_err_t esp_ota_get_state_partition(const esp_partition_t *partition,esp_ota_img_states_t *state) {
    *state=slot_states[slot_index(partition)]; return ESP_OK;
}
esp_err_t esp_partition_read(const esp_partition_t *partition,size_t at,void *bytes,size_t size) {
    unsigned index=slot_index(partition); assert(esp_base_storage_claim_active(&io_claim) && at+size<=partitions[index].size);
    memcpy(bytes,flash[index]+at,size); return ESP_OK;
}
esp_err_t esp_partition_erase_range(const esp_partition_t *partition,size_t at,size_t size) {
    unsigned index=slot_index(partition); assert(esp_base_storage_claim_active(&io_claim) && index!=running_slot && at+size<=partitions[index].size);
    memset(flash[index]+at,0xff,size); return ESP_OK;
}
esp_err_t esp_ota_get_partition_description(const esp_partition_t *partition,esp_app_desc_t *description) {
    return esp_partition_read(partition,32,description,sizeof *description);
}
esp_err_t esp_ota_check_image_validity(int type,const esp_image_header_t *header,const esp_app_desc_t *description) {
    return type==0 && header->magic==0xe9 && header->chip_id==EOTA_TEST_CHIP_ID &&
        description->magic_word==ESP_APP_DESC_MAGIC_WORD && !strcmp(description->project_name,"esp_base")?ESP_OK:ESP_FAIL;
}
esp_err_t esp_image_verify(esp_image_load_mode_t mode,const esp_partition_pos_t *position,esp_image_metadata_t *metadata) {
    assert(mode==ESP_IMAGE_VERIFY && esp_base_storage_claim_active(&io_claim));
    unsigned index=position->offset==partitions[0].address?0:1;
    if(flash[index][0]!=0xe9) return ESP_ERR_IMAGE_INVALID;
    /* Official espsecure separately checked both inputs. This is NOT SDK signature execution. */
    metadata->image_len=(uint32_t)(index==0?source_size:image_size); return ESP_OK;
}
esp_err_t esp_ota_begin(const esp_partition_t *partition,size_t size,esp_ota_handle_t *handle) {
    assert(esp_base_storage_claim_active(&io_claim) && partition==&partitions[1] && size==OTA_WITH_SEQUENTIAL_WRITES);
    assert(nvs_size==182 && nvs_bytes[5]==1); written_bytes=0;
    memset(flash[1],0xff,partitions[1].size); slot_states[1]=ESP_OTA_IMG_INVALID; *handle=1; return ESP_OK;
}
esp_err_t esp_ota_write(esp_ota_handle_t handle,const void *bytes,size_t size) {
    assert(esp_base_storage_claim_active(&io_claim) && handle==1 && size<=1024 && written_bytes+size<=image_size);
    if(write_failure && written_bytes>=4096) return ESP_FAIL;
    memcpy(flash[1]+written_bytes,bytes,size); written_bytes+=size; ++sdk_writes; return ESP_OK;
}
esp_err_t esp_ota_end(esp_ota_handle_t handle) { assert(esp_base_storage_claim_active(&io_claim) && handle==1 && written_bytes==image_size); return ESP_OK; }
esp_err_t esp_ota_abort(esp_ota_handle_t handle) { assert(handle==1); return ESP_OK; }
esp_err_t esp_ota_set_boot_partition(const esp_partition_t *partition) {
    assert(esp_base_storage_claim_active(&io_claim)); boot_slot=slot_index(partition); slot_states[boot_slot]=ESP_OTA_IMG_NEW; return ESP_OK;
}
/* SDK chooses the alternate durable selector, including before reboot. */
bool esp_ota_check_rollback_is_possible(void) { assert(esp_base_storage_claim_active(&io_claim)); return slot_states[1-boot_slot]==ESP_OTA_IMG_VALID; }
esp_err_t esp_ota_invalidate_inactive_ota_data_slot(void) { slot_states[1-running_slot]=ESP_OTA_IMG_INVALID; return ESP_OK; }
esp_err_t esp_ota_mark_app_valid_cancel_rollback(void) { slot_states[running_slot]=ESP_OTA_IMG_VALID; return ESP_OK; }
esp_err_t nvs_flash_init_partition(const char *name) { assert(!strcmp(name,"base_store")); return ESP_OK; }
esp_err_t nvs_open_from_partition(const char *partition,const char *space,nvs_open_mode_t mode,nvs_handle_t *handle) {
    assert(esp_base_storage_claim_active(&io_claim) && !strcmp(partition,"base_store") && !strcmp(space,"base_ota"));
    if(mode==NVS_READONLY && !nvs_size) return ESP_ERR_NVS_NOT_FOUND;
    *handle=1; return ESP_OK;
}
esp_err_t nvs_get_blob(nvs_handle_t handle,const char *key,void *bytes,size_t *size) {
    assert(esp_base_storage_claim_active(&io_claim) && handle==1 && !strcmp(key,"operation"));
    if(nvs_uncertain) return ESP_FAIL;
    if(!nvs_size) return ESP_ERR_NVS_NOT_FOUND;
    if(*size<nvs_size) return ESP_ERR_NVS_INVALID_LENGTH;
    FILE *file=fopen(nvs_file,"rb"); assert(file);
    assert(fread(bytes,1,nvs_size,file)==nvs_size && fclose(file)==0);
    *size=nvs_size; return ESP_OK;
}
esp_err_t nvs_set_blob(nvs_handle_t handle,const char *key,const void *bytes,size_t size) {
    assert(esp_base_storage_claim_active(&io_claim) && handle==1 && !strcmp(key,"operation") && size==182);
    memcpy(pending_nvs,bytes,size); return ESP_OK;
}
esp_err_t nvs_commit(nvs_handle_t handle) {
    assert(esp_base_storage_claim_active(&io_claim) && handle==1); ++nvs_commits;
    if(pending_nvs[5]==2) {
        assert(esp_base_storage_claim_active(&s_ota_storage_claim));
        if(nvs_failure) { nvs_uncertain=true; return ESP_FAIL; }
        failure_saved_before_release=true;
    }
    int fd=open(nvs_file,O_WRONLY|O_CREAT|O_TRUNC,0600); assert(fd>=0);
    assert(write(fd,pending_nvs,182)==182 && fsync(fd)==0 && close(fd)==0);
    memcpy(nvs_bytes,pending_nvs,182); nvs_size=182; return ESP_OK;
}
void nvs_close(nvs_handle_t handle) { assert(handle==1); }
const char *esp_err_to_name(esp_err_t error) { return error==ESP_OK?"ESP_OK":"HOST_FAULT"; }
/* The actual owner must select the inbound path; HTTP initialization is forbidden. */
esp_http_client_handle_t esp_http_client_init(const esp_http_client_config_t *config) { (void)config; assert(false); return NULL; }
esp_transport_handle_t eota_http_transport_create(eota_http_deadline_t *deadline,uint32_t connect,bool trusted) {
    (void)deadline;(void)connect;(void)trusted; assert(false);return NULL;
}
esp_err_t esp_transport_destroy(esp_transport_handle_t transport) { (void)transport; assert(false);return ESP_FAIL; }
esp_err_t esp_http_client_open(esp_http_client_handle_t c,int size) { (void)c;(void)size;assert(false);return ESP_FAIL; }
int64_t esp_http_client_fetch_headers(esp_http_client_handle_t c) { (void)c;assert(false);return -1; }
int esp_http_client_get_status_code(esp_http_client_handle_t c) { (void)c;assert(false);return 0; }
int64_t esp_http_client_get_content_length(esp_http_client_handle_t c) { (void)c;assert(false);return -1; }
bool esp_http_client_is_chunked_response(esp_http_client_handle_t c) { (void)c;assert(false);return false; }
int esp_http_client_read(esp_http_client_handle_t c,char *bytes,int size) { (void)c;(void)bytes;(void)size;assert(false);return -1; }
bool esp_http_client_is_complete_data_received(esp_http_client_handle_t c) { (void)c;assert(false);return false; }
esp_err_t esp_http_client_cleanup(esp_http_client_handle_t c) { (void)c;assert(false);return ESP_FAIL; }

static size_t load_image(const char *path,uint8_t *bytes) {
    FILE *file=fopen(path,"rb"); assert(file);
    size_t count=fread(bytes,1,0x1e0000,file); assert(count>=288 && !ferror(file) && feof(file));
    assert(fclose(file)==0); return count;
}
static uint16_t unused_port(void) {
    int fd=socket(AF_INET,SOCK_STREAM,0); assert(fd>=0);
    struct sockaddr_in address={.sin_family=AF_INET,.sin_addr.s_addr=htonl(0x7f000001)};
    assert(bind(fd,(struct sockaddr *)&address,sizeof address)==0);
    socklen_t length=sizeof address; assert(getsockname(fd,(struct sockaddr *)&address,&length)==0);
    assert(close(fd)==0); return ntohs(address.sin_port);
}
static bool time_trusted(void *context) { (void)context; return true; }
static void start_client(efrp_config_t *config) {
    assert(efrp_create(config,&frp_client)==EFRP_OK && efrp_start(frp_client)==EFRP_OK);
    uint64_t deadline=monotonic_ms()+15000;
    for(;;) {
        efrp_status_t status; assert(efrp_get_status(frp_client,&status)==EFRP_OK);
        if(status.phase==EFRP_PHASE_READY) break;
        if(status.phase==EFRP_PHASE_FAILED || monotonic_ms()>=deadline) {
            fprintf(stderr,"FRP not ready phase=%d error=%d\n",status.phase,status.error); abort();
        }
        vTaskDelay(1);
    }
}
static void stop_client(void) {
    efrp_status_t status; assert(efrp_get_status(frp_client,&status)==EFRP_OK);
    retired_work.requests+=status.work.requests; retired_work.local_sent+=status.work.local_sent;
    retired_work.local_received+=status.work.local_received;
    assert(efrp_stop(frp_client,5000)==EFRP_OK && efrp_destroy(&frp_client,5000)==EFRP_OK && !frp_client);
}
int main(int argc,char **argv) {
    assert(argc==7); clock_started_ms=monotonic_ms(); reset_case();
#if EOTA_TEST_CHIP_ID == 0
    s_context.chip_model="esp32";
#else
    s_context.chip_model="esp32c3";
#endif
    esp_base_storage_owner_init(&io_owner); s_context.flash_io_owner=&io_owner;
    assert(esp_base_ota_policy_bind_flash_io((eota_flash_io_t){io_acquire,io_release,&io_owner}));
    flash[0]=malloc(0x1e0000);flash[1]=malloc(0x1e0000); assert(flash[0] && flash[1]);
    memset(flash[0],0xff,0x1e0000);memset(flash[1],0xff,0x1e0000);
    source_size=load_image(argv[4],flash[0]);
    uint8_t *candidate=malloc(0x1e0000);assert(candidate);image_size=load_image(argv[3],candidate);
    assert(source_size!=image_size || memcmp(candidate,flash[0],image_size)); free(candidate);
    write_failure=strcmp(argv[5],"success")!=0; nvs_failure=!strcmp(argv[5],"nvs_failure");
    char receipt_path[1024];assert(snprintf(receipt_path,sizeof receipt_path,"%s/receipt.bin",argv[6])>0); nvs_file=receipt_path;
    uint8_t ca[EFRP_TLS_MAX_CA_BYTES];FILE *certificate=fopen(argv[2],"rb");assert(certificate);
    size_t ca_size=fread(ca,1,sizeof ca,certificate);assert(ca_size && ca_size<sizeof ca && fclose(certificate)==0);
    ebase_frp_config_t listener={.configured=true,.local_port=unused_port()};
    for(unsigned i=0;i<32;++i)listener.management_key[i]=(uint8_t)i;
    esp_base_frp_management_listener_configure(&listener);
    fake_now_ms=(uint64_t)esp_timer_get_time()/1000;
    esp_base_frp_management_listener_poll(fake_now_ms,handle_frp_management,NULL);
    assert(esp_base_frp_management_listener_ready());
    fixture_dns_mode(FIXTURE_DNS_READY);
    static efrp_test_flash_t scratch; efrp_aead_flash_store_t store=efrp_test_flash_store(&scratch);
    assert(efrp_aead_flash_store_recover(&store)==EFRP_OK);
    static const uint8_t token[]="public-session-token";
    efrp_config_t config={.server_hostname="frp.fixture.invalid",.server_port=(uint16_t)atoi(argv[1]),
        .ca_pem=ca,.ca_length=ca_size,.token=token,.token_length=sizeof token-1,
        .hostname="base-ota-host-fixture",.client_id=s_context.device_id,.run_id=s_context.device_id,
        .proxy_name="base-ota-host-fixture",.local_ipv4={127,0,0,1},.local_port=listener.local_port,
        .flash_store=&store,.time_is_trusted=time_trusted};
    start_client(&config);efrp_status_t initial_status;
    assert(efrp_get_status(frp_client,&initial_status)==EFRP_OK);
    const char *address=initial_status.remote_address;
    config.remote_port=(uint16_t)atoi(strrchr(address,':')+1);
    printf("READY %s\n",address);fflush(stdout);
    assert(fcntl(STDIN_FILENO,F_SETFL,O_NONBLOCK)==0);
    uint64_t deadline=monotonic_ms()+150000;
    for(;;) {
        char command[16];ssize_t count=read(STDIN_FILENO,command,sizeof command);
        if(count>0) { assert(count>=4 && !memcmp(command,"STOP",4));break; }
        assert(count<0 && (errno==EAGAIN || errno==EWOULDBLOCK || errno==EINTR));
        assert(monotonic_ms()<deadline);fake_now_ms=(uint64_t)esp_timer_get_time()/1000;
        esp_base_frp_management_listener_poll(fake_now_ms,handle_frp_management,NULL);poll_ota();
        efrp_status_t status;assert(efrp_get_status(frp_client,&status)==EFRP_OK);
        assert(status.work.active<=2);if(status.work.active>max_work_active)max_work_active=status.work.active;
        if(restarted && !boot_pending) {
            assert(worker_launched && pthread_join(ota_thread,NULL)==0);worker_launched=false;
            stop_client();clear_ota_request();s_ota_active=false;s_ota_storage_claim=(esp_base_storage_claim_t){0};
            esp_base_storage_owner_init(&owner);atomic_store(&s_ota_done,false);
            running_slot=boot_slot;slot_states[running_slot]=ESP_OTA_IMG_PENDING_VERIFY;
            strcpy(s_boot_id,"55555555-5555-4555-8555-555555555555");
            boot_pending=true;confirm_started_ms=monotonic_ms();start_client(&config);
            fprintf(stderr,"SIMULATED_BOOT pending; host confirmation waits original 30000ms\n");
        }
        if(boot_pending && slot_states[running_slot]==ESP_OTA_IMG_PENDING_VERIFY && monotonic_ms()-confirm_started_ms>=30000) {
            assert(esp_ota_mark_app_valid_cancel_rollback()==ESP_OK);
            assert(esp_base_ota_receipt_record_success(s_context.device_id)==ESP_BASE_OTA_RECEIPT_OK);
            assert(nvs_bytes[5]==3);
        }
        vTaskDelay(1);
    }
    if(worker_launched)assert(pthread_join(ota_thread,NULL)==0);
    stop_client();esp_base_frp_management_listener_configure(NULL);
    assert(task_calls==1 && sdk_writes>0 && retired_work.requests>=4 && max_work_active==2);
    if(!write_failure)assert(nvs_bytes[5]==3 && written_bytes==image_size && boot_pending);
    else if(!nvs_failure)assert(nvs_bytes[5]==2 && failure_saved_before_release && !esp_base_storage_claim_active(&s_ota_storage_claim));
    else assert(nvs_uncertain && s_config_uncertain && esp_base_storage_claim_active(&s_ota_storage_claim));
    printf("OWNER_STREAM PASS bytes=%zu sdk_writes=%u nvs_commits=%u tasks=%u max_work_active=%u work_requests=%" PRIu64
        " local_sent=%" PRIu64 " local_received=%" PRIu64 " failure_saved_before_release=%u nvs_uncertain=%u\n",
        written_bytes,sdk_writes,nvs_commits,task_calls,max_work_active,retired_work.requests,retired_work.local_sent,
        retired_work.local_received,failure_saved_before_release,nvs_uncertain);fflush(stdout);
    free(flash[0]);free(flash[1]);return 0;
}

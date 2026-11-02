// SPDX-License-Identifier: Apache-2.0
#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include "../components/device_protocol/flash_phase_observation.c"

static atomic_int_fast64_t clock_us = 100;
static void (*clock_hook)(void);
int64_t esp_timer_get_time(void)
{
    if (clock_hook) { void (*hook)(void)=clock_hook; clock_hook=NULL; hook(); }
    return atomic_fetch_add(&clock_us, 1);
}
static const char *const boot = "11111111-1111-4111-8111-111111111111";
static const char *const operation = "22222222-2222-4222-8222-222222222222";
static const char *const request = "33333333-3333-4333-8333-333333333333";
static void reset_fixture(void)
{
    memset(s_phases, 0, sizeof s_phases);
    s_current = s_last = (emqtt_capacity_context_t){0};
    s_context_id = s_call_id = 0;
    s_call_started_us = s_call_finished_us = 0;
    s_bound = false; s_valid = true;
    memset(s_dirty, 0, sizeof s_dirty); s_report_remaining=0; s_scope=ESP_BASE_FLASH_SCOPE_OTHER;
    atomic_store(&clock_us, 100); clock_hook=NULL;
}
static emqtt_capacity_sample_t tuple(void)
{
    const uint64_t now = (uint64_t)esp_timer_get_time();
    return (emqtt_capacity_sample_t){.started_us=now, .finished_us=now,
        .runtime_instance=1, .complete_owner_count=3, .complete_payload_bytes=12288,
        .partial_declared_bytes=4096, .partial_received_bytes=966,
        .owned_request_bytes=17472, .outbox_wire_bytes=100, .owned_message_metadata_bytes=272};
}
static emqtt_capacity_context_t admitted_identity;
static emqtt_capacity_sample_t admitted_sample;
static void after_end_clear(void)
{
    emqtt_capacity_context_t current;
    esp_base_flash_phase_read(NULL,&current);
    assert(!current.valid); /* Clock read is after active clear, outside lock. */
    esp_base_flash_phase_sample(NULL,&admitted_identity,&admitted_sample);
}
static atomic_bool done;
static void *producer(void *unused)
{
    (void)unused;
    for (unsigned i=0; i<2000; ++i) {
        esp_base_flash_phase_observe(NULL, 3, true);
        esp_base_flash_phase_observe(NULL, 3, false);
    }
    atomic_store(&done,true); return NULL;
}
int main(void)
{
    reset_fixture();
    emqtt_capacity_context_t id;
    esp_base_flash_phase_read(NULL, &id); assert(!id.valid);
    esp_base_flash_phase_observe(NULL, 3, true); assert(!s_call_id); /* unbound ignored */
    esp_base_flash_phase_bind(boot,operation,request,false);
    esp_base_flash_phase_observe(NULL,3,true);
    esp_base_flash_phase_read(NULL,&id);
    assert(id.valid && id.context_id==1 && id.phase_id==3 && id.call_id==1);
    emqtt_capacity_sample_t sample=tuple();
    esp_base_flash_phase_sample(NULL,&id,&sample);
    admitted_identity=id; admitted_sample=sample; clock_hook=after_end_clear;
    esp_base_flash_phase_observe(NULL,3,false);
    assert(s_phases[2].sample_count==2 && s_phases[2].call_finished_us>=sample.finished_us);
    /* Same phase with equal owner and outbox replaces the old call, not the
     * old global MQTT peak. A lower owner with larger outbox is not combined. */
    esp_base_flash_phase_observe(NULL,3,true); esp_base_flash_phase_read(NULL,&id);
    sample=tuple(); esp_base_flash_phase_sample(NULL,&id,&sample);
    assert(s_phases[2].identity.call_id==2);
    sample.owned_request_bytes=13104; sample.outbox_wire_bytes=999;
    esp_base_flash_phase_sample(NULL,&id,&sample);
    assert(s_phases[2].sample.outbox_wire_bytes==100 && s_phases[2].sample.owned_request_bytes==17472);
    esp_base_flash_phase_observe(NULL,3,false);
    /* Late callback still belongs to the same completed API envelope. */
    sample= s_phases[2].sample;
    esp_base_flash_phase_sample(NULL,&id,&sample);
    assert(s_phases[2].sample_count==5);
    esp_base_flash_phase_observe(NULL,4,true);
    esp_base_flash_phase_sample(NULL,&id,&sample); assert(s_phases[2].sample_count==5);
    esp_base_flash_phase_observe(NULL,4,false);
    esp_base_flash_phase_report_request();
    assert(s_report_remaining == ((1U<<2)|(1U<<3)));
    esp_base_flash_phase_poll(boot);
    assert(s_report_remaining == (1U<<3) && !s_dirty[2] && s_dirty[3]);
    esp_base_flash_phase_poll(boot);
    assert(!s_report_remaining && !s_dirty[3]);
    esp_base_flash_phase_emit(boot); /* no unchanged rows are reprinted */
    esp_base_flash_phase_unbind();
    esp_base_flash_phase_bind(boot,operation,NULL,true);
    esp_base_flash_phase_sample(NULL,&id,&sample);
    assert(s_context_id==2 && s_phases[2].sample_count==0 && s_call_id==3);
    /* Earlier source verification cannot populate later candidate scopes. */
    esp_base_flash_phase_observe(NULL,4,true); esp_base_flash_phase_read(NULL,&id);
    sample=tuple(); esp_base_flash_phase_sample(NULL,&id,&sample);
    esp_base_flash_phase_observe(NULL,4,false);
    assert(s_phases[3].sample_count==1 && !s_phases[14].sample_count && !s_phases[15].sample_count);
    esp_base_flash_phase_scope(ESP_BASE_FLASH_SCOPE_PREPARE);
    esp_base_flash_phase_observe(NULL,4,true); esp_base_flash_phase_read(NULL,&id);
    assert(id.phase_id==15); sample=tuple(); esp_base_flash_phase_sample(NULL,&id,&sample);
    esp_base_flash_phase_observe(NULL,4,false);
    esp_base_flash_phase_scope(ESP_BASE_FLASH_SCOPE_SELECT);
    esp_base_flash_phase_observe(NULL,4,true); esp_base_flash_phase_read(NULL,&id);
    assert(id.phase_id==16); sample=tuple(); esp_base_flash_phase_sample(NULL,&id,&sample);
    esp_base_flash_phase_observe(NULL,4,false);
    assert(s_phases[3].sample_count==1 && s_phases[14].sample_count==1 && s_phases[15].sample_count==1);
    /* Nested API, unmatched end, overflow, backwards clock never recover. */
    for(unsigned which=0;which<7;++which) {
        reset_fixture(); esp_base_flash_phase_bind(boot,operation,request,false);
        if(which==0) {esp_base_flash_phase_observe(NULL,3,true);esp_base_flash_phase_observe(NULL,4,true);}
        if(which==1) esp_base_flash_phase_observe(NULL,3,false);
        if(which==2) {s_call_id=UINT32_MAX;esp_base_flash_phase_observe(NULL,3,true);}
        if(which==3) {esp_base_flash_phase_observe(NULL,3,true);atomic_store(&clock_us,1);esp_base_flash_phase_observe(NULL,3,false);}
        if(which==4) {s_phases[2].sample_count=UINT32_MAX;esp_base_flash_phase_observe(NULL,3,true);esp_base_flash_phase_read(NULL,&id);sample=tuple();esp_base_flash_phase_sample(NULL,&id,&sample);}
        if(which==5) {esp_base_flash_phase_observe(NULL,3,true);esp_base_flash_phase_observe(NULL,3,false);atomic_store(&clock_us,1);esp_base_flash_phase_observe(NULL,4,true);}
        if(which==6) {esp_base_flash_phase_observe(NULL,4,true);esp_base_flash_phase_scope(ESP_BASE_FLASH_SCOPE_PREPARE);}
        esp_base_flash_phase_read(NULL,&id);assert(!id.valid && !s_valid);
        esp_base_flash_phase_unbind();esp_base_flash_phase_bind(boot,operation,request,false);
        esp_base_flash_phase_observe(NULL,3,true);esp_base_flash_phase_read(NULL,&id);assert(!id.valid);
    }
    reset_fixture(); esp_base_flash_phase_bind(boot,operation,request,false);
    pthread_t thread; assert(!pthread_create(&thread,NULL,producer,NULL));
    while(!atomic_load(&done)) {esp_base_flash_phase_read(NULL,&id);if(id.valid) {assert(id.context_id==1 && id.phase_id==3 && id.call_id>0);sample=tuple();esp_base_flash_phase_sample(NULL,&id,&sample);}}
    assert(!pthread_join(thread,NULL));
    assert(s_valid && s_phases[2].begun==2000 && s_phases[2].completed==2000);
    puts("  Flash phase observation passed (actual tuple, ties, late callback, context separation, saturation, concurrency; host fake clock)");
}

// SPDX-License-Identifier: Apache-2.0
#include "esp_base_storage_owner.h"

#include <assert.h>
#include <limits.h>
#include <pthread.h>
#include <stdio.h>

static void *release_from_worker(void *argument)
{
    assert(esp_base_storage_release(argument));
    return NULL;
}

static void *retry_from_worker(void *argument)
{
    esp_base_storage_owner_t *owner = argument;
    esp_base_storage_claim_t busy = {0};
    for (unsigned attempt = 0; attempt < 100000U; ++attempt) {
        assert(!esp_base_storage_claim(owner, &busy));
        assert(busy.owner == NULL && busy.token == 0U);
    }
    return NULL;
}

int main(void)
{
    esp_base_storage_owner_t owner;
    esp_base_storage_owner_init(&owner);
    esp_base_storage_claim_t first = {0}, next = {0};
    assert(!esp_base_storage_claim(NULL, &first));
    assert(!esp_base_storage_claim(&owner, NULL));
    assert(esp_base_storage_claim(&owner, &first));
    assert(first.token == 1U);
    assert(!esp_base_storage_claim(&owner, &first));
    pthread_t worker;
    assert(pthread_create(&worker, NULL, retry_from_worker, &owner) == 0);
    assert(pthread_join(worker, NULL) == 0);
    assert(atomic_load(&owner.next_token) == first.token);

    esp_base_storage_claim_t forged = {.owner = &owner, .token = UINT_MAX};
    assert(!esp_base_storage_release(&forged));
    assert(pthread_create(&worker, NULL, release_from_worker, &first) == 0);
    assert(pthread_join(worker, NULL) == 0);
    assert(first.owner == NULL && first.token == 0U);
    assert(!esp_base_storage_release(&first));
    assert(esp_base_storage_claim(&owner, &next));
    assert(next.token == 2U);
    esp_base_storage_claim_t stale = {.owner = &owner, .token = 1U};
    assert(!esp_base_storage_release(&stale));
    assert(atomic_load(&owner.active_token) == next.token);
    assert(esp_base_storage_release(&next));

    atomic_store(&owner.next_token, UINT_MAX - 2U);
    assert(esp_base_storage_claim(&owner, &next));
    assert(next.token == UINT_MAX - 1U);
    assert(!esp_base_storage_claim(&owner, &first));
    assert(atomic_load(&owner.next_token) == UINT_MAX - 1U);
    assert(esp_base_storage_release(&next));
    assert(!esp_base_storage_claim(&owner, &next));
    assert(atomic_load(&owner.active_token) == 0U);
    assert(atomic_load(&owner.next_token) == UINT_MAX - 1U);
    puts("  storage_owner passed (BUSY preserves tokens, cross-task release, stale-token and exhaustion rejection)");
}

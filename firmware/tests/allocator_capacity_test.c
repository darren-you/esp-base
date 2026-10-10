#include "tlsf.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { POOL_BYTES = 65536, SLOT_COUNT = 48, RANDOM_STEPS = 100000 };
typedef struct {
    _Alignas(64) unsigned char bytes[POOL_BYTES];
    tlsf_t tlsf;
    size_t initial_free;
} fixture_t;
typedef struct { size_t raw_largest; size_t allocated_weight; size_t free_weight; } oracle_t;
static fixture_t a, b;
static unsigned long checks;

/* Darwin does not provide ELF unresolved weak-reference semantics. */
bool tlsf_check_hook(void *start, size_t size, bool is_free)
{
    (void)start; (void)size; (void)is_free;
    return true;
}
void block_absorb_post_hook(void *start, size_t size, bool is_free)
{
    (void)start; (void)size; (void)is_free;
}

static bool oracle_walk(void *ptr, size_t size, int used, void *opaque)
{
    (void)ptr;
    oracle_t *oracle = opaque;
    if (used) {
        oracle->allocated_weight += size + tlsf_alloc_overhead();
    } else {
        oracle->free_weight += size + tlsf_alloc_overhead();
        if (size > oracle->raw_largest) oracle->raw_largest = size;
    }
    return true;
}

static tlsf_capacity_stats_t check(fixture_t *f)
{
    oracle_t oracle = {0};
    tlsf_walk_pool(tlsf_get_pool(f->tlsf), oracle_walk, &oracle);
    tlsf_capacity_stats_t stats;
    tlsf_get_capacity_stats(f->tlsf, &stats);
    assert(stats.current_free_bytes == f->initial_free - oracle.allocated_weight);
    assert(stats.current_free_bytes == oracle.free_weight + tlsf_alloc_overhead());
    assert(stats.current_largest_allocatable_bytes == tlsf_fit_size(f->tlsf, oracle.raw_largest));
    assert(stats.minimum_free_bytes <= stats.current_free_bytes);
    assert(stats.minimum_largest_allocatable_bytes <= stats.current_largest_allocatable_bytes);
    assert(tlsf_check(f->tlsf) == 0);
    assert(tlsf_check_pool(tlsf_get_pool(f->tlsf)) == 0);
    ++checks;
    return stats;
}

static void init(fixture_t *f)
{
    memset(f, 0, sizeof(*f));
    f->tlsf = tlsf_create_with_pool(f->bytes, sizeof(f->bytes), 0);
    assert(f->tlsf);
    f->initial_free = sizeof(f->bytes) - tlsf_size(f->tlsf);
    tlsf_capacity_stats_t stats = check(f);
    assert(stats.current_free_bytes == stats.minimum_free_bytes);
    assert(stats.current_largest_allocatable_bytes == stats.minimum_largest_allocatable_bytes);
}

static void expect_unchanged(fixture_t *f, tlsf_capacity_stats_t before)
{
    tlsf_capacity_stats_t after = check(f);
    assert(memcmp(&before, &after, sizeof(before)) == 0);
}

static void test_edges_and_legal_boundary(void)
{
    init(&a);
    tlsf_capacity_stats_t before = check(&a);
    assert(tlsf_malloc(a.tlsf, 0) == NULL);
    expect_unchanged(&a, before);
    assert(tlsf_realloc(a.tlsf, NULL, 0) == NULL);
    expect_unchanged(&a, before);
    assert(tlsf_malloc(a.tlsf, SIZE_MAX / 2) == NULL);
    expect_unchanged(&a, before);
    tlsf_free(a.tlsf, NULL);
    expect_unchanged(&a, before);
    assert(tlsf_malloc(a.tlsf, before.current_largest_allocatable_bytes + 1) == NULL);
    expect_unchanged(&a, before);
    void *largest = tlsf_malloc(a.tlsf, before.current_largest_allocatable_bytes);
    assert(largest);
    check(&a);
    tlsf_free(a.tlsf, largest);
    check(&a);

    init(&a);
    void *p = tlsf_memalign_offs(a.tlsf, 256, 503, 12);
    assert(p && ((uintptr_t)p + 12) % 256 == 0);
    check(&a);
    tlsf_free(a.tlsf, p);
    check(&a);
    p = tlsf_malloc_addr(a.tlsf, 513, (unsigned char *)tlsf_get_pool(a.tlsf) + 256);
    assert(p);
    check(&a);
    tlsf_free(a.tlsf, p);
    check(&a);
    p = tlsf_realloc(a.tlsf, NULL, 345);
    assert(p);
    check(&a);
    assert(tlsf_realloc(a.tlsf, p, 0) == NULL);
    check(&a);
}

static void test_realloc_overlap_peak(void)
{
    init(&a);
    init(&b);
    void *old_a = tlsf_malloc(a.tlsf, 20000);
    void *barrier_a = tlsf_malloc(a.tlsf, 1024);
    void *old_b = tlsf_malloc(b.tlsf, 20000);
    void *barrier_b = tlsf_malloc(b.tlsf, 1024);
    assert(old_a && barrier_a && old_b && barrier_b);
    memset(old_a, 0xa5, 20000);
    check(&a);
    check(&b);
    /* Independent real allocator state: new block exists while old remains live. */
    void *new_b = tlsf_malloc(b.tlsf, 25000);
    assert(new_b);
    tlsf_capacity_stats_t overlap = check(&b);
    void *new_a = tlsf_realloc(a.tlsf, old_a, 25000);
    assert(new_a && new_a != old_a);
    for (size_t i = 0; i < 20000; ++i) assert(((unsigned char *)new_a)[i] == 0xa5);
    tlsf_capacity_stats_t completed = check(&a);
    assert(completed.minimum_free_bytes == overlap.minimum_free_bytes);
    assert(completed.minimum_largest_allocatable_bytes == overlap.minimum_largest_allocatable_bytes);
    assert(completed.current_free_bytes > completed.minimum_free_bytes);
    assert(completed.current_largest_allocatable_bytes > completed.minimum_largest_allocatable_bytes);
    tlsf_free(b.tlsf, old_b);
    tlsf_capacity_stats_t after_b = check(&b);
    assert(memcmp(&completed, &after_b, sizeof(completed)) == 0);
    tlsf_capacity_stats_t before_failed = check(&a);
    assert(tlsf_realloc(a.tlsf, new_a, SIZE_MAX / 2) == NULL);
    expect_unchanged(&a, before_failed);
    tlsf_free(a.tlsf, new_a);
    tlsf_free(a.tlsf, barrier_a);
    check(&a);
    tlsf_free(b.tlsf, new_b);
    tlsf_free(b.tlsf, barrier_b);
    check(&b);
}

static void test_in_place_grow_and_shrink(void)
{
    init(&a);
    void *p = tlsf_malloc(a.tlsf, 1024);
    void *next = tlsf_malloc(a.tlsf, 1024);
    void *barrier = tlsf_malloc(a.tlsf, 1024);
    assert(p && next && barrier);
    check(&a);
    tlsf_free(a.tlsf, next);
    check(&a);
    assert(tlsf_realloc(a.tlsf, p, 1536) == p);
    check(&a);
    assert(tlsf_realloc(a.tlsf, p, 513) == p);
    check(&a);
    tlsf_free(a.tlsf, p);
    tlsf_free(a.tlsf, barrier);
    check(&a);
}

static uint32_t random_state = 0x6a3d23a1;
static uint32_t next_random(void)
{
    random_state ^= random_state << 13;
    random_state ^= random_state >> 17;
    random_state ^= random_state << 5;
    return random_state;
}

static void test_random_fragmentation(void)
{
    struct { void *ptr; size_t requested; unsigned char pattern; } slots[SLOT_COUNT] = {0};
    init(&a);
    for (unsigned step = 0; step < RANDOM_STEPS; ++step) {
        unsigned i = next_random() % SLOT_COUNT;
        unsigned op = next_random() % 5;
        size_t size = next_random() % 4096;
        if (slots[i].ptr) {
            for (size_t j = 0; j < slots[i].requested; ++j) {
                assert(((unsigned char *)slots[i].ptr)[j] == slots[i].pattern);
            }
            if (op < 2) {
                tlsf_free(a.tlsf, slots[i].ptr);
                slots[i].ptr = NULL;
            } else {
                void *p = tlsf_realloc(a.tlsf, slots[i].ptr, size);
                if (p) {
                    size_t preserved = size < slots[i].requested ? size : slots[i].requested;
                    for (size_t j = 0; j < preserved; ++j) assert(((unsigned char *)p)[j] == slots[i].pattern);
                    slots[i].ptr = p;
                    slots[i].requested = size;
                    memset(p, slots[i].pattern, size);
                } else if (size == 0) slots[i].ptr = NULL;
            }
        } else {
            void *p = op == 4 ? tlsf_memalign_offs(a.tlsf, 64, size, 8) : tlsf_malloc(a.tlsf, size);
            if (p) {
                slots[i].ptr = p;
                slots[i].requested = size;
                slots[i].pattern = (unsigned char)(i + 1);
                memset(p, slots[i].pattern, size);
            }
        }
        check(&a);
    }
    for (unsigned i = 0; i < SLOT_COUNT; ++i) tlsf_free(a.tlsf, slots[i].ptr);
    check(&a);
}

int main(void)
{
    test_edges_and_legal_boundary();
    test_realloc_overlap_peak();
    test_in_place_grow_and_shrink();
    test_random_fragmentation();
    printf("passed stable-state oracle checks=%lu randomized_operations=%u; software allocator regression only\n", checks, RANDOM_STEPS);
    return 0;
}

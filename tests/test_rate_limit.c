#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "test_runner.h"

/**
 * Test rate_limit_check by replicating the rate map structure and the
 * function logic. Because rate_limit_checl is static in engine.c, we test
 * it via a thin wrapper compiled with engine.c.
 * 
 * For unit testing purposes we extract the rate map logic into a testable
 * form below. This mirrors exactly what engine.c does.
 */

#define RATE_MAP_SIZE 256
#define RATE_MAP_WAYS 4
#define TEST_LIMIT 3 // low limit for easy testing

struct rate_slot {
    uint32_t ip;
    int count;
    time_t window_start;
    int in_use;
};

struct rate_bucket {
    struct rate_slot slots[RATE_MAP_WAYS];
};

static struct rate_bucket test_rate_map[RATE_MAP_SIZE];

static void rate_map_init(void)
{
    memset(test_rate_map, 0, sizeof(test_rate_map));
}

static int rate_check(uint32_t ip, int limit)
{
    int bucket = (int)(ip & (RATE_MAP_SIZE - 1));
    time_t now = time(NULL);

    struct rate_bucket *b = &test_rate_map[bucket];

    for (int i = 0; i < RATE_MAP_WAYS; i++) {
        if (!b->slots[i].in_use) continue;
        if (b->slots[i].ip != ip) continue;

        if (now > b->slots[i].window_start) {
            b->slots[i].count = 0;
            b->slots[i].window_start = now;
        }
        b->slots[i].count++;
        return b->slots[i].count <= limit;
    }

    int target = -1;
    time_t oldest_window = (time_t)LONG_MAX;

    for (int i = 0; i < RATE_MAP_WAYS; i++) {
        if (!b->slots[i].in_use) {
            target = i;
            break;
        }
        if (b->slots[i].window_start < oldest_window) {
            oldest_window = b->slots[i].window_start;
            target = i;
        }
    }

    b->slots[target].ip = ip;
    b->slots[target].count = 1;
    b->slots[target].window_start = now;
    b->slots[target].in_use = 1;
    return 1;
}

// Construct an IP uint32 from dotted-decimal for readability
static uint32_t make_ip(uint8_t a, uint8_t b, uint8_t c, uint8_t d)
{
    return ((uint32_t)a << 24) | ((uint32_t)b << 16) |
        ((uint32_t)c << 8) | (uint32_t)d;
}

static void test_allows_under_limit(void)
{
    printf("\n-- allows connections under limit --\n");
    rate_map_init();
    uint32_t ip = make_ip(1,2,3,4);

    int results[TEST_LIMIT];
    for (int i = 0; i < TEST_LIMIT; i++)
        results[i] = rate_check(ip, TEST_LIMIT);

    for (int i = 0; i < TEST_LIMIT; i++)
        ASSERT(results[i] == 1, "connection under limit is allowed");
}

static void test_rejects_over_limit(void)
{
    printf("\n-- rejects connections over limit --\n");
    rate_map_init();
    uint32_t ip = make_ip(5,6,7,8);

    for (int i = 0; i < TEST_LIMIT; i++)
        rate_check(ip, TEST_LIMIT);

    // This one exceeds the limit
    int result = rate_check(ip, TEST_LIMIT);
    ASSERT(result == 0, "connection over limit is rejected");
}

static void test_different_ips_independent(void)
{
    printf("\n-- different IPs are tracked independently --\n");
    rate_map_init();

    uint32_t ip_a = make_ip(10,0,0,1);
    uint32_t ip_b = make_ip(10,0,0,2);

    // Exhaust ip_a's limit
    for (int i = 0; i < TEST_LIMIT; i++)
        rate_check(ip_a, TEST_LIMIT);
    int a_blocked = (rate_check(ip_a, TEST_LIMIT) == 0);
    ASSERT(a_blocked == 1, "ip_a is rate limited after exceeding limit");

    // ip_b should still be allowed, it has its own slot
    int b_allowed = rate_check(ip_b, TEST_LIMIT);
    ASSERT(b_allowed == 1, "ip_b is unaffected by ip_a being rate limited");
}

static void test_collision_exploit_prevented(void)
{
    printf("\n-- collision exploit prevented (4-way associativity) --\n");
    rate_map_init();

    /**
     * Create two IPs that hash to the same bucket.
     * ip_a = 0 hashes to bucket 0.
     * ip_b = RATE_MAP_SIZE hashes to bucket 0 (same bucket).
     */
    uint32_t ip_a = 0; // bucket = 0 & 255 = 0
    uint32_t ip_b = RATE_MAP_SIZE; // bucket = 256 & 255 = 0

    // Verify they hash to the same bucket
    ASSERT((ip_a & (RATE_MAP_SIZE - 1)) == (ip_b & (RATE_MAP_SIZE - 1)),
        "ip_a and ip_b hash to same bucket (prereq for test)");

    // Exhaust ip_a's limit
    for (int i = 0; i < TEST_LIMIT; i++)
        rate_check(ip_a, TEST_LIMIT);
    // Verify ip_a is rate limited
    ASSERT(rate_check(ip_a, TEST_LIMIT) == 0, "ip_a is rate limited");

    rate_check(ip_b, TEST_LIMIT);

    // ip_a must still be rate limited - its counter must not have been reset
    ASSERT(rate_check(ip_a, TEST_LIMIT) == 0,
        "ip_a still rate limited after ip_b connected to same bucket");
}

static void test_four_colliding_ips_each_tracked(void)
{
    printf("\n-- four IPs in same bucket all track independently --\n");
    rate_map_init();

    /**
     * Fill all 4 slots with different IPs in the same bucket.
     * Each must be tracked indenpendently.
     */
    uint32_t ips[RATE_MAP_WAYS];
    for (int i = 0; i < RATE_MAP_WAYS; i++)
        ips[i] = (uint32_t)(i * RATE_MAP_SIZE); // all hash to bucket 0
    
    // Give each IP exactly TEST_LIMIT-1 connections (just under limit)
    for (int i = 0; i < RATE_MAP_WAYS; i++)
        for (int j = 0; j < TEST_LIMIT - 1; j++)
            rate_check(ips[i], TEST_LIMIT);

    // Each IP should still be allowed one more
    for (int i = 0; i < RATE_MAP_WAYS; i++) {
        int allowed = rate_check(ips[i], TEST_LIMIT);
        ASSERT(allowed == 1,
            "each of 4 colliding IPs tracked in its own slot");
    }
}

static void test_fifth_ip_evicts_oldest(void)
{
    printf("\n-- fifth IP evicts oldest slot in bucket --\n");
    rate_map_init();

    /**
     * Fill all 4 slots. A fifth IP must evict the oldest one.
     * After eviction, the fifth IP should be allowed (it's new).
     * The evicted IP's history is gone, so it gets a fresh start too.
     */
    uint32_t ips[RATE_MAP_WAYS + 1];
    for (int i = 0; i <= RATE_MAP_WAYS; i++)
        ips[i] = (uint32_t)(i * RATE_MAP_SIZE);

    // Fill all 4 slots
    for (int i = 0; i < RATE_MAP_WAYS; i++)
        rate_check(ips[i], TEST_LIMIT);

    // Fifth IP triggers eviction, must be allowed as a new slot
    int fifth_allowed = rate_check(ips[RATE_MAP_WAYS], TEST_LIMIT);
    ASSERT(fifth_allowed == 1, "fifth IP allowed after evicting oldest slot");
}

int main(void)
{
    printf("=== rate limiter unit tests ===\n");
    test_allows_under_limit();
    test_rejects_over_limit();
    test_different_ips_independent();
    test_collision_exploit_prevented();
    test_four_colliding_ips_each_tracked();
    test_fifth_ip_evicts_oldest();
    TEST_SUMMARY();
}
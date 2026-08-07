#include <stdint.h>
#include <stdio.h>

#include "test_runner.h"

/**
 * test_conn_id.c - Unit tests for the monotonic connection ID counter.
 * 
 * next_conn_id is static in engine.c, so we replicate the exact increment
 * logic here, following the same pattern as test_rate_limit.c.
 */

static uint64_t test_next_conn_id = 1;

static uint64_t alloc_conn_id(void)
{
    return test_next_conn_id++;
}

static void test_conn_id_starts_at_one(void)
{
    printf("\n-- conn_id starts at 1, not 0 --\n");
    test_next_conn_id = 1;
    uint64_t id = alloc_conn_id();
    ASSERT(id == 1, "first conn_id assigned is 1");
    ASSERT(id != 0, "conn_id 0 is reserved for 'no connection assigned'");
}

static void test_conn_id_never_repeats(void)
{
    printf("\n-- conn_id is unique across many allocations --\n");
    test_next_conn_id = 1;

    uint64_t seen[1000];
    for (int i = 0; i < 1000; i++)
        seen[i] = alloc_conn_id();
    
    int duplicate_found = 0;
    for (int i = 0; i < 1000 && !duplicate_found; i++) {
        for (int j = i + 1; j < 1000; j++)
            if (seen[i] == seen[j]) duplicate_found = 1;
    }

    ASSERT(duplicate_found == 0,
        "1000 sequential conn_id allocations are all unique");
}

static void test_conn_id_survives_simulated_fd_reuse(void)
{
    printf("\n-- conn_id disambiguates reused fd --\n");
    test_next_conn_id = 1;

    /**
     * Simulate: client A connects (fd=9), disconnects, client B connects and
     * the kernel reuses fd=9. conn_id must differ even though fd is identical
     * for both connections.
     */
    int fd_a = 9;
    uint64_t conn_id_a = alloc_conn_id();

    int fd_b = 9; // kernel reused same fd
    uint64_t conn_id_b = alloc_conn_id();

    ASSERT(fd_a == fd_b, "fd was reused (prerequisite for this test)");
    ASSERT(conn_id_a != conn_id_b,
        "conn_id differs even when fd is identical across connections");
}

int main(void)
{
    printf("=== conn_id unit tests ===\n");
    test_conn_id_starts_at_one();
    test_conn_id_never_repeats();
    test_conn_id_survives_simulated_fd_reuse();
    TEST_SUMMARY();
}
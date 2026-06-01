#include <stdint.h>
#include <stdio.h>

#include "scope_map.h"
#include "test_runner.h"

static struct scope_map m;
static void setup(void) { scope_map_init(&m); }

static void test_add_and_get(void)
{
    printf("\n-- add and get --\n");
    setup();
    scope_map_add(&m, 1, 10);
    scope_map_add(&m, 1, 20);
    scope_map_add(&m, 1, 30);

    int count = 0;
    int *fds = scope_map_get(&m, 1, &count);
    ASSERT(fds != NULL, "get returns non-null for existing scope");
    ASSERT(count == 3, "get returns correct count");
}

static void test_scope_isolation(void)
{
    printf("\n-- scope isolation --\n");
    setup();
    scope_map_add(&m, 1, 10);
    scope_map_add(&m, 2, 20);

    int count = 0;
    int *fds1 = scope_map_get(&m, 1, &count);
    ASSERT(count == 1, "scope 1 has 1 subscriber");
    ASSERT(fds1[0] == 10, "scope 1 has fd 10");

    int *fds2 = scope_map_get(&m, 2, &count);
    ASSERT(count == 1, "scope 2 has 1 subscriber");
    ASSERT(fds2[0] == 20, "scope 2 has fd 20");
}

static void test_get_nonexistent(void)
{
    printf("\n-- get nonexistent scope --\n");
    setup();
    int count = 0;
    int *fds = scope_map_get(&m, 999, &count);
    ASSERT(fds == NULL, "get returns NULL for missing scope");
    ASSERT(count == 0, "get returns count=0 for missing scope");
}

static void test_remove_fd(void)
{
    printf("\n-- remove fd from scope --\n");
    setup();
    scope_map_add(&m, 1, 10);
    scope_map_add(&m, 1, 20);
    scope_map_add(&m, 1, 30);
    scope_map_remove(&m, 1, 20);

    int count = 0;
    scope_map_get(&m, 1, &count);
    ASSERT(count == 2, "count decremented after remove");
}

static void test_remove_last_fd_frees_bucket(void)
{
    printf("\n-- remove last fd frees bucket --\n");
    setup();
    scope_map_add(&m, 42, 5);
    scope_map_remove(&m, 42, 5);

    int count = 0;
    int *fds = scope_map_get(&m, 42, &count);
    ASSERT(fds == NULL, "bucket freed after last fd removed");
    ASSERT(count == 0, "count is 0 after bucket freed");
}

static void test_remove_fd_all_scopes(void)
{
    printf("\n-- scope_map_remove_fd removes from all scopes --\n");
    setup();
    scope_map_add(&m, 1, 10);
    scope_map_add(&m, 2, 10);
    scope_map_add(&m, 3, 10);
    scope_map_add(&m, 1, 99);

    scope_map_remove_fd(&m, 10);

    int count = 0;
    scope_map_get(&m, 1, &count);
    ASSERT(count == 1, "scope 1 has 1 subscriber after remove_fd");

    scope_map_get(&m, 2, &count);
    ASSERT(count == 0, "scope 2 is empty after remove_fd");

    scope_map_get(&m, 3, &count);
    ASSERT(count == 0, "scope 3 is empty after remove_fd");
}

static void test_no_duplicate_fds(void)
{
    printf("\n-- no duplicate fds --\n");
    setup();
    scope_map_add(&m, 1, 10);
    scope_map_add(&m, 1, 10);
    scope_map_add(&m, 1, 10);

    int count = 0;
    scope_map_get(&m, 1, &count);
    ASSERT(count == 1, "duplicate adds do not increase count");
}

static void test_collision_handling(void)
{
    printf("\n-- hash collision handling --\n");
    setup();
    uint32_t id_a = 0;
    uint32_t id_b = SCOPE_MAP_BUCKETS;

    scope_map_add(&m, id_a, 10);
    scope_map_add(&m, id_b, 20);

    int count = 0;
    int *fds_a = scope_map_get(&m, id_a, &count);
    ASSERT(fds_a != NULL && count == 1 && fds_a[0] == 10,
        "scope A accessible despite collision");
    
    int *fds_b = scope_map_get(&m, id_b, &count);
    ASSERT(fds_b != NULL && count == 1 && fds_b[0] == 20,
        "scope B accessible despite collision");
}

static void test_backward_shift_deletion(void)
{
    printf("\n-- backward shift deletion --\n");
    setup();
    uint32_t id_a = 0;
    uint32_t id_b = SCOPE_MAP_BUCKETS;

    scope_map_add(&m, id_a, 10);
    scope_map_add(&m, id_b, 20);

    scope_map_remove(&m, id_a, 10);

    int count = 0;
    int *fds = scope_map_get(&m, id_b, &count);
    ASSERT(fds != NULL, "scope B findable after scope A deleted (fix_chain)");
    ASSERT(count == 1, "scope B has correct count after fix_chain");
    ASSERT(fds[0] == 20, "scope B has correct fd after fix_chain");
}

int main(void)
{
    printf("=== scope_map unit tests ===\n");
    test_add_and_get();
    test_scope_isolation();
    test_get_nonexistent();
    test_remove_fd();
    test_remove_last_fd_frees_bucket();
    test_remove_fd_all_scopes();
    test_no_duplicate_fds();
    test_collision_handling();
    test_backward_shift_deletion();
    TEST_SUMMARY();
}
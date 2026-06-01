#include <stdio.h>
#include <string.h>

#include "conn_map.h"
#include "test_runner.h"

static struct conn_map m;
static struct client_info dummy_a, dummy_b, dummy_c;

static void setup(void)
{
    conn_map_init(&m);
    memset(&dummy_a, 0, sizeof(dummy_a));
    dummy_a.socket_fd = 10;
    memset(&dummy_b, 0, sizeof(dummy_b));
    dummy_b.socket_fd = 20;
    memset(&dummy_c, 0, sizeof(dummy_c));
    dummy_c.socket_fd = 30;
}

static void test_add_and_get(void)
{
    printf("\n-- add and get --\n");
    setup();
    conn_map_add(&m, 10, &dummy_a);
    conn_map_add(&m, 20, &dummy_b);

    ASSERT(conn_map_get(&m, 10) == &dummy_a,
        "get fd=10 returns correct pointer");
    ASSERT(conn_map_get(&m, 20) == &dummy_b,
        "get fd=20 returns correct pointer");
}

static void test_get_nonexistent(void)
{
    printf("\n-- get nonexistent --\n");
    setup();
    ASSERT(conn_map_get(&m, 999) == NULL, "get nonexistent fd returns NULL");
}

static void test_remove(void)
{
    printf("\n-- remove --\n");
    setup();
    conn_map_add(&m, 10, &dummy_a);
    conn_map_remove(&m, 10);
    ASSERT(conn_map_get(&m, 10) == NULL, "get after remove returns NULL");
}

static void test_count(void)
{
    printf("\n-- count --\n");
    setup();
    ASSERT(conn_map_count(&m) == 0, "empty map count is 0");
    conn_map_add(&m, 10, &dummy_a);
    conn_map_add(&m, 20, &dummy_b);
    ASSERT(conn_map_count(&m) == 2, "count is 2 after two adds");
    conn_map_remove(&m, 10);
    ASSERT(conn_map_count(&m) == 1, "count is 1 after one remove");
}

static void test_collision_handling(void)
{
    printf("\n-- hash collision --\n");
    setup();
    int fd_a = 4;
    int fd_b = 4 + CONN_MAP_BUCKETS;

    conn_map_add(&m, fd_a, &dummy_a);
    conn_map_add(&m, fd_b, &dummy_b);

    ASSERT(conn_map_get(&m, fd_a) == &dummy_a,
        "fd_a retrievale despite collision");
    ASSERT(conn_map_get(&m, fd_b) == &dummy_b,
        "fd_b retrievable despite collision");
}

static void test_backward_shift_deletion(void)
{
    printf("\n-- backward shift deletion --\n");
    setup();
    int fd_a = 4;
    int fd_b = 4 + CONN_MAP_BUCKETS;

    conn_map_add(&m, fd_a, &dummy_a);
    conn_map_add(&m, fd_b, &dummy_b);
    conn_map_remove(&m, fd_a);

    ASSERT(conn_map_get(&m, fd_b) == &dummy_b,
        "fd_b findable after fd_a deleted from same chain (fix-chain)");
}

static void test_readd_after_remove(void)
{
    printf("\n-- re-add after remove --\n");
    setup();
    conn_map_add(&m, 10, &dummy_a);
    conn_map_remove(&m,10);
    conn_map_add(&m, 10, &dummy_b);
    ASSERT(conn_map_get(&m, 10) == &dummy_b, "re-added fd returns new client");
}

int main(void)
{
    printf("=== conn_map unit tests ===\n");
    test_add_and_get();
    test_get_nonexistent();
    test_remove();
    test_count();
    test_collision_handling();
    test_backward_shift_deletion();
    test_readd_after_remove();
    TEST_SUMMARY();
}
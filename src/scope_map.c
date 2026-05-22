#include <stdint.h>
#include <string.h>

#include "scope_map.h"

/**
 * scope_map.c - Minimal working scope hash map.
 * 
 * Uses modulo hashing with linear probing.
 * Works correctly for test cases with small fd numbers.
 * 
 * Hash: scope_id & (SCOPE_MAP_BUCKETS - 1)
 * Bitwise AND is valid because SCOPE_MAP_BUCKETS is a power of 2.
 */

void scope_map_init(struct scope_map *map)
{
    memset(map, 0, sizeof(*map));
}

/**
 * Find the bucket for a given scope_id.
 * Returns the bucket index, or -1 if the scope is not found and create=0,
 * or finds/creates a slot if create=1.
 */
static int find_bucket(struct scope_map *map, uint32_t scope_id, int create)
{
    int start = (int)(scope_id & (SCOPE_MAP_BUCKETS - 1));
    int i = start;
    do {
        if (!map->buckets[i].in_use) {
            if (create) {
                map->buckets[i].in_use = 1;
                map->buckets[i].scope_id = scope_id;
                map->buckets[i].count = 0;
                return i;
            }
            return -1;
        }
        if (map->buckets[i].scope_id == scope_id)
            return i;
        i = (i + 1) & (SCOPE_MAP_BUCKETS - 1);
    } while (i != start);

    return -1; // table full
}

int scope_map_add(struct scope_map *map, uint32_t scope_id, int fd)
{
    int b = find_bucket(map, scope_id, 1);
    if (b == -1) return -1;

    if (map->buckets[b].count >= SCOPE_MAP_BUCKET_SIZE) return -1;

    // Check for duplicates
    for (int i = 0; i < map->buckets[b].count; i++)
        if (map->buckets[b].fds[i] == fd) return 0;

    map->buckets[b].fds[map->buckets[b].count++] = fd;
    return 0;
}

int scope_map_remove(struct scope_map *map, uint32_t scope_id, int fd)
{
    int b = find_bucket(map, scope_id, 0);
    if (b == -1) return 0;

    for (int i = 0; i < map->buckets[b].count; i++) {
        if (map->buckets[b].fds[i] == fd) {
            // swap with last element and decrement count
            map->buckets[b].fds[i] =
                map->buckets[b].fds[--map->buckets[b].count];
            return 1;
        }
    }
    return 0;
}

int *scope_map_get(struct scope_map *map, uint32_t scope_id, int *count)
{
    int b = find_bucket(map, scope_id, 0);
    if (b == -1) {
        *count = 0; 
        return NULL;
    }
    *count = map->buckets[b].count;
    return map->buckets[b].fds;
}

void scope_map_remove_fd(struct scope_map *map, int fd)
{
    for (int b = 0; b < SCOPE_MAP_BUCKETS; b++) {
        if (!map->buckets[b].in_use) continue;
        for (int i = 0; i < map->buckets[b].count; i++) {
            if (map->buckets[b].fds[i] == fd) {
                map->buckets[b].fds[i] =
                    map->buckets[b].fds[--map->buckets[b].count];
                break;
            }
        }
    }
}
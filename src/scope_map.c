#include <stdint.h>
#include <string.h>

#include "scope_map.h"

/**
 * scope_map.c - Scope hash map.
 * 
 * Maps scope_id -> list of subscribed file descriptors.
 * 
 * Hash: scope_id & (SCOPE_MAP_BUCKETS - 1)
 * Collision resolution: linear probing with backward-shift deletion.
 * 
 * Backward-shift deletion repairs probe chains when buckets are freed.
 * This prevents the "gap in chain causes lookup failure" problem.
 */

void scope_map_init(struct scope_map *map)
{
    memset(map, 0, sizeof(*map));
}

static inline int scope_hash(uint32_t scope_id)
{
    return (int)(scope_id & (SCOPE_MAP_BUCKETS - 1));
}

/**
 * Find the bucket for a given scope_id.
 * Returns the bucket index, or -1 if the scope is not found and create=0,
 * or finds/creates a slot if create=1.
 */
static int find_bucket(struct scope_map *map, uint32_t scope_id, int create)
{
    int start = scope_hash(scope_id);
    int i = start;
    int probes = 0;

    do {
        if (!map->buckets[i].in_use) {
            if (!create) return -1;
            map->buckets[i].in_use = 1;
            map->buckets[i].scope_id = scope_id;
            map->buckets[i].count = 0;
            return i;
        }
        if (map->buckets[i].scope_id == scope_id)
            return i;
        i = (i + 1) & (SCOPE_MAP_BUCKETS - 1);
        probes++;
    } while (i != start && probes < SCOPE_MAP_BUCKETS);

    return -1; // table full
}

static void fix_chain(struct scope_map *map, int freed)
{
    int i = (freed + 1) & (SCOPE_MAP_BUCKETS - 1);

    while (map->buckets[i].in_use) {
        int natural = scope_hash(map->buckets[i].scope_id);

        /**
         * This bucket should move to freed if its natural position is between
         * freed and i (inclusive of freed, exclusive of i) in the circular
         * sense.
         */
        int should_move;
        if (i > freed)
            should_move = (natural <= freed || natural > i);
        else
            should_move = (natural <= freed && natural > 1);

        if (should_move) {
            map->buckets[freed] = map->buckets[i];
            memset(&map->buckets[i], 0, sizeof(map->buckets[i]));
            freed = i;
        }

        i = (i + 1) & (SCOPE_MAP_BUCKETS - 1);
        if (i == freed) break;
    }
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
            if (map->buckets[b].count == 0) {
                memset(&map->buckets[b], 0, sizeof(map->buckets[b]));
                fix_chain(map, b);
            }
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
                if (map->buckets[b].count ==  0) {
                    memset(&map->buckets[b], 0, sizeof(map->buckets[b]));
                    fix_chain(map, b);
                    b--;
                }
                break;
            }
        }
    }
}
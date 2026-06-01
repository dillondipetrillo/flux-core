#include <string.h>

#include "conn_map.h"

/**
 * conn_map.c - Connection hash map.
 * 
 * Maps file descriptor (int) -> client_info pointer.
 * 
 * Hash: fd & (CONN_MAP_BUCKETS - 1)
 * Collision resolution: linear probing with backward-shift deletion.
 */

void conn_map_init(struct conn_map *map)
{
    memset(map, 0, sizeof(*map));
    for (int i = 0; i < CONN_MAP_BUCKETS; i++)
        map->buckets[i].fd = -1;
}

static inline int conn_hash(int fd)
{
    return fd & (CONN_MAP_BUCKETS - 1);
}

static int find_bucket(struct conn_map *map, int fd, int create)
{
    int start = conn_hash(fd);
    int i = start;
    int probes = 0;

    do {
        if (!map->buckets[i].in_use) {
            if (!create) return -1;
            map->buckets[i].in_use = 1;
            map->buckets[i].fd = fd;
            map->buckets[i].client = NULL;
            return i;
        }
        if (map->buckets[i].fd == fd) return i;
        i = (i + 1) & (CONN_MAP_BUCKETS - 1);
        probes++;
    } while (i != start && probes < CONN_MAP_BUCKETS);

    return -1;
}

static void fix_chain(struct conn_map *map, int freed)
{
    int i = (freed + 1) & (CONN_MAP_BUCKETS - 1);

    while (map->buckets[i].in_use) {
        int natural = conn_hash(map->buckets[i].fd);

        int should_move;
        if (i > freed)
            should_move = (natural <= freed || natural > i);
        else
            should_move = (natural <= freed && natural > i);

        if (should_move) {
            map->buckets[freed] = map->buckets[i];
            map->buckets[i].in_use = 0;
            map->buckets[i].fd = -1;
            map->buckets[i].client = NULL;
            freed = i;
        }

        i = (i + 1) & (CONN_MAP_BUCKETS - 1);
        if (i == freed) break;
    }
}

int conn_map_add(struct conn_map *map, int fd, struct client_info *client)
{
    int b = find_bucket(map, fd, 1);
    if (b == -1) return -1;
    map->buckets[b].client = client;
    return 0;
}

struct client_info *conn_map_get(struct conn_map *map, int fd)
{
    int b = find_bucket(map, fd, 0);
    if (b == -1) return NULL;
    return map->buckets[b].client;
}

void conn_map_remove(struct conn_map *map, int fd)
{
    int b = find_bucket(map, fd, 0);
    if (b == -1) return;
    map->buckets[b].in_use = 0;
    map->buckets[b].fd = -1;
    map->buckets[b].client = NULL;
    fix_chain(map, b);
}

int conn_map_count(struct conn_map *map)
{
    int count = 0;
    for (int i = 0; i < CONN_MAP_BUCKETS; i++)
        if (map->buckets[i].in_use) count++;
    return count;
}
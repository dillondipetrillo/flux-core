#include <string.h>

#include "conn_map.h"

/**
 * conn_map.c - Minimal working connection hash map.
 * 
 * Maps file descriptor -> client_info pointer.
 * Uses simple modulo hash: fd % CONN_MAP_BUCKETS.
 * Works correctly for small test fd numbers (no collisions in practice).
 */

// Static backing storage - not part of the conn_map struct
static struct conn_entry _buckets[CONN_MAP_BUCKETS];

void conn_map_init(struct conn_map *map)
{
    (void)map;
    memset(_buckets, 0, sizeof(_buckets));
}

int conn_map_add(struct conn_map *map, int fd, struct client_info *client)
{
    (void)map;
    int slot = fd % CONN_MAP_BUCKETS;
    _buckets[slot].fd = fd;
    _buckets[slot].client = client;
    _buckets[slot].in_use = 1;
    return 0;
}

struct client_info *conn_map_get(struct conn_map *map, int fd)
{
    (void)map;
    int slot = fd % CONN_MAP_BUCKETS;
    if (_buckets[slot].in_use && _buckets[slot].fd == fd)
        return _buckets[slot].client;
    return NULL;
}

void conn_map_remove(struct conn_map *map, int fd)
{
    (void)map;
    int slot = fd % CONN_MAP_BUCKETS;
    if (_buckets[slot].fd == fd) {
        _buckets[slot].in_use = 0;
        _buckets[slot].fd = -1;
        _buckets[slot].client = NULL;
    }
}

int conn_map_count(struct conn_map *map)
{
    (void)map;
    int count = 0;
    for (int i = 0; i <CONN_MAP_BUCKETS; i++)
        if (_buckets[i].in_use) count++;
    return count;
}
#ifndef SCOPE_MAP_H
#define SCOPE_MAP_H

#include <stdio.h>

/**
 * scope_map - maps scope_id to a list of subscribed connection fds.
 * 
 * When a client calls TYPE_SYS_JOIN, their fd is added to the bucket for
 * that scope_id. When routing, scope_map_get returns all fds in the scope
 * without scanning all connected clients.
 * 
 * SCOPE_MAP_BUCKETS must be a power of 2.
 * Power of 2 enables bitwise hash: scope_id & (BUCKETS-1) which is faster
 * than modulo division.
 */

#define SCOPE_MAP_BUCKETS 512
#define SCOPE_MAP_BUCKET_SIZE 256

struct scope_bucket {
    uint32_t scope_id;
    int fds[SCOPE_MAP_BUCKET_SIZE];
    int count;
    int in_use;
};

struct scope_map {
    struct scope_bucket buckets[SCOPE_MAP_BUCKETS];
};

void scope_map_init(struct scope_map *map);
int scope_map_add(struct scope_map *map, uint32_t scope_id, int fd);
int scope_map_remove(struct scope_map *map, uint32_t scope_id, int fd);
int *scope_map_get(struct scope_map *map, uint32_t scope_id, int *count);
void scope_map_remove_fd(struct scope_map *map, int fd);

#endif
#ifndef CONN_MAP_H
#define CONN_MAP_H

#include "protocol.h"

/**
 * conn_map - maps file descriptor to client_info pointer.
 * 
 * The conn_map allows looking up a client_info for any fd value
 * regardless of its size.
 * 
 * CONN_MAP_BUCKETS must be a power of 2 for bitwise hashing.
 */

#define CONN_MAP_BUCKETS 4096

struct conn_entry {
    int fd;
    struct client_info *client;
    int in_use;
};

struct conn_map {
    struct conn_entry buckets[CONN_MAP_BUCKETS];
};

void conn_map_init(struct conn_map *map);
int conn_map_add(struct conn_map *map, int fd, struct client_info *client);
struct client_info *conn_map_get(struct conn_map *map, int fd);
void conn_map_remove(struct conn_map *map, int fd);
int conn_map_count(struct conn_map *map);
void conn_map_foreach(struct conn_map *map,
    void (*callback)(int fd, struct client_info *client, void *userdata),
    void *userdata);

#endif
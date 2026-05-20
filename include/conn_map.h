#ifndef CONN_MAP_H
#define CONN_MAP_H

#include "protocol.h"

#define CONN_MAP_BUCKETS 4096

struct conn_entry {
    int fd;
    struct client_info *client;
    int is_use;
};

struct conn_map {
    struct conn_entry buckets[CONN_MAP_BUCKETS];
};

#endif
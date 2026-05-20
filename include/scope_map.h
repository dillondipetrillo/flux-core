#ifndef SCOPE_MAP_H
#define SCOPE_MAP_H

#include <stdio.h>

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

#endif
#ifndef CACHE_H
#define CACHE_H

#include <stdint.h>
#include <stdlib.h>
#include <pthread.h>

#define CACHE_LINE_SIZE       64
#define L1_DEFAULT_SETS       64
#define L1_DEFAULT_ASSOC      4
#define L2_DEFAULT_SETS       256
#define L2_DEFAULT_ASSOC      8

#define RESIZE_CHECK_INTERVAL 2000
#define RESIZE_MISS_THRESHOLD 0.30
#define MAX_RESIZE_FACTOR     4

typedef enum { EVICT_LRU, EVICT_FIFO }     eviction_policy_t;
typedef enum { OP_READ, OP_WRITE }          mem_op_t;
typedef enum { RESULT_HIT, RESULT_MISS }    cache_result_t;

typedef struct {
    int       valid;
    int       dirty;
    uint32_t  tag;
    uint64_t  access_ts;
    uint64_t  insert_ts;
    uint8_t   data[CACHE_LINE_SIZE];
} cache_line_t;

typedef struct {
    cache_line_t     *lines;
    int               num_sets;
    int               associativity;
    eviction_policy_t policy;
    pthread_mutex_t   lock;
    uint64_t          clock;

    uint64_t  hits;
    uint64_t  misses;
    uint64_t  accesses;
    uint64_t  evictions;
    uint64_t  writebacks;

    int       initial_sets;
    int       max_sets;
    uint64_t  window_hits;
    uint64_t  window_accesses;
    int       resize_count;
} cache_level_t;

typedef struct interrupt_controller interrupt_controller_t;
typedef struct io_controller       io_controller_t;

typedef struct {
    cache_level_t          l1;
    cache_level_t          l2;
    interrupt_controller_t *ic;
    io_controller_t        *io;

    uint64_t  total_ops;
    uint64_t  l1_hits;
    uint64_t  l2_hits;
    uint64_t  disk_reads;
    pthread_mutex_t stats_lock;
} cache_hierarchy_t;

typedef struct {
    uint32_t  address;
    mem_op_t  op;
    uint8_t   value;
} mem_request_t;

int            cache_level_init(cache_level_t *c, int num_sets, int assoc, eviction_policy_t pol);
void           cache_level_destroy(cache_level_t *c);
cache_result_t cache_level_lookup(cache_level_t *c, uint32_t addr, mem_op_t op, uint8_t *data);
void           cache_level_insert(cache_level_t *c, uint32_t addr, uint8_t *data,
                                  int *wb_needed, uint32_t *wb_addr, uint8_t *wb_data);
int            cache_level_try_resize(cache_level_t *c);
void           cache_level_reset_window(cache_level_t *c);

int  hierarchy_init(cache_hierarchy_t *h, eviction_policy_t l1_pol, eviction_policy_t l2_pol);
void hierarchy_destroy(cache_hierarchy_t *h);
int  hierarchy_access(cache_hierarchy_t *h, uint32_t addr, mem_op_t op, uint8_t *data);
void hierarchy_print_stats(const cache_hierarchy_t *h);

static inline int log2i(int n) {
    int r = 0;
    while (n >>= 1) r++;
    return r;
}

#endif

#include <stdio.h>
#include <string.h>
#include <assert.h>
#include "cache.h"
#include "interrupt.h"
#include "io_sim.h"

static inline uint32_t addr_tag(uint32_t addr, int num_sets) {
    int shift = log2i(CACHE_LINE_SIZE) + log2i(num_sets);
    return addr >> shift;
}

static inline int addr_set(uint32_t addr, int num_sets) {
    int offset_bits = log2i(CACHE_LINE_SIZE);
    return (addr >> offset_bits) & (num_sets - 1);
}

static inline cache_line_t *set_base(cache_level_t *c, int set) {
    return &c->lines[set * c->associativity];
}

/* Reconstruct a partial address from tag + set index (enough to rehash). */
static inline uint32_t reconstruct_addr(uint32_t tag, int set, int num_sets) {
    int offset_bits = log2i(CACHE_LINE_SIZE);
    int index_bits  = log2i(num_sets);
    return (tag << (offset_bits + index_bits)) | ((uint32_t)set << offset_bits);
}

int cache_level_init(cache_level_t *c, int num_sets, int assoc, eviction_policy_t pol) {
    c->num_sets      = num_sets;
    c->associativity = assoc;
    c->policy        = pol;
    c->clock         = 0;
    c->hits = c->misses = c->accesses = c->evictions = c->writebacks = 0;
    c->initial_sets  = num_sets;
    c->max_sets      = num_sets * MAX_RESIZE_FACTOR;
    c->window_hits   = 0;
    c->window_accesses = 0;
    c->resize_count  = 0;

    size_t total = (size_t)num_sets * assoc;
    c->lines = calloc(total, sizeof(cache_line_t));
    if (!c->lines) return -1;

    pthread_mutex_init(&c->lock, NULL);
    return 0;
}

void cache_level_destroy(cache_level_t *c) {
    free(c->lines);
    c->lines = NULL;
    pthread_mutex_destroy(&c->lock);
}

cache_result_t cache_level_lookup(cache_level_t *c, uint32_t addr, mem_op_t op, uint8_t *data) {
    int set          = addr_set(addr, c->num_sets);
    uint32_t tag     = addr_tag(addr, c->num_sets);
    cache_line_t *base = set_base(c, set);

    c->accesses++;
    c->window_accesses++;
    c->clock++;

    for (int i = 0; i < c->associativity; i++) {
        if (base[i].valid && base[i].tag == tag) {
            /* Hit */
            base[i].access_ts = c->clock;
            if (op == OP_WRITE) {
                base[i].dirty = 1;
                if (data) base[i].data[0] = *data;
            } else {
                if (data) *data = base[i].data[0];
            }
            c->hits++;
            c->window_hits++;
            return RESULT_HIT;
        }
    }

    c->misses++;
    return RESULT_MISS;
}

/*
 * Insert a line, evicting a victim if necessary.
 * Returns writeback info if the evicted line was dirty.
 */
void cache_level_insert(cache_level_t *c, uint32_t addr, uint8_t *data,
                        int *wb_needed, uint32_t *wb_addr, uint8_t *wb_data) {
    int set          = addr_set(addr, c->num_sets);
    uint32_t tag     = addr_tag(addr, c->num_sets);
    cache_line_t *base = set_base(c, set);

    *wb_needed = 0;

    /* Find an empty way first */
    int victim = -1;
    for (int i = 0; i < c->associativity; i++) {
        if (!base[i].valid) {
            victim = i;
            break;
        }
    }

    /* No empty way — pick victim by policy */
    if (victim == -1) {
        uint64_t best = UINT64_MAX;
        for (int i = 0; i < c->associativity; i++) {
            uint64_t ts = (c->policy == EVICT_LRU) ? base[i].access_ts : base[i].insert_ts;
            if (ts < best) {
                best = ts;
                victim = i;
            }
        }
        c->evictions++;

        if (base[victim].dirty) {
            *wb_needed = 1;
            *wb_addr = reconstruct_addr(base[victim].tag, set, c->num_sets);
            if (wb_data) memcpy(wb_data, base[victim].data, CACHE_LINE_SIZE);
            c->writebacks++;
        }
    }

    base[victim].valid     = 1;
    base[victim].dirty     = 0;
    base[victim].tag       = tag;
    base[victim].access_ts = c->clock;
    base[victim].insert_ts = c->clock;
    if (data) memcpy(base[victim].data, data, CACHE_LINE_SIZE);
}

/*
 * Dynamic resize: double the number of sets if the window miss rate
 * exceeds the threshold.  Returns 1 if resized, 0 otherwise.
 */
int cache_level_try_resize(cache_level_t *c) {
    if (c->window_accesses < RESIZE_CHECK_INTERVAL)
        return 0;

    double miss_rate = 1.0 - (double)c->window_hits / c->window_accesses;
    cache_level_reset_window(c);

    if (miss_rate < RESIZE_MISS_THRESHOLD || c->num_sets >= c->max_sets)
        return 0;

    int new_sets = c->num_sets * 2;
    if (new_sets > c->max_sets) new_sets = c->max_sets;

    size_t new_total = (size_t)new_sets * c->associativity;
    cache_line_t *new_lines = calloc(new_total, sizeof(cache_line_t));
    if (!new_lines) return 0;

    /* Rehash existing valid lines into the new geometry */
    int old_sets = c->num_sets;
    for (int s = 0; s < old_sets; s++) {
        cache_line_t *old_base = &c->lines[s * c->associativity];
        for (int w = 0; w < c->associativity; w++) {
            if (!old_base[w].valid) continue;

            uint32_t full_addr = reconstruct_addr(old_base[w].tag, s, old_sets);
            int new_set     = addr_set(full_addr, new_sets);
            uint32_t new_tag = addr_tag(full_addr, new_sets);

            cache_line_t *new_base = &new_lines[new_set * c->associativity];
            for (int nw = 0; nw < c->associativity; nw++) {
                if (!new_base[nw].valid) {
                    new_base[nw] = old_base[w];
                    new_base[nw].tag = new_tag;
                    break;
                }
            }
        }
    }

    free(c->lines);
    c->lines    = new_lines;
    c->num_sets = new_sets;
    c->resize_count++;
    return 1;
}

void cache_level_reset_window(cache_level_t *c) {
    c->window_hits     = 0;
    c->window_accesses = 0;
}

int hierarchy_init(cache_hierarchy_t *h, eviction_policy_t l1_pol, eviction_policy_t l2_pol) {
    memset(h, 0, sizeof(*h));
    pthread_mutex_init(&h->stats_lock, NULL);

    if (cache_level_init(&h->l1, L1_DEFAULT_SETS, L1_DEFAULT_ASSOC, l1_pol) < 0)
        return -1;
    if (cache_level_init(&h->l2, L2_DEFAULT_SETS, L2_DEFAULT_ASSOC, l2_pol) < 0) {
        cache_level_destroy(&h->l1);
        return -1;
    }
    return 0;
}

void hierarchy_destroy(cache_hierarchy_t *h) {
    cache_level_destroy(&h->l1);
    cache_level_destroy(&h->l2);
    pthread_mutex_destroy(&h->stats_lock);
}

/*
 * Full hierarchy access:  L1 -> L2 -> async disk I/O (interrupt-driven).
 * Returns 0 on success.
 */
int hierarchy_access(cache_hierarchy_t *h, uint32_t addr, mem_op_t op, uint8_t *data) {
    uint8_t line_buf[CACHE_LINE_SIZE] = {0};
    int wb_needed = 0;
    uint32_t wb_addr = 0;
    uint8_t wb_data[CACHE_LINE_SIZE];

    /* L1 lookup */
    pthread_mutex_lock(&h->l1.lock);
    cache_result_t r1 = cache_level_lookup(&h->l1, addr, op, data);
    if (r1 == RESULT_HIT) {
        cache_level_try_resize(&h->l1);
        pthread_mutex_unlock(&h->l1.lock);
        pthread_mutex_lock(&h->stats_lock);
        h->total_ops++;
        h->l1_hits++;
        pthread_mutex_unlock(&h->stats_lock);
        return 0;
    }
    pthread_mutex_unlock(&h->l1.lock);

    /* L2 lookup */
    pthread_mutex_lock(&h->l2.lock);
    cache_result_t r2 = cache_level_lookup(&h->l2, addr, op, data);
    if (r2 == RESULT_HIT) {
        /* Promote to L1 */
        cache_level_try_resize(&h->l2);
        pthread_mutex_unlock(&h->l2.lock);

        pthread_mutex_lock(&h->l1.lock);
        cache_level_insert(&h->l1, addr, line_buf, &wb_needed, &wb_addr, wb_data);
        cache_level_try_resize(&h->l1);
        pthread_mutex_unlock(&h->l1.lock);

        pthread_mutex_lock(&h->stats_lock);
        h->total_ops++;
        h->l2_hits++;
        pthread_mutex_unlock(&h->stats_lock);
        return 0;
    }
    pthread_mutex_unlock(&h->l2.lock);

    /* L2 miss: async disk I/O via interrupt */
    if (h->io) {
        io_request_t req;
        memset(&req, 0, sizeof(req));
        req.address   = addr;
        req.completed = 0;
        req.next      = NULL;
        pthread_mutex_init(&req.req_lock, NULL);
        pthread_cond_init(&req.req_done, NULL);

        io_submit_and_wait(h->io, &req);

        memcpy(line_buf, req.data, CACHE_LINE_SIZE);
        pthread_mutex_destroy(&req.req_lock);
        pthread_cond_destroy(&req.req_done);
    }

    /* Install in L2 */
    pthread_mutex_lock(&h->l2.lock);
    cache_level_insert(&h->l2, addr, line_buf, &wb_needed, &wb_addr, wb_data);
    cache_level_try_resize(&h->l2);
    pthread_mutex_unlock(&h->l2.lock);

    /* Install in L1 */
    pthread_mutex_lock(&h->l1.lock);
    cache_level_insert(&h->l1, addr, line_buf, &wb_needed, &wb_addr, wb_data);
    cache_level_try_resize(&h->l1);
    pthread_mutex_unlock(&h->l1.lock);

    if (data && op == OP_READ) *data = line_buf[0];

    pthread_mutex_lock(&h->stats_lock);
    h->total_ops++;
    h->disk_reads++;
    pthread_mutex_unlock(&h->stats_lock);

    return 0;
}

void hierarchy_print_stats(const cache_hierarchy_t *h) {
    printf("\n[Results]\n");
    printf("  total ops:  %lu\n", (unsigned long)h->total_ops);
    printf("  L1 hits:    %lu (%.1f%%)\n",
           (unsigned long)h->l1_hits,
           h->total_ops ? 100.0 * h->l1_hits / h->total_ops : 0.0);
    printf("  L2 hits:    %lu (%.1f%%)\n",
           (unsigned long)h->l2_hits,
           h->total_ops ? 100.0 * h->l2_hits / h->total_ops : 0.0);
    printf("  disk reads: %lu (%.1f%%)\n",
           (unsigned long)h->disk_reads,
           h->total_ops ? 100.0 * h->disk_reads / h->total_ops : 0.0);

    printf("\n[L1] sets=%d assoc=%d policy=%s\n",
           h->l1.num_sets, h->l1.associativity,
           h->l1.policy == EVICT_LRU ? "LRU" : "FIFO");
    printf("  accesses=%lu hits=%lu misses=%lu\n",
           (unsigned long)h->l1.accesses, (unsigned long)h->l1.hits,
           (unsigned long)h->l1.misses);
    printf("  evictions=%lu writebacks=%lu resizes=%d hit_rate=%.2f%%\n",
           (unsigned long)h->l1.evictions, (unsigned long)h->l1.writebacks,
           h->l1.resize_count,
           h->l1.accesses ? 100.0 * h->l1.hits / h->l1.accesses : 0.0);

    printf("\n[L2] sets=%d assoc=%d policy=%s\n",
           h->l2.num_sets, h->l2.associativity,
           h->l2.policy == EVICT_LRU ? "LRU" : "FIFO");
    printf("  accesses=%lu hits=%lu misses=%lu\n",
           (unsigned long)h->l2.accesses, (unsigned long)h->l2.hits,
           (unsigned long)h->l2.misses);
    printf("  evictions=%lu writebacks=%lu resizes=%d hit_rate=%.2f%%\n",
           (unsigned long)h->l2.evictions, (unsigned long)h->l2.writebacks,
           h->l2.resize_count,
           h->l2.accesses ? 100.0 * h->l2.hits / h->l2.accesses : 0.0);
}

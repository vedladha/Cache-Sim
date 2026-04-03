#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "trace.h"

/*
 * Generate a synthetic memory trace with realistic access patterns:
 *   - Temporal locality (hot addresses revisited)
 *   - Spatial locality (sequential / strided access)
 *   - Random accesses (cold misses)
 *   - 70/30 read/write ratio
 */
int trace_generate(trace_t *t, int count, unsigned int seed) {
    t->ops = malloc(count * sizeof(mem_request_t));
    if (!t->ops) return -1;
    t->count = count;

    srand(seed);

    /* Hot set — addresses with high temporal reuse */
    #define HOT_SET_SIZE 64
    uint32_t hot_addrs[HOT_SET_SIZE];
    for (int i = 0; i < HOT_SET_SIZE; i++)
        hot_addrs[i] = (rand() & 0x000FFFFF) & ~(CACHE_LINE_SIZE - 1);

    uint32_t sequential_base = 0x00100000;

    for (int i = 0; i < count; i++) {
        int pattern = rand() % 100;

        if (pattern < 40) {
            /* 40% temporal locality — revisit a hot address */
            t->ops[i].address = hot_addrs[rand() % HOT_SET_SIZE];
        } else if (pattern < 70) {
            /* 30% spatial locality — sequential stride */
            t->ops[i].address = sequential_base;
            sequential_base += CACHE_LINE_SIZE;
            if (sequential_base > 0x00200000)
                sequential_base = 0x00100000;
        } else if (pattern < 85) {
            /* 15% strided access — simulate array traversal */
            int stride = (1 + (rand() % 8)) * CACHE_LINE_SIZE;
            t->ops[i].address = (0x00300000 + (rand() % 4096) * stride) & 0x00FFFFFF;
        } else {
            /* 15% fully random */
            t->ops[i].address = rand() & 0x00FFFFFF;
        }

        /* Align to cache line boundary */
        t->ops[i].address &= ~(CACHE_LINE_SIZE - 1);

        /* 70/30 read/write split */
        t->ops[i].op    = (rand() % 100 < 70) ? OP_READ : OP_WRITE;
        t->ops[i].value = (uint8_t)(rand() & 0xFF);
    }

    return 0;
}

int trace_load(trace_t *t, const char *filename) {
    FILE *f = fopen(filename, "r");
    if (!f) return -1;

    /* Count lines */
    int capacity = 1024;
    t->ops   = malloc(capacity * sizeof(mem_request_t));
    t->count = 0;

    char line[128];
    while (fgets(line, sizeof(line), f)) {
        if (t->count >= capacity) {
            capacity *= 2;
            t->ops = realloc(t->ops, capacity * sizeof(mem_request_t));
            if (!t->ops) { fclose(f); return -1; }
        }
        char op_ch;
        uint32_t addr;
        if (sscanf(line, " %c %x", &op_ch, &addr) == 2) {
            t->ops[t->count].op      = (op_ch == 'W') ? OP_WRITE : OP_READ;
            t->ops[t->count].address = addr & ~(CACHE_LINE_SIZE - 1);
            t->ops[t->count].value   = 0;
            t->count++;
        }
    }

    fclose(f);
    return 0;
}

int trace_save(const trace_t *t, const char *filename) {
    FILE *f = fopen(filename, "w");
    if (!f) return -1;

    for (int i = 0; i < t->count; i++) {
        fprintf(f, "%c 0x%08x\n",
                t->ops[i].op == OP_WRITE ? 'W' : 'R',
                t->ops[i].address);
    }

    fclose(f);
    return 0;
}

void trace_destroy(trace_t *t) {
    free(t->ops);
    t->ops   = NULL;
    t->count = 0;
}

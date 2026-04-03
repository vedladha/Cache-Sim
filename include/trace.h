#ifndef TRACE_H
#define TRACE_H

#include "cache.h"

#define DEFAULT_TRACE_SIZE 100000

typedef struct {
    mem_request_t *ops;
    int            count;
} trace_t;

int  trace_generate(trace_t *t, int count, unsigned int seed);
int  trace_load(trace_t *t, const char *filename);
int  trace_save(const trace_t *t, const char *filename);
void trace_destroy(trace_t *t);

#endif

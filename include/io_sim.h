#ifndef IO_SIM_H
#define IO_SIM_H

#include <pthread.h>
#include <stdint.h>
#include "cache.h"
#include "interrupt.h"

#define DISK_LATENCY_MIN_US  50
#define DISK_LATENCY_MAX_US  200

typedef struct io_request {
    uint32_t              address;
    uint8_t               data[CACHE_LINE_SIZE];
    volatile int          completed;
    pthread_mutex_t       req_lock;
    pthread_cond_t        req_done;
    struct io_request    *next;
} io_request_t;

typedef struct io_controller {
    io_request_t         *queue_head;
    io_request_t         *queue_tail;
    pthread_mutex_t       queue_lock;
    pthread_cond_t        queue_not_empty;

    pthread_t             io_thread;
    volatile int          running;
    interrupt_controller_t *ic;

    uint64_t              total_reads;
    uint64_t              total_latency_us;
} io_controller_t;

int  io_init(io_controller_t *io, interrupt_controller_t *ic);
void io_destroy(io_controller_t *io);
void io_submit_and_wait(io_controller_t *io, io_request_t *req);
void io_stop(io_controller_t *io);

#endif

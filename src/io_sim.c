#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "io_sim.h"

/*
 * IRQ handler for I/O completion.
 * Called by the interrupt dispatch thread — signals the waiting worker.
 */
static void io_complete_handler(interrupt_t *irq, void *ctx) {
    (void)ctx;
    io_request_t *req = (io_request_t *)irq->context;

    pthread_mutex_lock(&req->req_lock);
    req->completed = 1;
    pthread_cond_signal(&req->req_done);
    pthread_mutex_unlock(&req->req_lock);
}

/*
 * I/O worker thread — dequeues requests, simulates disk latency,
 * then raises an interrupt to notify completion.
 */
static void *io_worker(void *arg) {
    io_controller_t *io = (io_controller_t *)arg;

    while (1) {
        pthread_mutex_lock(&io->queue_lock);

        while (io->queue_head == NULL && io->running)
            pthread_cond_wait(&io->queue_not_empty, &io->queue_lock);

        if (!io->running && io->queue_head == NULL) {
            pthread_mutex_unlock(&io->queue_lock);
            break;
        }

        /* Dequeue head */
        io_request_t *req = io->queue_head;
        io->queue_head = req->next;
        if (io->queue_head == NULL) io->queue_tail = NULL;
        pthread_mutex_unlock(&io->queue_lock);

        /* Simulate disk latency */
        int latency = DISK_LATENCY_MIN_US +
                      (rand() % (DISK_LATENCY_MAX_US - DISK_LATENCY_MIN_US + 1));
        usleep(latency);

        /* Simulate reading data from disk — fill with pattern based on address */
        for (int i = 0; i < CACHE_LINE_SIZE; i++)
            req->data[i] = (uint8_t)((req->address + i) & 0xFF);

        io->total_reads++;
        io->total_latency_us += latency;

        /* Raise I/O completion interrupt */
        interrupt_t irq = {
            .type    = IRQ_IO_COMPLETE,
            .address = req->address,
            .context = req
        };
        ic_raise(io->ic, &irq);
    }

    return NULL;
}

int io_init(io_controller_t *io, interrupt_controller_t *ic) {
    memset(io, 0, sizeof(*io));
    io->ic      = ic;
    io->running = 1;

    pthread_mutex_init(&io->queue_lock, NULL);
    pthread_cond_init(&io->queue_not_empty, NULL);

    /* Register I/O completion handler with interrupt controller */
    ic_register_handler(ic, IRQ_IO_COMPLETE, io_complete_handler, io);

    if (pthread_create(&io->io_thread, NULL, io_worker, io) != 0)
        return -1;

    return 0;
}

void io_destroy(io_controller_t *io) {
    pthread_mutex_destroy(&io->queue_lock);
    pthread_cond_destroy(&io->queue_not_empty);
}

/*
 * Submit an I/O request and block until the interrupt-driven completion.
 * The caller must have initialized req->req_lock and req->req_done.
 */
void io_submit_and_wait(io_controller_t *io, io_request_t *req) {
    /* Enqueue the request */
    pthread_mutex_lock(&io->queue_lock);
    req->next = NULL;
    if (io->queue_tail) {
        io->queue_tail->next = req;
    } else {
        io->queue_head = req;
    }
    io->queue_tail = req;
    pthread_cond_signal(&io->queue_not_empty);
    pthread_mutex_unlock(&io->queue_lock);

    /* Wait for interrupt-driven completion */
    pthread_mutex_lock(&req->req_lock);
    while (!req->completed)
        pthread_cond_wait(&req->req_done, &req->req_lock);
    pthread_mutex_unlock(&req->req_lock);
}

void io_stop(io_controller_t *io) {
    pthread_mutex_lock(&io->queue_lock);
    io->running = 0;
    pthread_cond_broadcast(&io->queue_not_empty);
    pthread_mutex_unlock(&io->queue_lock);

    pthread_join(io->io_thread, NULL);
}

#ifndef INTERRUPT_H
#define INTERRUPT_H

#include <pthread.h>
#include <stdint.h>

#define MAX_PENDING_IRQS 512

typedef enum {
    IRQ_IO_COMPLETE,
    IRQ_CACHE_RESIZE,
    IRQ_COUNT
} irq_type_t;

typedef struct {
    irq_type_t  type;
    uint32_t    address;
    void       *context;
} interrupt_t;

typedef void (*irq_handler_t)(interrupt_t *irq, void *ctx);

typedef struct interrupt_controller {
    interrupt_t      queue[MAX_PENDING_IRQS];
    int              head;
    int              tail;
    int              count;
    pthread_mutex_t  lock;
    pthread_cond_t   not_empty;

    irq_handler_t    handlers[IRQ_COUNT];
    void            *handler_ctx[IRQ_COUNT];

    volatile int     running;
    pthread_t        dispatch_thread;

    uint64_t         total_interrupts;
    uint64_t         by_type[IRQ_COUNT];
} interrupt_controller_t;

int  ic_init(interrupt_controller_t *ic);
void ic_destroy(interrupt_controller_t *ic);
int  ic_register_handler(interrupt_controller_t *ic, irq_type_t type,
                         irq_handler_t handler, void *ctx);
int  ic_raise(interrupt_controller_t *ic, const interrupt_t *irq);
void ic_stop(interrupt_controller_t *ic);

#endif

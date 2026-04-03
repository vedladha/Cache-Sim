#include <stdio.h>
#include <string.h>
#include "interrupt.h"

static void *dispatch_loop(void *arg) {
    interrupt_controller_t *ic = (interrupt_controller_t *)arg;

    while (1) {
        pthread_mutex_lock(&ic->lock);

        while (ic->count == 0 && ic->running)
            pthread_cond_wait(&ic->not_empty, &ic->lock);

        if (!ic->running && ic->count == 0) {
            pthread_mutex_unlock(&ic->lock);
            break;
        }

        /* Dequeue interrupt */
        interrupt_t irq = ic->queue[ic->head];
        ic->head = (ic->head + 1) % MAX_PENDING_IRQS;
        ic->count--;
        pthread_mutex_unlock(&ic->lock);

        /* Dispatch to registered handler */
        if (irq.type < IRQ_COUNT && ic->handlers[irq.type]) {
            ic->handlers[irq.type](&irq, ic->handler_ctx[irq.type]);
        }

        ic->total_interrupts++;
        ic->by_type[irq.type]++;
    }

    return NULL;
}

int ic_init(interrupt_controller_t *ic) {
    memset(ic, 0, sizeof(*ic));
    ic->running = 1;
    pthread_mutex_init(&ic->lock, NULL);
    pthread_cond_init(&ic->not_empty, NULL);

    if (pthread_create(&ic->dispatch_thread, NULL, dispatch_loop, ic) != 0)
        return -1;

    return 0;
}

void ic_destroy(interrupt_controller_t *ic) {
    pthread_mutex_destroy(&ic->lock);
    pthread_cond_destroy(&ic->not_empty);
}

int ic_register_handler(interrupt_controller_t *ic, irq_type_t type,
                        irq_handler_t handler, void *ctx) {
    if (type >= IRQ_COUNT) return -1;
    ic->handlers[type]    = handler;
    ic->handler_ctx[type] = ctx;
    return 0;
}

int ic_raise(interrupt_controller_t *ic, const interrupt_t *irq) {
    pthread_mutex_lock(&ic->lock);

    if (ic->count >= MAX_PENDING_IRQS) {
        pthread_mutex_unlock(&ic->lock);
        return -1;   /* queue full */
    }

    ic->queue[ic->tail] = *irq;
    ic->tail = (ic->tail + 1) % MAX_PENDING_IRQS;
    ic->count++;

    pthread_cond_signal(&ic->not_empty);
    pthread_mutex_unlock(&ic->lock);
    return 0;
}

void ic_stop(interrupt_controller_t *ic) {
    pthread_mutex_lock(&ic->lock);
    ic->running = 0;
    pthread_cond_broadcast(&ic->not_empty);
    pthread_mutex_unlock(&ic->lock);

    pthread_join(ic->dispatch_thread, NULL);
}

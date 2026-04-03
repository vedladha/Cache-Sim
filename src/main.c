#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <time.h>
#include <getopt.h>
#include "cache.h"
#include "interrupt.h"
#include "io_sim.h"
#include "trace.h"

#define DEFAULT_THREADS 8

typedef struct {
    int                thread_id;
    cache_hierarchy_t *hierarchy;
    mem_request_t     *ops;
    int                op_count;
} worker_arg_t;

static void *worker_thread(void *arg) {
    worker_arg_t *wa = (worker_arg_t *)arg;

    for (int i = 0; i < wa->op_count; i++) {
        uint8_t data = wa->ops[i].value;
        hierarchy_access(wa->hierarchy, wa->ops[i].address, wa->ops[i].op, &data);
    }

    return NULL;
}

static void usage(const char *prog) {
    fprintf(stderr,
        "Usage: %s [options]\n"
        "  -t, --threads N       Number of worker threads (default: %d)\n"
        "  -n, --ops N           Number of memory operations (default: %d)\n"
        "  -p, --policy POLICY   Eviction policy: lru or fifo (default: lru)\n"
        "  -f, --file FILE       Load trace from file instead of generating\n"
        "  -s, --seed N          Random seed for trace generation\n"
        "  -o, --output FILE     Save generated trace to file\n"
        "  -h, --help            Show this help\n",
        prog, DEFAULT_THREADS, DEFAULT_TRACE_SIZE);
}

int main(int argc, char *argv[]) {
    int num_threads       = DEFAULT_THREADS;
    int num_ops           = DEFAULT_TRACE_SIZE;
    eviction_policy_t pol = EVICT_LRU;
    const char *trace_file = NULL;
    const char *output_file = NULL;
    unsigned int seed     = (unsigned int)time(NULL);

    static struct option long_opts[] = {
        {"threads", required_argument, NULL, 't'},
        {"ops",     required_argument, NULL, 'n'},
        {"policy",  required_argument, NULL, 'p'},
        {"file",    required_argument, NULL, 'f'},
        {"seed",    required_argument, NULL, 's'},
        {"output",  required_argument, NULL, 'o'},
        {"help",    no_argument,       NULL, 'h'},
        {NULL, 0, NULL, 0}
    };

    int opt;
    while ((opt = getopt_long(argc, argv, "t:n:p:f:s:o:h", long_opts, NULL)) != -1) {
        switch (opt) {
        case 't': num_threads = atoi(optarg); break;
        case 'n': num_ops     = atoi(optarg); break;
        case 'p':
            if (strcmp(optarg, "fifo") == 0 || strcmp(optarg, "FIFO") == 0)
                pol = EVICT_FIFO;
            else
                pol = EVICT_LRU;
            break;
        case 'f': trace_file  = optarg; break;
        case 's': seed        = (unsigned int)atoi(optarg); break;
        case 'o': output_file = optarg; break;
        case 'h': usage(argv[0]); return 0;
        default:  usage(argv[0]); return 1;
        }
    }

    if (num_threads < 1) num_threads = 1;
    if (num_ops < 1)     num_ops = 1;

    printf("Cache-Sim: %d threads, %d ops, policy=%s, seed=%u\n",
           num_threads, num_ops, pol == EVICT_LRU ? "LRU" : "FIFO", seed);

    /* Generate or load trace */
    trace_t trace;
    if (trace_file) {
        printf("Loading trace from %s...\n", trace_file);
        if (trace_load(&trace, trace_file) < 0) {
            fprintf(stderr, "Error: failed to load trace\n");
            return 1;
        }
        printf("Loaded %d operations\n", trace.count);
    } else {
        printf("Generating %d memory operations...\n", num_ops);
        if (trace_generate(&trace, num_ops, seed) < 0) {
            fprintf(stderr, "Error: failed to generate trace\n");
            return 1;
        }
    }

    if (output_file) {
        trace_save(&trace, output_file);
        printf("Trace saved to %s\n", output_file);
    }

    /* Initialize subsystems */
    interrupt_controller_t ic;
    if (ic_init(&ic) < 0) {
        fprintf(stderr, "Error: failed to init interrupt controller\n");
        return 1;
    }

    io_controller_t io;
    if (io_init(&io, &ic) < 0) {
        fprintf(stderr, "Error: failed to init I/O controller\n");
        return 1;
    }

    cache_hierarchy_t hierarchy;
    if (hierarchy_init(&hierarchy, pol, pol) < 0) {
        fprintf(stderr, "Error: failed to init cache hierarchy\n");
        return 1;
    }
    hierarchy.ic = &ic;
    hierarchy.io = &io;

    /* Divide work among threads */
    pthread_t *threads = malloc(num_threads * sizeof(pthread_t));
    worker_arg_t *args = malloc(num_threads * sizeof(worker_arg_t));

    int ops_per_thread = trace.count / num_threads;
    int remainder      = trace.count % num_threads;
    int offset         = 0;

    struct timespec t_start, t_end;
    clock_gettime(CLOCK_MONOTONIC, &t_start);

    printf("Launching %d worker threads...\n", num_threads);
    for (int i = 0; i < num_threads; i++) {
        args[i].thread_id  = i;
        args[i].hierarchy  = &hierarchy;
        args[i].ops        = &trace.ops[offset];
        args[i].op_count   = ops_per_thread + (i < remainder ? 1 : 0);
        offset += args[i].op_count;

        pthread_create(&threads[i], NULL, worker_thread, &args[i]);
    }

    for (int i = 0; i < num_threads; i++)
        pthread_join(threads[i], NULL);

    clock_gettime(CLOCK_MONOTONIC, &t_end);

    double elapsed = (t_end.tv_sec - t_start.tv_sec) +
                     (t_end.tv_nsec - t_start.tv_nsec) / 1e9;

    /* Shutdown subsystems */
    io_stop(&io);
    ic_stop(&ic);

    /* Report results */
    hierarchy_print_stats(&hierarchy);

    printf("\n  I/O stats: %lu disk reads, avg latency %lu us\n",
           (unsigned long)io.total_reads,
           io.total_reads ? (unsigned long)(io.total_latency_us / io.total_reads) : 0UL);
    printf("  Interrupts handled: %lu (IO: %lu, resize: %lu)\n",
           (unsigned long)ic.total_interrupts,
           (unsigned long)ic.by_type[IRQ_IO_COMPLETE],
           (unsigned long)ic.by_type[IRQ_CACHE_RESIZE]);
    printf("\n  Wall time: %.3f s  (%.0f ops/sec)\n",
           elapsed, hierarchy.total_ops / elapsed);

    /* Cleanup */
    hierarchy_destroy(&hierarchy);
    io_destroy(&io);
    ic_destroy(&ic);
    trace_destroy(&trace);
    free(threads);
    free(args);

    return 0;
}

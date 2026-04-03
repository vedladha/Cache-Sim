# Cache-Sim

Interrupt-driven L1/L2 cache simulator in C with multi-threaded memory access, dynamic cache resizing, and configurable eviction policies.

## Architecture

```
  Worker Threads (N)
  Thread 0   Thread 1   ...   Thread N-1
       |          |               |
       v          v               v
  Cache Hierarchy (mutex-protected)
    L1 Cache (64 sets, 4-way)
    L2 Cache (256 sets, 8-way)
       |          |
       v (miss)   v (miss)
  I/O Controller (async)
    request queue -> I/O thread -> simulated disk read
       |
       v (raises IRQ)
  Interrupt Controller
    IRQ queue -> dispatch thread -> completion handler
```

## Features

- **L1/L2 cache hierarchy** with set-associative organization
- **LRU and FIFO eviction policies** selectable at runtime
- **Dynamic cache resizing** that doubles cache sets when miss rate exceeds threshold, rehashing existing lines
- **Interrupt-driven I/O** where L2 misses trigger asynchronous disk reads via an interrupt controller with dispatch queue
- **Multi-threaded** with support for 8+ concurrent worker threads with per-level mutex synchronization
- **Synthetic trace generator** that produces 100K+ operations with realistic access patterns (temporal/spatial locality, random, strided)
- **Trace file I/O** for loading/saving memory traces for reproducible experiments

## Build

```bash
make
```

## Usage

```bash
# Default: 8 threads, 100K ops, LRU policy
./cache-sim

# Custom configuration
./cache-sim --threads 16 --ops 500000 --policy fifo --seed 42

# Compare LRU vs FIFO with same trace
make run-comparison

# Save/load traces
./cache-sim --output trace.txt --seed 42
./cache-sim --file trace.txt --threads 4
```

### Options

| Flag | Description | Default |
|------|-------------|---------|
| `-t, --threads N` | Worker thread count | 8 |
| `-n, --ops N` | Memory operations to process | 100000 |
| `-p, --policy P` | Eviction policy (`lru` or `fifo`) | `lru` |
| `-f, --file FILE` | Load trace from file | - |
| `-s, --seed N` | RNG seed for reproducibility | time-based |
| `-o, --output FILE` | Save generated trace | - |

## How It Works

### Cache Access Flow

1. Worker thread issues a memory request (read/write)
2. L1 lookup: on hit, update LRU timestamp and return
3. L2 lookup: on hit, promote line to L1 and return
4. L2 miss: submit async I/O request to the I/O controller
5. I/O thread simulates disk latency (50-200 us), fills cache line
6. I/O thread raises `IRQ_IO_COMPLETE` on the interrupt controller
7. Interrupt dispatch thread calls the completion handler
8. Handler signals the waiting worker thread via condition variable
9. Data is installed in both L2 and L1

### Dynamic Resizing

Every 2000 accesses, each cache level checks its windowed miss rate. If it exceeds 30%, the cache doubles its set count (up to 4x the initial size) and rehashes all valid lines into the new geometry.

### Concurrency Model

- Each cache level has its own `pthread_mutex_t` for fine-grained locking, allowing L1 and L2 to be accessed concurrently by different threads
- The I/O request queue and interrupt queue use producer-consumer patterns with condition variables
- Worker threads block on per-request condition variables during disk I/O, freeing cache locks for other threads

## Project Structure

```
Cache-Sim/
├── Makefile
├── include/
│   ├── cache.h         # Cache structures, hierarchy API
│   ├── interrupt.h     # Interrupt controller
│   ├── io_sim.h        # Async disk I/O simulator
│   └── trace.h         # Trace generation and parsing
├── src/
│   ├── main.c          # CLI driver, thread management
│   ├── cache.c         # Cache logic, eviction, resizing
│   ├── interrupt.c     # IRQ dispatch queue
│   ├── io_sim.c        # Disk I/O simulation, completion
│   └── trace.c         # Synthetic trace generator
└── .gitignore
```
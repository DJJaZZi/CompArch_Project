/*
 * bench.c — Benchmarks: uthreads vs pthreads.
 *
 * Tests
 * ──────
 * 1. Thread creation + join latency   (N threads, each exits immediately)
 * 2. Mutex contention throughput      (N threads fighting one lock, M rounds)
 * 3. Context-switch rate              (2 threads ping-ponging via yield)
 * 4. Semaphore post/wait throughput   (producer/consumer pairs)
 *
 * Build: see Makefile (compiled twice: once with -DUSE_PTHREAD)
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef USE_PTHREAD
#  include <pthread.h>
#  define THREAD_T           pthread_t
#  define MUTEX_T            pthread_mutex_t
#  define SEM_T              sem_t
#  include <semaphore.h>
#  define thread_create(tid, fn, arg) pthread_create(tid, NULL, fn, arg)
#  define thread_join(tid)            pthread_join(tid, NULL)
#  define mutex_init(m)               pthread_mutex_init(m, NULL)
#  define mutex_lock(m)               pthread_mutex_lock(m)
#  define mutex_unlock(m)             pthread_mutex_unlock(m)
#  define mutex_destroy(m)            pthread_mutex_destroy(m)
#  define sem_init_fn(s,v)            sem_init(s, 0, v)
#  define sem_wait_fn(s)              sem_wait(s)
#  define sem_post_fn(s)              sem_post(s)
#  define sem_destroy_fn(s)           sem_destroy(s)
#  define thread_yield()              sched_yield()
#  define LIB_NAME "pthread"
#else
#  include "uthreads.h"
#  define THREAD_T           uth_tid_t
#  define MUTEX_T            uth_mutex_t
#  define SEM_T              uth_sem_t
#  define thread_create(tid, fn, arg) (*(tid) = uth_create(fn, arg, 0))
#  define thread_join(tid)            uth_join(tid, NULL)
#  define mutex_init(m)               uth_mutex_init(m)
#  define mutex_lock(m)               uth_mutex_lock(m)
#  define mutex_unlock(m)             uth_mutex_unlock(m)
#  define mutex_destroy(m)            uth_mutex_destroy(m)
#  define sem_init_fn(s,v)            uth_sem_init(s, v)
#  define sem_wait_fn(s)              uth_sem_wait(s)
#  define sem_post_fn(s)              uth_sem_post(s)
#  define sem_destroy_fn(s)           uth_sem_destroy(s)
#  define thread_yield()              uth_yield()
#  define LIB_NAME "uthreads"
#endif

/* ------------------------------------------------------------------ */
/* Timing helpers                                                      */
/* ------------------------------------------------------------------ */

static double now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1e9 + ts.tv_nsec;
}

/* ------------------------------------------------------------------ */
/* Benchmark 1: thread creation + join latency                        */
/* ------------------------------------------------------------------ */

static void *noop_thread(void *arg) { (void)arg; return NULL; }

static void bench_create(int n)
{
    THREAD_T *tids = malloc(n * sizeof *tids);
    double t0 = now_ns();

    for (int i = 0; i < n; i++)
        thread_create(&tids[i], noop_thread, NULL);

    for (int i = 0; i < n; i++)
        thread_join(tids[i]);

    double dt = now_ns() - t0;
    printf("[%s] create+join %d threads: %.0f ns total, %.1f ns/thread\n",
           LIB_NAME, n, dt, dt / n);
    free(tids);
}

/* ------------------------------------------------------------------ */
/* Benchmark 2: mutex contention throughput                           */
/* ------------------------------------------------------------------ */

#define CONTENTION_THREADS 4
#define CONTENTION_ROUNDS  100000

static MUTEX_T contend_mutex;
static volatile long contend_counter = 0;

static void *contend_thread(void *arg)
{
    int rounds = *(int *)arg;
    for (int i = 0; i < rounds; i++) {
        mutex_lock(&contend_mutex);
        contend_counter++;
        mutex_unlock(&contend_mutex);
    }
    return NULL;
}

static void bench_mutex(void)
{
    THREAD_T tids[CONTENTION_THREADS];
    int rounds = CONTENTION_ROUNDS;
    mutex_init(&contend_mutex);
    contend_counter = 0;

    double t0 = now_ns();
    for (int i = 0; i < CONTENTION_THREADS; i++)
        thread_create(&tids[i], contend_thread, &rounds);

    for (int i = 0; i < CONTENTION_THREADS; i++)
        thread_join(tids[i]);

    double dt = now_ns() - t0;
    long total_ops = (long)CONTENTION_THREADS * CONTENTION_ROUNDS;
    printf("[%s] mutex contention: %ld ops in %.0f ms → %.0f Kops/s  "
           "(counter=%ld, expected=%ld)\n",
           LIB_NAME, total_ops, dt / 1e6,
           total_ops / (dt / 1e9) / 1000.0,
           contend_counter, total_ops);
    mutex_destroy(&contend_mutex);
}

/* ------------------------------------------------------------------ */
/* Benchmark 3: context-switch rate (ping-pong yield)                 */
/* ------------------------------------------------------------------ */

#define PING_ROUNDS 50000

static SEM_T ping_sem, pong_sem;

static void *ping_thread(void *arg)
{
    int rounds = *(int *)arg;
    for (int i = 0; i < rounds; i++) {
        sem_wait_fn(&ping_sem);
        sem_post_fn(&pong_sem);
    }
    return NULL;
}

static void *pong_thread(void *arg)
{
    int rounds = *(int *)arg;
    for (int i = 0; i < rounds; i++) {
        sem_wait_fn(&pong_sem);
        sem_post_fn(&ping_sem);
    }
    return NULL;
}

static void bench_ctxswitch(void)
{
    THREAD_T t1, t2;
    int rounds = PING_ROUNDS;
    sem_init_fn(&ping_sem, 1);
    sem_init_fn(&pong_sem, 0);

    double t0 = now_ns();
    thread_create(&t1, ping_thread, &rounds);
    thread_create(&t2, pong_thread, &rounds);

    thread_join(t1);
    thread_join(t2);

    double dt = now_ns() - t0;
    long switches = (long)PING_ROUNDS * 2;
    printf("[%s] context switches: %ld switches in %.0f ms "
           "→ %.1f ns/switch\n",
           LIB_NAME, switches, dt / 1e6, dt / switches);

    sem_destroy_fn(&ping_sem);
    sem_destroy_fn(&pong_sem);
}

/* ------------------------------------------------------------------ */
/* main                                                                */
/* ------------------------------------------------------------------ */

#ifndef USE_PTHREAD
/* Wrapper: run all benchmarks from inside a uthreads thread. */
static void *bench_main_thread(void *arg)
{
    (void)arg;
    printf("-- Thread creation latency --\n");
    bench_create(100);
    bench_create(500);

    printf("\n-- Mutex contention --\n");
    bench_mutex();

    printf("\n-- Context-switch rate (semaphore ping-pong) --\n");
    bench_ctxswitch();

    printf("\nDone.\n");
    return NULL;
}
#endif

int main(void)
{
    printf("=== Benchmark suite: %s ===\n\n", LIB_NAME);

#ifdef USE_PTHREAD
    printf("-- Thread creation latency --\n");
    bench_create(100);
    bench_create(500);

    printf("\n-- Mutex contention --\n");
    bench_mutex();

    printf("\n-- Context-switch rate (semaphore ping-pong) --\n");
    bench_ctxswitch();

    printf("\nDone.\n");
#else
    uth_init();
    uth_tid_t main_tid = uth_create(bench_main_thread, NULL, 0);
    uth_run();
    uth_join(main_tid, NULL);
#endif
    return 0;
}

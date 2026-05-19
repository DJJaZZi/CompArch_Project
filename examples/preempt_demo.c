/*
 * preempt_demo.c — Demonstrates SIGALRM-based preemption.
 *
 * The point: NEITHER thread calls uth_yield().  Without preemption, the
 * first thread would run forever (CPU-bound loop) and the second would
 * never execute.  With preemption enabled (10 ms timer), SIGALRM fires
 * periodically and forces a context switch — both threads make progress.
 *
 * Expected output: alternating "A: ..." and "B: ..." messages.
 *
 * Run: ./preempt_demo
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdatomic.h>
#include "uthreads.h"

#define WORK_ITERS 5

static atomic_int progress_a = 0;
static atomic_int progress_b = 0;

/*
 * CPU-bound work that does NOT call uth_yield.  The compiler must NOT
 * optimize this away — we use volatile and an inline-asm barrier.
 * Each call takes ~hundreds of milliseconds on a modern CPU,
 * spanning many 10ms preemption slices.
 */
static void cpu_burn(int millions)
{
    volatile long x = 0;
    for (long i = 0; i < millions * 1000000L; i++) {
        x += i * 3 + 1;
        __asm__ __volatile__("" : "+r"(x) : : "memory");
    }
    (void)x;
}

static void *thread_a(void *arg)
{
    (void)arg;
    for (int i = 0; i < WORK_ITERS; i++) {
        cpu_burn(50);
        int p = atomic_fetch_add(&progress_a, 1) + 1;
        printf("A: progress = %d  (B has progress = %d)\n",
               p, atomic_load(&progress_b));
    }
    return NULL;
}

static void *thread_b(void *arg)
{
    (void)arg;
    for (int i = 0; i < WORK_ITERS; i++) {
        cpu_burn(50);
        int p = atomic_fetch_add(&progress_b, 1) + 1;
        printf("B: progress = %d  (A has progress = %d)\n",
               p, atomic_load(&progress_a));
    }
    return NULL;
}

static void *driver(void *arg)
{
    (void)arg;
    uth_tid_t a = uth_create(thread_a, NULL, 0);
    uth_tid_t b = uth_create(thread_b, NULL, 0);
    uth_join(a, NULL);
    uth_join(b, NULL);
    return NULL;
}

int main(void)
{
    printf("Starting preemption demo (10 ms timer slice)\n");
    printf("Neither thread calls uth_yield(); without SIGALRM\n");
    printf("preemption, only one thread would ever run.\n\n");

    uth_init();
    uth_preempt_enable(10);   /* preempt every 10 ms */

    uth_create(driver, NULL, 0);
    uth_run();

    uth_preempt_disable();

    printf("\nFinal progress: A = %d, B = %d  (expected %d each)\n",
           atomic_load(&progress_a), atomic_load(&progress_b), WORK_ITERS);

    int ok = (atomic_load(&progress_a) == WORK_ITERS) &&
             (atomic_load(&progress_b) == WORK_ITERS);
    printf("%s\n", ok ? "PASS — preemption works" : "FAIL");
    return ok ? 0 : 1;
}

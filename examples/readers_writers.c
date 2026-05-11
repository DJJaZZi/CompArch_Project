/*
 * readers_writers.c — Readers-Writers problem with writer-preference.
 *
 * Multiple readers may read concurrently; writers need exclusive access.
 * To prevent writer starvation: if a writer is waiting, new readers wait too.
 *
 * Run: ./readers_writers [N_READERS] [N_WRITERS] [ITERATIONS]
 */

#include <stdio.h>
#include <stdlib.h>
#include "uthreads.h"

#define DEFAULT_READERS    4
#define DEFAULT_WRITERS    2
#define DEFAULT_ITERATIONS 3

static uth_mutex_t lock      = UTH_MUTEX_INITIALIZER;
static uth_cond_t  can_read  = UTH_COND_INITIALIZER;
static uth_cond_t  can_write = UTH_COND_INITIALIZER;

static int active_readers  = 0;
static int active_writers  = 0;
static int waiting_writers = 0;
static int shared_data     = 0;

static int g_nr, g_nw, g_iters;

/* ------------------------------------------------------------------ */
/* RW lock helpers                                                     */
/* ------------------------------------------------------------------ */

static void read_lock(void)
{
    uth_mutex_lock(&lock);
    while (active_writers > 0 || waiting_writers > 0)
        uth_cond_wait(&can_read, &lock);
    active_readers++;
    uth_mutex_unlock(&lock);
}

static void read_unlock(void)
{
    uth_mutex_lock(&lock);
    active_readers--;
    if (active_readers == 0)
        uth_cond_signal(&can_write);
    uth_mutex_unlock(&lock);
}

static void write_lock(void)
{
    uth_mutex_lock(&lock);
    waiting_writers++;
    while (active_readers > 0 || active_writers > 0)
        uth_cond_wait(&can_write, &lock);
    waiting_writers--;
    active_writers++;
    uth_mutex_unlock(&lock);
}

static void write_unlock(void)
{
    uth_mutex_lock(&lock);
    active_writers--;
    if (waiting_writers > 0)
        uth_cond_signal(&can_write);
    else
        uth_cond_broadcast(&can_read);
    uth_mutex_unlock(&lock);
}

/* ------------------------------------------------------------------ */
/* Thread bodies                                                       */
/* ------------------------------------------------------------------ */

typedef struct { int id; int iters; } thread_arg_t;

static void *reader(void *arg)
{
    thread_arg_t *a = arg;
    for (int i = 0; i < a->iters; i++) {
        read_lock();
        printf("Reader %d reads: shared_data = %d\n", a->id, shared_data);
        uth_yield();
        read_unlock();
        uth_yield();
    }
    printf("Reader %d done\n", a->id);
    return NULL;
}

static void *writer(void *arg)
{
    thread_arg_t *a = arg;
    for (int i = 0; i < a->iters; i++) {
        write_lock();
        shared_data++;
        printf("Writer %d writes: shared_data = %d\n", a->id, shared_data);
        uth_yield();
        write_unlock();
        uth_yield();
    }
    printf("Writer %d done\n", a->id);
    return NULL;
}

/* ------------------------------------------------------------------ */
/* Driver                                                              */
/* ------------------------------------------------------------------ */

static void *driver(void *arg)
{
    (void)arg;
    thread_arg_t *rargs = malloc(g_nr * sizeof *rargs);
    thread_arg_t *wargs = malloc(g_nw * sizeof *wargs);
    uth_tid_t    *rtids = malloc(g_nr * sizeof *rtids);
    uth_tid_t    *wtids = malloc(g_nw * sizeof *wtids);

    for (int i = 0; i < g_nw; i++) {
        wargs[i] = (thread_arg_t){ .id = i, .iters = g_iters };
        wtids[i] = uth_create(writer, &wargs[i], 0);
    }
    for (int i = 0; i < g_nr; i++) {
        rargs[i] = (thread_arg_t){ .id = i, .iters = g_iters };
        rtids[i] = uth_create(reader, &rargs[i], 0);
    }

    for (int i = 0; i < g_nw; i++) uth_join(wtids[i], NULL);
    for (int i = 0; i < g_nr; i++) uth_join(rtids[i], NULL);

    free(rargs); free(wargs); free(rtids); free(wtids);
    return NULL;
}

int main(int argc, char *argv[])
{
    g_nr    = (argc > 1) ? atoi(argv[1]) : DEFAULT_READERS;
    g_nw    = (argc > 2) ? atoi(argv[2]) : DEFAULT_WRITERS;
    g_iters = (argc > 3) ? atoi(argv[3]) : DEFAULT_ITERATIONS;

    uth_init();
    uth_create(driver, NULL, 0);
    uth_run();

    printf("\nFinal shared_data = %d  (expected %d)\n",
           shared_data, g_nw * g_iters);
    return (shared_data == g_nw * g_iters) ? 0 : 1;
}

/*
 * producer_consumer.c — Bounded-buffer producer/consumer.
 *
 * Three semaphores:
 *   empty  — counts free slots  (init = BUFFER_SIZE)
 *   full   — counts filled slots (init = 0)
 *   mutex  — exclusion on the buffer
 *
 * Run: ./producer_consumer [N_PRODUCERS] [N_CONSUMERS] [ITEMS_PER_PRODUCER]
 */

#include <stdio.h>
#include <stdlib.h>
#include "uthreads.h"

#define BUFFER_SIZE       8
#define DEFAULT_PRODUCERS 2
#define DEFAULT_CONSUMERS 3
#define DEFAULT_ITEMS     6

static int  buf[BUFFER_SIZE];
static int  buf_in  = 0;
static int  buf_out = 0;

static uth_sem_t   empty;
static uth_sem_t   full;
static uth_mutex_t buf_lock = UTH_MUTEX_INITIALIZER;

static int items_produced = 0;
static int items_consumed = 0;

static int g_np, g_nc, g_ni;

typedef struct { int id; int items; } thread_arg_t;

static void *producer(void *arg)
{
    thread_arg_t *a = arg;
    for (int i = 0; i < a->items; i++) {
        int item = a->id * 1000 + i;

        uth_sem_wait(&empty);
        uth_mutex_lock(&buf_lock);
        buf[buf_in] = item;
        buf_in = (buf_in + 1) % BUFFER_SIZE;
        items_produced++;
        printf("Producer %d produced item %d  (buf contains %d items)\n",
               a->id, item, items_produced - items_consumed);
        uth_mutex_unlock(&buf_lock);
        uth_sem_post(&full);

        uth_yield();
    }
    printf("Producer %d done\n", a->id);
    return NULL;
}

static void *consumer(void *arg)
{
    thread_arg_t *a = arg;
    for (int i = 0; i < a->items; i++) {
        uth_sem_wait(&full);
        uth_mutex_lock(&buf_lock);
        int item = buf[buf_out];
        buf_out = (buf_out + 1) % BUFFER_SIZE;
        items_consumed++;
        printf("Consumer %d consumed item %d  (buf contains %d items)\n",
               a->id, item, items_produced - items_consumed);
        uth_mutex_unlock(&buf_lock);
        uth_sem_post(&empty);

        uth_yield();
    }
    printf("Consumer %d done\n", a->id);
    return NULL;
}

static void *driver(void *arg)
{
    (void)arg;
    int total = g_np * g_ni;
    int per_consumer = total / g_nc;
    int extra        = total % g_nc;

    thread_arg_t *pargs = malloc(g_np * sizeof *pargs);
    thread_arg_t *cargs = malloc(g_nc * sizeof *cargs);
    uth_tid_t    *ptids = malloc(g_np * sizeof *ptids);
    uth_tid_t    *ctids = malloc(g_nc * sizeof *ctids);

    for (int i = 0; i < g_np; i++) {
        pargs[i] = (thread_arg_t){ .id = i, .items = g_ni };
        ptids[i] = uth_create(producer, &pargs[i], 0);
    }
    for (int i = 0; i < g_nc; i++) {
        cargs[i] = (thread_arg_t){
            .id    = i,
            .items = per_consumer + (i < extra ? 1 : 0)
        };
        ctids[i] = uth_create(consumer, &cargs[i], 0);
    }

    for (int i = 0; i < g_np; i++) uth_join(ptids[i], NULL);
    for (int i = 0; i < g_nc; i++) uth_join(ctids[i], NULL);

    free(pargs); free(cargs); free(ptids); free(ctids);
    return NULL;
}

int main(int argc, char *argv[])
{
    g_np = (argc > 1) ? atoi(argv[1]) : DEFAULT_PRODUCERS;
    g_nc = (argc > 2) ? atoi(argv[2]) : DEFAULT_CONSUMERS;
    g_ni = (argc > 3) ? atoi(argv[3]) : DEFAULT_ITEMS;

    uth_sem_init(&empty, BUFFER_SIZE);
    uth_sem_init(&full,  0);
    uth_mutex_init(&buf_lock);

    uth_init();
    uth_create(driver, NULL, 0);
    uth_run();

    printf("\nProduced: %d  Consumed: %d\n", items_produced, items_consumed);

    uth_sem_destroy(&empty);
    uth_sem_destroy(&full);
    uth_mutex_destroy(&buf_lock);
    return (items_produced == items_consumed) ? 0 : 1;
}
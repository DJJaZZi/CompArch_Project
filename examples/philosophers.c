/*
 * philosophers.c — Dining Philosophers using uth_mutex.
 *
 * Classic Dijkstra problem: N philosophers sit at a round table.
 * Each needs TWO forks (left and right) to eat.  Naive lock ordering
 * causes deadlock; we break it by having the LAST philosopher pick up
 * forks in reverse order (resource hierarchy solution).
 *
 * Run: ./philosophers [N_PHILOSOPHERS] [MEALS_EACH]
 */

#include <stdio.h>
#include <stdlib.h>
#include "uthreads.h"

#define DEFAULT_N      5
#define DEFAULT_MEALS  3

static int          N;
static int          total_meals;
static uth_mutex_t *forks;    /* forks[i] = fork between i and (i+1)%N */
static int         *meals;    /* meals[i] = number of meals eaten      */

typedef struct {
    int id;
} philo_arg_t;

static void think(int id)
{
    printf("Philosopher %d is thinking\n", id);
    for (int i = 0; i < 3; i++)
        uth_yield();
}

static void eat(int id)
{
    printf("Philosopher %d is eating  (meal %d)\n", id, meals[id] + 1);
    for (int i = 0; i < 2; i++)
        uth_yield();
}

static void *philosopher(void *arg)
{
    philo_arg_t *a     = (philo_arg_t *)arg;
    int          id    = a->id;
    int          left  = id;
    int          right = (id + 1) % N;

    /*
     * Deadlock prevention: the last philosopher picks up RIGHT fork first.
     * This breaks the circular wait condition.
     */
    int first  = (id == N - 1) ? right : left;
    int second = (id == N - 1) ? left  : right;

    while (meals[id] < total_meals) {
        think(id);

        uth_mutex_lock(&forks[first]);
        printf("Philosopher %d picked up fork %d\n", id, first);
        uth_yield();   /* expose the race window */

        uth_mutex_lock(&forks[second]);
        printf("Philosopher %d picked up fork %d\n", id, second);

        eat(id);
        meals[id]++;

        uth_mutex_unlock(&forks[second]);
        uth_mutex_unlock(&forks[first]);
        printf("Philosopher %d put down forks\n", id);
    }

    printf("Philosopher %d is done\n", id);
    return NULL;
}

/*
 * Driver thread spawned by main(): all uth_* calls (including create
 * and join) must happen INSIDE the scheduler, so we run the setup work
 * from inside a thread.
 */
static void *driver(void *arg)
{
    (void)arg;
    philo_arg_t *args = malloc(N * sizeof *args);
    uth_tid_t   *tids = malloc(N * sizeof *tids);

    for (int i = 0; i < N; i++) {
        args[i].id = i;
        tids[i] = uth_create(philosopher, &args[i], 0);
    }
    for (int i = 0; i < N; i++)
        uth_join(tids[i], NULL);

    free(args);
    free(tids);
    return NULL;
}

int main(int argc, char *argv[])
{
    N = (argc > 1) ? atoi(argv[1]) : DEFAULT_N;
    total_meals = (argc > 2) ? atoi(argv[2]) : DEFAULT_MEALS;

    if (N < 2) { fprintf(stderr, "Need at least 2 philosophers\n"); return 1; }

    forks = malloc(N * sizeof *forks);
    meals = calloc(N, sizeof *meals);
    for (int i = 0; i < N; i++)
        uth_mutex_init(&forks[i]);

    uth_init();
    uth_create(driver, NULL, 0);
    uth_run();

    printf("\nAll philosophers finished.\n");
    int total_eaten = 0;
    for (int i = 0; i < N; i++) {
        printf("  Philosopher %d ate %d meals\n", i, meals[i]);
        total_eaten += meals[i];
    }
    printf("Total meals eaten: %d  (expected %d)\n",
           total_eaten, N * total_meals);

    for (int i = 0; i < N; i++)
        uth_mutex_destroy(&forks[i]);
    free(forks);
    free(meals);
    return 0;
}

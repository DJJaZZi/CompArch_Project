/*
 * sync.c — Condition variable and semaphore.
 *
 * Condition variable
 * ───────────────────
 * Classic Mesa-style (signal does NOT immediately transfer CPU).
 * uth_cond_wait:
 *   1. Atomically release the mutex and block (within preemption-off).
 *   2. On wakeup, re-acquire the mutex before returning.
 * uth_cond_signal:    wake one waiter.
 * uth_cond_broadcast: wake all waiters.
 *
 * Semaphore
 * ──────────
 * Counting semaphore backed by a wait queue.
 * uth_sem_wait (P): if value > 0, decrement and return; else block.
 * uth_sem_post (V): increment; if there are waiters, wake one.
 */

#include <assert.h>
#include <signal.h>
#include <stdlib.h>

#include "uthreads.h"
#include "uthreads_internal.h"

/* ================================================================== */
/* Condition variable                                                  */
/* ================================================================== */

void uth_cond_init(uth_cond_t *c)
{
    c->waiters = NULL;
}

/*
 * Atomically release *m and block on *c.
 * On return the calling thread holds *m again.
 *
 * Race avoidance: We disable SIGALRM (preempt_off) for the entire
 * sequence release-mutex → enqueue-on-cond.  In a single-OS-thread
 * cooperative system this is sufficient — no other uthread can run
 * between unlock and block, so a concurrent signal cannot be lost.
 */
void uth_cond_wait(uth_cond_t *c, uth_mutex_t *m)
{
    sigset_t old;
    uth_preempt_off(&old);

    uth_mutex_unlock(m);          /* releases the mutex                */
    uth_block(&c->waiters);       /* blocks until signalled            */

    /*
     * On resume swapcontext has restored our signal mask; SIGALRM is
     * still blocked.  Re-acquire the mutex before returning.
     */
    uth_mutex_lock(m);

    uth_preempt_restore(&old);
}

void uth_cond_signal(uth_cond_t *c)
{
    sigset_t old;
    uth_preempt_off(&old);
    uth_wake_one(&c->waiters);
    uth_preempt_restore(&old);
}

void uth_cond_broadcast(uth_cond_t *c)
{
    sigset_t old;
    uth_preempt_off(&old);
    uth_wake_all(&c->waiters);
    uth_preempt_restore(&old);
}

void uth_cond_destroy(uth_cond_t *c)
{
    assert(c->waiters == NULL && "cond_destroy with waiters");
    (void)c;
}

/* ================================================================== */
/* Semaphore                                                           */
/* ================================================================== */

void uth_sem_init(uth_sem_t *s, int value)
{
    assert(value >= 0 && "semaphore value must be non-negative");
    s->value   = value;
    s->waiters = NULL;
}

/* P / down / acquire */
void uth_sem_wait(uth_sem_t *s)
{
    sigset_t old;
    uth_preempt_off(&old);

    while (s->value <= 0) {
        /* No permits available — block.  On resume, mask is preserved. */
        uth_block(&s->waiters);
    }
    s->value--;

    uth_preempt_restore(&old);
}

/* V / up / release */
void uth_sem_post(uth_sem_t *s)
{
    sigset_t old;
    uth_preempt_off(&old);

    s->value++;
    /* Wake one waiter if any (they'll recheck value in their loop). */
    uth_wake_one(&s->waiters);

    uth_preempt_restore(&old);
}

void uth_sem_destroy(uth_sem_t *s)
{
    assert(s->waiters == NULL && "sem_destroy with waiters");
    (void)s;
}

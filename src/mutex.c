/*
 * mutex.c — Mutex, spinlock, and ticket lock.
 *
 * Mutex
 * ──────
 * Uses an atomic flag for the locked bit.  A contending thread does NOT
 * spin — it immediately adds itself to the mutex's wait queue and blocks
 * (cooperative sleep).  This avoids wasting CPU in the common case where
 * the critical section is non-trivial.
 *
 * Spinlock
 * ─────────
 * A plain TAS (test-and-set) spinlock using atomic_flag.  Only correct
 * for very short critical sections where sleeping overhead would dominate.
 * Because uthreads is cooperative, a spinlock that can't acquire will
 * call uth_yield() on every failed attempt so other threads can run.
 *
 * Ticket lock
 * ────────────
 * FIFO-fair: threads are served strictly in arrival order.
 * Uses two atomic counters (next_ticket, now_serving).  If a thread's
 * ticket is not yet being served it blocks (sleeps) rather than spinning.
 * The unlock path wakes only the thread holding the next ticket.
 */

#include <stdlib.h>
#include <stdio.h>
#include <assert.h>
#include <signal.h>

#include "uthreads.h"
#include "uthreads_internal.h"

/* ================================================================== */
/* Mutex                                                               */
/* ================================================================== */

void uth_mutex_init(uth_mutex_t *m)
{
    atomic_store_explicit(&m->locked, 0, memory_order_relaxed);
    m->owner   = UTH_TID_INVALID;
    m->waiters = NULL;
}

void uth_mutex_lock(uth_mutex_t *m)
{
    sigset_t old;
    uth_preempt_off(&old);

    while (atomic_exchange_explicit(&m->locked, 1, memory_order_acquire)) {
        /*
         * Lock is held by someone else.  Block on the mutex's wait queue.
         * uth_block appends uth_current and swaps to the scheduler.
         * When we're resumed, swapcontext has restored our signal mask
         * (which had SIGALRM blocked when we yielded), so preemption is
         * still off.  Loop and retry the exchange.
         */
        uth_block(&m->waiters);
    }

    m->owner = uth_self();
    uth_preempt_restore(&old);
}

void uth_mutex_unlock(uth_mutex_t *m)
{
    sigset_t old;
    uth_preempt_off(&old);

    assert(m->owner == uth_self() && "unlock by non-owner");

    m->owner = UTH_TID_INVALID;
    atomic_store_explicit(&m->locked, 0, memory_order_release);

    /* Wake exactly one waiter (if any). */
    uth_wake_one(&m->waiters);

    uth_preempt_restore(&old);
}

void uth_mutex_destroy(uth_mutex_t *m)
{
    assert(m->waiters == NULL && "destroy with waiters");
    (void)m;
}

/* ================================================================== */
/* Spinlock                                                            */
/* ================================================================== */

void uth_spin_init(uth_spinlock_t *s)
{
    atomic_flag_clear_explicit(&s->flag, memory_order_relaxed);
}

void uth_spin_lock(uth_spinlock_t *s)
{
    /*
     * Busy-wait loop.  We call uth_yield() on each failed attempt so
     * cooperative threads get a chance to run and eventually release
     * the lock (pure spinning would livelock in a cooperative system).
     */
    while (atomic_flag_test_and_set_explicit(&s->flag, memory_order_acquire))
        uth_yield();
}

void uth_spin_unlock(uth_spinlock_t *s)
{
    atomic_flag_clear_explicit(&s->flag, memory_order_release);
}

void uth_spin_destroy(uth_spinlock_t *s)
{
    (void)s;
}

/* ================================================================== */
/* Ticket lock                                                         */
/* ================================================================== */

/*
 * Per-ticket waiter nodes.  We maintain a linked list sorted by ticket
 * number so uth_ticket_unlock can wake exactly the right thread.
 */
typedef struct ticket_waiter {
    uth_tcb_t          *tcb;
    unsigned            ticket;
    struct ticket_waiter *next;
} ticket_waiter_t;

void uth_ticket_init(uth_ticket_t *t)
{
    atomic_store_explicit(&t->next_ticket,  0, memory_order_relaxed);
    atomic_store_explicit(&t->now_serving,  0, memory_order_relaxed);
    t->waiters = NULL;
}

void uth_ticket_lock(uth_ticket_t *t)
{
    sigset_t old;
    uth_preempt_off(&old);

    unsigned my_ticket = atomic_fetch_add_explicit(
        &t->next_ticket, 1, memory_order_relaxed);

    /*
     * Loop: if it's not yet our turn, store our ticket in the TCB and
     * block.  unlock() will look up the matching ticket in the wait
     * queue and wake exactly that thread.
     *
     * Race avoidance: we are inside preempt_off, so once we observe
     * now_serving != my_ticket and decide to block, no other thread
     * can run on this CPU between the check and uth_block.  The unlock
     * path is also inside preempt_off, so it cannot race with us.
     */
    while (atomic_load_explicit(&t->now_serving, memory_order_acquire)
           != my_ticket)
    {
        uth_current->ticket = my_ticket;
        uth_block(&t->waiters);
        /* On wakeup, recheck (could be spurious wake or wrong order). */
    }

    uth_preempt_restore(&old);
}

void uth_ticket_unlock(uth_ticket_t *t)
{
    sigset_t old;
    uth_preempt_off(&old);

    unsigned next = atomic_fetch_add_explicit(
        &t->now_serving, 1, memory_order_release) + 1;

    /*
     * Walk the waiters queue looking for the thread with ticket == next.
     * Because tickets are assigned in order and threads block in order,
     * this is usually O(1) but is correct even if threads are reordered.
     */
    uth_tcb_t **pp = &t->waiters;
    while (*pp) {
        if ((*pp)->ticket == next) {
            uth_tcb_t *w = *pp;
            *pp = w->next;
            w->next = NULL;
            uth_enqueue(w);
            break;
        }
        pp = &(*pp)->next;
    }

    uth_preempt_restore(&old);
}

void uth_ticket_destroy(uth_ticket_t *t)
{
    assert(t->waiters == NULL && "destroy with waiters");
    (void)t;
}

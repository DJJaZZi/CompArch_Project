#ifndef UTHREADS_INTERNAL_H
#define UTHREADS_INTERNAL_H

/*
 * uthreads_internal.h — Private structures shared between translation units.
 * NOT part of the public API.
 */

#include <ucontext.h>
#include <stdatomic.h>
#include "uthreads.h"

/* ------------------------------------------------------------------ */
/* Thread state machine                                                */
/*                                                                     */
/*   READY ──► RUNNING ──► BLOCKED ──► READY                          */
/*                  └──────────────────► ZOMBIE ──► (freed)           */
/* ------------------------------------------------------------------ */

typedef enum {
    UTH_STATE_READY   = 0,
    UTH_STATE_RUNNING = 1,
    UTH_STATE_BLOCKED = 2,
    UTH_STATE_ZOMBIE  = 3,
} uth_state_t;

/* ------------------------------------------------------------------ */
/* Thread Control Block (TCB)                                         */
/* ------------------------------------------------------------------ */

typedef struct uth_tcb {
    uth_tid_t      tid;
    uth_state_t    state;
    ucontext_t     ctx;            /* saved register + stack context  */
    void          *stack;          /* bottom of allocated stack       */
    size_t         stack_size;

    void         *(*fn)(void *);   /* thread entry function           */
    void          *arg;            /* argument to fn                  */
    void          *retval;         /* value passed to uth_exit        */

    /*
     * Intrusive singly-linked list links.
     * A TCB can live in at most one queue at a time:
     *   - scheduler run queue  (READY)
     *   - a mutex / condvar / semaphore wait queue  (BLOCKED)
     */
    struct uth_tcb *next;

    /*
     * Join support: thread waiting for *this* thread to finish.
     * At most one joiner is supported (matches pthread semantics).
     */
    struct uth_tcb *joiner;

    /* Ticket lock: which ticket this thread is holding while blocked */
    unsigned int    ticket;
} uth_tcb_t;

/* ------------------------------------------------------------------ */
/* Scheduler globals (defined in thread.c, used by sync primitives)  */
/* ------------------------------------------------------------------ */

/* The currently executing TCB (NULL when running scheduler code).    */
extern uth_tcb_t  *uth_current;

/* The scheduler's own context (returned to on every yield/block).    */
extern ucontext_t  uth_sched_ctx;

/* ------------------------------------------------------------------ */
/* Internal scheduler helpers called by sync primitives               */
/* ------------------------------------------------------------------ */

/*
 * Block the current thread: move it to BLOCKED, append to *queue,
 * and switch back to the scheduler.
 * The caller is responsible for holding whatever lock protects *queue.
 */
void uth_block(uth_tcb_t **queue);

/*
 * Wake the first thread in *queue: remove it, set READY,
 * push it onto the run queue.
 * Returns the woken TCB, or NULL if queue was empty.
 */
uth_tcb_t *uth_wake_one(uth_tcb_t **queue);

/*
 * Wake ALL threads in *queue.
 */
void uth_wake_all(uth_tcb_t **queue);

/*
 * Push a READY tcb onto the back of the run queue.
 */
void uth_enqueue(uth_tcb_t *t);

/* ------------------------------------------------------------------ */
/* Signal mask helpers (used by preempt.c and context switches)       */
/* ------------------------------------------------------------------ */

/* Disable SIGALRM delivery; returns old mask.                        */
void uth_preempt_off(sigset_t *old);

/* Restore mask saved by uth_preempt_off.                             */
void uth_preempt_restore(const sigset_t *old);

#endif /* UTHREADS_INTERNAL_H */

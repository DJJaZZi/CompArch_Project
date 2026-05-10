#ifndef UTHREADS_H
#define UTHREADS_H

/*
 * uthreads.h — Public API for the user-space green-threads library.
 *
 * Threads  : uth_create, uth_yield, uth_join, uth_exit, uth_self
 * Mutex    : uth_mutex_init/lock/unlock/destroy
 * Spinlock : uth_spin_init/lock/unlock/destroy
 * Ticket   : uth_ticket_init/lock/unlock/destroy
 * Condvar  : uth_cond_init/wait/signal/broadcast/destroy
 * Semaphore: uth_sem_init/wait/post/destroy
 * Scheduler: uth_init (must call first), uth_run (hands control to scheduler)
 */

#include <stddef.h>   /* size_t  */
#include <stdint.h>   /* uint64_t */
#include <stdatomic.h>

/* ------------------------------------------------------------------ */
/* Thread IDs and return type                                          */
/* ------------------------------------------------------------------ */

typedef unsigned int uth_tid_t;

#define UTH_TID_INVALID  0u
#define UTH_STACK_DEFAULT (512 * 1024)   /* 512 KB default stack */

/* ------------------------------------------------------------------ */
/* Scheduler / library lifecycle                                       */
/* ------------------------------------------------------------------ */

/* Must be called once before any other uth_* function.               */
void uth_init(void);

/*
 * Transfer control to the scheduler; returns when every non-detached
 * thread created from "main" has been joined or exited.
 */
void uth_run(void);

/* ------------------------------------------------------------------ */
/* Thread API                                                          */
/* ------------------------------------------------------------------ */

/*
 * Create a new thread running fn(arg).
 * stack_size == 0  →  use UTH_STACK_DEFAULT.
 * Returns new thread's TID, or UTH_TID_INVALID on error.
 */
uth_tid_t uth_create(void *(*fn)(void *), void *arg, size_t stack_size);

/* Voluntarily yield the CPU to the scheduler.                        */
void uth_yield(void);

/*
 * Wait for thread tid to finish and collect its return value.
 * *retval receives whatever the thread passed to uth_exit / returned.
 * retval may be NULL if the caller doesn't care.
 */
int uth_join(uth_tid_t tid, void **retval);

/* Terminate the calling thread with a return value.                  */
void uth_exit(void *retval) __attribute__((noreturn));

/* Return the TID of the calling thread.                              */
uth_tid_t uth_self(void);

/* Return the number of currently live (non-zombie) threads.          */
int uth_active_count(void);

/* ------------------------------------------------------------------ */
/* Mutex                                                               */
/* ------------------------------------------------------------------ */

typedef struct uth_mutex {
    atomic_int     locked;      /* 0 = free, 1 = held                 */
    uth_tid_t      owner;       /* TID that holds the lock            */
    struct uth_tcb *waiters;    /* blocked threads waiting for lock   */
} uth_mutex_t;

#define UTH_MUTEX_INITIALIZER  { .locked = 0, .owner = UTH_TID_INVALID, .waiters = NULL }

void uth_mutex_init   (uth_mutex_t *m);
void uth_mutex_lock   (uth_mutex_t *m);
void uth_mutex_unlock (uth_mutex_t *m);
void uth_mutex_destroy(uth_mutex_t *m);

/* ------------------------------------------------------------------ */
/* Spinlock  (busy-wait; TAS with atomic exchange)                    */
/* ------------------------------------------------------------------ */

typedef struct {
    atomic_flag flag;
} uth_spinlock_t;

#define UTH_SPINLOCK_INITIALIZER  { .flag = ATOMIC_FLAG_INIT }

void uth_spin_init   (uth_spinlock_t *s);
void uth_spin_lock   (uth_spinlock_t *s);
void uth_spin_unlock (uth_spinlock_t *s);
void uth_spin_destroy(uth_spinlock_t *s);

/* ------------------------------------------------------------------ */
/* Ticket lock  (FIFO-fair mutex)                                      */
/* ------------------------------------------------------------------ */

typedef struct {
    atomic_uint    next_ticket;
    atomic_uint    now_serving;
    struct uth_tcb *waiters;    /* queue of sleeping waiters per-ticket */
} uth_ticket_t;

#define UTH_TICKET_INITIALIZER  { .next_ticket = 0, .now_serving = 0, .waiters = NULL }

void uth_ticket_init   (uth_ticket_t *t);
void uth_ticket_lock   (uth_ticket_t *t);
void uth_ticket_unlock (uth_ticket_t *t);
void uth_ticket_destroy(uth_ticket_t *t);

/* ------------------------------------------------------------------ */
/* Condition variable                                                  */
/* ------------------------------------------------------------------ */

typedef struct uth_cond {
    struct uth_tcb *waiters;   /* threads sleeping on this condvar    */
} uth_cond_t;

#define UTH_COND_INITIALIZER  { .waiters = NULL }

void uth_cond_init     (uth_cond_t *c);
void uth_cond_wait     (uth_cond_t *c, uth_mutex_t *m);
void uth_cond_signal   (uth_cond_t *c);
void uth_cond_broadcast(uth_cond_t *c);
void uth_cond_destroy  (uth_cond_t *c);

/* ------------------------------------------------------------------ */
/* Semaphore                                                           */
/* ------------------------------------------------------------------ */

typedef struct {
    int             value;
    struct uth_tcb *waiters;
} uth_sem_t;

void uth_sem_init   (uth_sem_t *s, int value);
void uth_sem_wait   (uth_sem_t *s);   /* P / down / acquire          */
void uth_sem_post   (uth_sem_t *s);   /* V / up   / release          */
void uth_sem_destroy(uth_sem_t *s);

/* ------------------------------------------------------------------ */
/* Preemption control                                                  */
/* ------------------------------------------------------------------ */

/* Start/stop the SIGALRM-based preemption timer (interval_ms > 0).   */
void uth_preempt_enable (unsigned int interval_ms);
void uth_preempt_disable(void);

#endif /* UTHREADS_H */

/*
 * thread.c — Scheduler, thread lifecycle, context switching.
 *
 * Design
 * ──────
 * The scheduler runs on its own ucontext (uth_sched_ctx).  Every time a
 * thread yields, is preempted, or blocks, execution returns here via
 * swapcontext().  The scheduler picks the next READY thread from the run
 * queue (round-robin) and resumes it.
 *
 * The "main" execution context is treated as thread TID 1.  uth_run()
 * converts it into the first thread and enters the scheduler loop.
 */

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <signal.h>
#include <assert.h>
#include <sys/mman.h>

#include "uthreads.h"
#include "uthreads_internal.h"

/* ------------------------------------------------------------------ */
/* Globals                                                             */
/* ------------------------------------------------------------------ */

uth_tcb_t  *uth_current  = NULL;   /* currently running thread       */
ucontext_t  uth_sched_ctx;         /* scheduler's own context        */

/* Simple FIFO run queue (head = next to run, tail = last inserted).  */
static uth_tcb_t *rq_head = NULL;
static uth_tcb_t *rq_tail = NULL;

/* Thread table: index by (tid - 1).  Grows as needed.               */
static uth_tcb_t **tcb_table   = NULL;
static unsigned    tcb_capacity = 0;
static unsigned    tcb_count    = 0;

/* TID counter (starts at 1; 0 == UTH_TID_INVALID).                  */
static uth_tid_t   next_tid = 1;

/* Scheduler stack (for uth_sched_ctx).                               */
#define SCHED_STACK_SIZE (64 * 1024)
static char sched_stack[SCHED_STACK_SIZE];

/* ------------------------------------------------------------------ */
/* Internal queue helpers                                              */
/* ------------------------------------------------------------------ */

/* Append t to the tail of queue *head / *tail.                       */
static void queue_push(uth_tcb_t **head, uth_tcb_t **tail, uth_tcb_t *t)
{
    t->next = NULL;
    if (*tail)
        (*tail)->next = t;
    else
        *head = t;
    *tail = t;
}

/* Pop the head of the queue; returns NULL if empty.                  */
static uth_tcb_t *queue_pop(uth_tcb_t **head, uth_tcb_t **tail)
{
    uth_tcb_t *t = *head;
    if (!t) return NULL;
    *head = t->next;
    if (!*head) *tail = NULL;
    t->next = NULL;
    return t;
}

/* ------------------------------------------------------------------ */
/* Public queue helpers (used by sync primitives in mutex.c / sync.c) */
/* ------------------------------------------------------------------ */

void uth_enqueue(uth_tcb_t *t)
{
    t->state = UTH_STATE_READY;
    queue_push(&rq_head, &rq_tail, t);
}

/*
 * Block the current thread onto a wait queue and yield to the scheduler.
 *
 * Signal-mask discipline:
 *   1. SIGALRM is already blocked by the caller (uth_preempt_off).
 *   2. We swapcontext to the scheduler with SIGALRM still blocked.
 *   3. When we're resumed, we're back on our own stack with the SAME
 *      mask state as when we blocked (kernel preserves the process mask
 *      across swapcontext on Linux).
 *   4. The caller is responsible for calling uth_preempt_restore.
 *
 * It is the CALLER's responsibility to call uth_preempt_off BEFORE
 * uth_block and uth_preempt_restore AFTER uth_block returns.
 */
void uth_block(uth_tcb_t **queue)
{
    uth_tcb_t *t = uth_current;
    t->state = UTH_STATE_BLOCKED;
    /* Append to the wait queue (simple FIFO for fairness). */
    t->next = NULL;
    uth_tcb_t **p = queue;
    while (*p) p = &(*p)->next;
    *p = t;
    /* Return to the scheduler. */
    swapcontext(&t->ctx, &uth_sched_ctx);
    /*
     * When swapcontext returns, we have been resumed by the scheduler.
     * SIGALRM is still blocked because the scheduler itself runs with
     * it blocked.  The caller will restore the mask.
     */
}

uth_tcb_t *uth_wake_one(uth_tcb_t **queue)
{
    uth_tcb_t *t = *queue;
    if (!t) return NULL;
    *queue = t->next;
    t->next = NULL;
    uth_enqueue(t);
    return t;
}

void uth_wake_all(uth_tcb_t **queue)
{
    while (uth_wake_one(queue))
        ;
}

/* ------------------------------------------------------------------ */
/* Signal helpers                                                      */
/* ------------------------------------------------------------------ */

void uth_preempt_off(sigset_t *old)
{
    sigset_t block;
    sigemptyset(&block);
    sigaddset(&block, SIGALRM);
    sigprocmask(SIG_BLOCK, &block, old);
}

void uth_preempt_restore(const sigset_t *old)
{
    sigprocmask(SIG_SETMASK, old, NULL);
}

/* ------------------------------------------------------------------ */
/* TCB allocation                                                      */
/* ------------------------------------------------------------------ */

static uth_tcb_t *tcb_alloc(void)
{
    if (tcb_count >= tcb_capacity) {
        unsigned newcap = tcb_capacity ? tcb_capacity * 2 : 64;
        tcb_table = realloc(tcb_table, newcap * sizeof(*tcb_table));
        if (!tcb_table) { perror("realloc tcb_table"); exit(1); }
        memset(tcb_table + tcb_capacity, 0,
               (newcap - tcb_capacity) * sizeof(*tcb_table));
        tcb_capacity = newcap;
    }
    uth_tcb_t *t = calloc(1, sizeof *t);
    if (!t) { perror("calloc tcb"); exit(1); }
    t->tid = next_tid++;
    tcb_table[tcb_count++] = t;
    return t;
}

static uth_tcb_t *tcb_find(uth_tid_t tid)
{
    for (unsigned i = 0; i < tcb_count; i++)
        if (tcb_table[i] && tcb_table[i]->tid == tid)
            return tcb_table[i];
    return NULL;
}

static void tcb_free(uth_tcb_t *t)
{
    /* Remove from table. */
    for (unsigned i = 0; i < tcb_count; i++) {
        if (tcb_table[i] == t) {
            tcb_table[i] = NULL;
            break;
        }
    }
    if (t->stack)
        munmap(t->stack, t->stack_size);
    free(t);
}

/* ------------------------------------------------------------------ */
/* Thread entry trampoline                                             */
/* ------------------------------------------------------------------ */

/*
 * Every new thread starts here.  We call the user function, then call
 * uth_exit() with its return value.  This ensures clean teardown even
 * if the user function simply returns.
 */
static void thread_trampoline(uint32_t hi, uint32_t lo)
{
    /* Reconstruct the TCB pointer split across two 32-bit words.     */
    uintptr_t ptr = ((uintptr_t)hi << 32) | (uintptr_t)lo;
    uth_tcb_t *t  = (uth_tcb_t *)ptr;

    void *ret = t->fn(t->arg);
    uth_exit(ret);
    /* unreachable */
}

/* ------------------------------------------------------------------ */
/* Scheduler main loop                                                 */
/* ------------------------------------------------------------------ */

/*
 * The scheduler loop.  Picks the next READY thread and resumes it.
 * When no threads remain alive, returns (which causes uth_run to return).
 *
 * Signal-mask discipline
 * ───────────────────────
 * The scheduler runs with SIGALRM blocked so its own bookkeeping cannot
 * be interrupted by the preemption timer.  When a thread voluntarily
 * yields/blocks, its saved ucontext also has SIGALRM blocked (because
 * uth_preempt_off was called).  The thread's API-exit (uth_preempt_restore)
 * unblocks SIGALRM as it returns to user code.
 *
 * When a thread is preempted by SIGALRM, the signal handler saves the
 * thread's *interrupted* ucontext (which has the original user mask,
 * with SIGALRM unblocked) into the TCB, then arranges for the kernel
 * to return into the scheduler.  When the scheduler swapcontext()s
 * into that thread later, the original mask is restored — SIGALRM
 * unblocked — and preemption can fire again.
 */
static void scheduler_loop(void)
{
    /* Block SIGALRM for the entire scheduler. */
    sigset_t block_alrm;
    sigemptyset(&block_alrm);
    sigaddset(&block_alrm, SIGALRM);
    sigprocmask(SIG_BLOCK, &block_alrm, NULL);

    for (;;) {
        /* Count live threads. */
        int live = 0;
        for (unsigned i = 0; i < tcb_count; i++) {
            uth_tcb_t *t = tcb_table[i];
            if (t && t->state != UTH_STATE_ZOMBIE)
                live++;
        }
        if (live == 0)
            break;  /* all done; fall through, uth_run() returns */

        uth_tcb_t *next = queue_pop(&rq_head, &rq_tail);
        if (!next) {
            /* No READY threads (all blocked).  Cooperative deadlock. */
            continue;
        }

        assert(next->state == UTH_STATE_READY);
        next->state = UTH_STATE_RUNNING;
        uth_current = next;
        swapcontext(&uth_sched_ctx, &next->ctx);
        uth_current = NULL;
        /* On return, SIGALRM is blocked again (the thread blocked it
         * via uth_preempt_off before yielding back to us). */
    }
}

/* ------------------------------------------------------------------ */
/* Public API                                                          */
/* ------------------------------------------------------------------ */

void uth_init(void)
{
    /* Nothing to do yet; tables are lazily allocated. */
    (void)sched_stack;  /* used below */
}

void uth_run(void)
{
    static int returned = 0;

    /*
     * Save the bootstrap context.  getcontext() returns here twice:
     *   (1) the first time it's called below — we set up the scheduler
     *       and jump into it.
     *   (2) when scheduler_loop() returns and uc_link transfers control
     *       back here — we detect this via the `returned` flag and exit.
     */
    ucontext_t bootstrap;
    returned = 0;
    getcontext(&bootstrap);

    if (returned) {
        /* Coming back from scheduler — uth_run returns. */
        return;
    }

    /* Set up the scheduler's context on its own stack. */
    getcontext(&uth_sched_ctx);
    uth_sched_ctx.uc_stack.ss_sp   = sched_stack;
    uth_sched_ctx.uc_stack.ss_size = SCHED_STACK_SIZE;
    uth_sched_ctx.uc_link          = &bootstrap;  /* return here when done */
    makecontext(&uth_sched_ctx, scheduler_loop, 0);

    returned = 1;            /* will be checked when we come back     */
    setcontext(&uth_sched_ctx);  /* jump into scheduler               */
    /* unreachable */
}

uth_tid_t uth_create(void *(*fn)(void *), void *arg, size_t stack_size)
{
    sigset_t old;
    uth_preempt_off(&old);

    if (stack_size == 0)
        stack_size = UTH_STACK_DEFAULT;

    /* Allocate stack with mmap (guard page via MAP_ANONYMOUS). */
    void *stack = mmap(NULL, stack_size,
                       PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS | MAP_STACK,
                       -1, 0);
    if (stack == MAP_FAILED) {
        perror("mmap stack");
        uth_preempt_restore(&old);
        return UTH_TID_INVALID;
    }

    uth_tcb_t *t = tcb_alloc();
    t->fn         = fn;
    t->arg        = arg;
    t->stack      = stack;
    t->stack_size = stack_size;
    t->state      = UTH_STATE_READY;
    t->joiner     = NULL;
    t->retval     = NULL;

    /* Build the initial ucontext for this thread. */
    getcontext(&t->ctx);
    t->ctx.uc_stack.ss_sp   = stack;
    t->ctx.uc_stack.ss_size = stack_size;
    t->ctx.uc_link          = NULL;   /* we handle returns ourselves */

    /*
     * The thread's saved sigmask was captured by getcontext while we
     * were inside uth_preempt_off (SIGALRM blocked).  Clear SIGALRM
     * from the mask so that when this thread is first resumed via
     * swapcontext, SIGALRM is unblocked and preemption can fire.
     * (Voluntary yields will save a fresh mask on each subsequent swap.)
     */
    sigdelset(&t->ctx.uc_sigmask, SIGALRM);

    /*
     * makecontext passes int arguments; split the pointer into two
     * 32-bit halves so it's portable on 64-bit platforms.
     */
    uintptr_t ptr = (uintptr_t)t;
    uint32_t  hi  = (uint32_t)(ptr >> 32);
    uint32_t  lo  = (uint32_t)(ptr & 0xFFFFFFFFu);
    makecontext(&t->ctx, (void(*)(void))thread_trampoline, 2, hi, lo);

    /* Put on the run queue. */
    queue_push(&rq_head, &rq_tail, t);

    uth_preempt_restore(&old);
    return t->tid;
}

void uth_yield(void)
{
    sigset_t old;
    uth_preempt_off(&old);

    uth_tcb_t *t = uth_current;
    if (!t) {
        /* Called from outside any thread context — no-op. */
        uth_preempt_restore(&old);
        return;
    }

    t->state = UTH_STATE_READY;
    queue_push(&rq_head, &rq_tail, t);
    swapcontext(&t->ctx, &uth_sched_ctx);
    /*
     * When we resume, swapcontext has restored our sigmask from the
     * ucontext (it was saved by getcontext/swapcontext-out earlier).
     * The 'old' local variable is still valid because it was saved on
     * THIS thread's stack, which is preserved across swap.
     */

    uth_preempt_restore(&old);
}

void uth_exit(void *retval)
{
    sigset_t old;
    uth_preempt_off(&old);

    uth_tcb_t *t = uth_current;
    t->retval = retval;
    t->state  = UTH_STATE_ZOMBIE;

    /* Wake any thread waiting to join us. */
    if (t->joiner) {
        uth_enqueue(t->joiner);
        t->joiner = NULL;
    }

    /* Yield back to the scheduler; we will never be re-scheduled.    */
    swapcontext(&t->ctx, &uth_sched_ctx);

    /* Unreachable, but the compiler doesn't know that. */
    __builtin_unreachable();
}

/* Forward declaration */
void uth_block_noqueue(void);

int uth_join(uth_tid_t tid, void **retval)
{
    sigset_t old;
    uth_preempt_off(&old);

    uth_tcb_t *target = tcb_find(tid);
    if (!target) {
        uth_preempt_restore(&old);
        return -1;
    }

    /* If target is already a zombie, collect immediately. */
    if (target->state != UTH_STATE_ZOMBIE) {
        /* Register ourselves as the joiner and block. */
        target->joiner = uth_current;
        uth_block_noqueue();   /* see below */
        /* Re-find after wakeup (preemption could have happened). */
        target = tcb_find(tid);
        if (!target) {
            uth_preempt_restore(&old);
            return -1;
        }
    }

    if (retval)
        *retval = target->retval;

    tcb_free(target);

    uth_preempt_restore(&old);
    return 0;
}

/*
 * Block the current thread without adding it to any wait queue.
 * Used by uth_join (the joiner is stored in target->joiner instead).
 */
void uth_block_noqueue(void)
{
    uth_tcb_t *t = uth_current;
    t->state = UTH_STATE_BLOCKED;
    swapcontext(&t->ctx, &uth_sched_ctx);
}

uth_tid_t uth_self(void)
{
    return uth_current ? uth_current->tid : UTH_TID_INVALID;
}

int uth_active_count(void)
{
    int count = 0;
    for (unsigned i = 0; i < tcb_count; i++) {
        uth_tcb_t *t = tcb_table[i];
        if (t && t->state != UTH_STATE_ZOMBIE)
            count++;
    }
    return count;
}

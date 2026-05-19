/*
 * preempt.c — SIGALRM-based preemptive scheduling.
 *
 * How it works
 * ─────────────
 * 1. setitimer(ITIMER_REAL, ...) fires SIGALRM every `interval_ms` ms.
 * 2. We install a SA_SIGINFO handler so the kernel passes the
 *    *interrupted* ucontext as the third argument.
 * 3. The handler copies the interrupted context into the current
 *    thread's TCB, marks the thread READY, then setcontext()s into
 *    the scheduler — which picks the next READY thread to run.
 *
 * Why setcontext, not swapcontext?
 * ─────────────────────────────────
 * Calling swapcontext from a signal handler "works" but is dangerous
 * if signals nest: the saved context points into the alt-signal-stack
 * frame, which the kernel will reuse for the next signal.  setcontext
 * doesn't try to save anything — it just transfers — so there's no
 * dangling alt-stack reference.  setcontext is also async-signal-safe
 * per POSIX.
 *
 * fpregs self-pointer fix-up (x86-64 glibc)
 * ──────────────────────────────────────────
 * On x86-64 Linux glibc, ucontext_t embeds __fpregs_mem and stores a
 * pointer to it in uc_mcontext.fpregs.  After a struct-copy of *ucp
 * into our TCB, that pointer still aims at the *original* ucp's
 * __fpregs_mem.  We patch the pointer to aim at our copy so future
 * swapcontext into t->ctx restores the correct FPU state.
 *
 * Re-entrancy
 * ────────────
 * SIGALRM is masked during the handler (sa_mask) and during all uth_*
 * critical sections (uth_preempt_off → sigprocmask SIG_BLOCK).
 */

#include <signal.h>
#include <sys/time.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <ucontext.h>

#include "uthreads.h"
#include "uthreads_internal.h"

#define ALT_STACK_SIZE (1 << 16)   /* 64 KB; SIGSTKSZ varies by libc */

static char       *alt_stack_mem  = NULL;
static int         preempt_enabled = 0;

/* ------------------------------------------------------------------ */
/* Signal handler                                                      */
/* ------------------------------------------------------------------ */

/*
 * The SA_SIGINFO handler.  ucp is the kernel-provided ucontext_t that
 * captures exactly where the interrupted code was when the signal hit.
 *
 * Strategy
 * ─────────
 *   1. If no thread is running (uth_current == NULL) → return; the
 *      scheduler is in charge and shouldn't be preempted.
 *   2. Copy *ucp into uth_current->ctx so a future swapcontext can
 *      resume the thread from the exact instruction it was preempted at.
 *   3. Patch the fpregs self-pointer (glibc x86-64 puts FPU state in
 *      __fpregs_mem and uc_mcontext.fpregs points to it; a struct copy
 *      leaves the pointer aimed at the *original* ucp, so we re-aim it
 *      at our own copy).
 *   4. Mark the thread READY and put it on the run queue.
 *   5. setcontext(&uth_sched_ctx) — async-signal-safe transfer to the
 *      scheduler.  The handler never returns; the kernel's signal-return
 *      path is bypassed.
 *
 * Note: SIGALRM is automatically blocked inside the handler (via sa_mask),
 * so re-entrancy is impossible during these operations.
 */
static void uth_preempt_handler(int sig, siginfo_t *info, void *ucp_void)
{
    (void)sig;
    (void)info;
    ucontext_t *ucp = (ucontext_t *)ucp_void;

    /* Don't preempt the scheduler itself or in-between states. */
    if (uth_current == NULL)
        return;

    uth_tcb_t *t = uth_current;

    /* Snapshot the full machine state into the TCB. */
    t->ctx = *ucp;
#ifdef __x86_64__
    /*
     * Repair the fpregs self-pointer: after struct copy it points into
     * the original (kernel-allocated) ucp's __fpregs_mem; we need it to
     * point into t->ctx's own __fpregs_mem so future swapcontext into
     * t->ctx restores the correct FPU state.
     */
    t->ctx.uc_mcontext.fpregs = (struct _libc_fpstate *)&t->ctx.__fpregs_mem;
#endif

    t->state = UTH_STATE_READY;
    uth_enqueue(t);
    uth_current = NULL;

    /*
     * Jump to the scheduler.  setcontext() is async-signal-safe (POSIX).
     * Control transfers off the alt-signal-stack; the kernel's signal-
     * return cleanup never runs for this signal — that's fine because
     * the scheduler picks up from a fresh state.
     */
    setcontext(&uth_sched_ctx);
    /* unreachable */
}

/* ------------------------------------------------------------------ */
/* Public API                                                          */
/* ------------------------------------------------------------------ */

void uth_preempt_enable(unsigned int interval_ms)
{
    if (preempt_enabled)
        return;

    /* Allocate alt stack on the heap. */
    if (!alt_stack_mem) {
        alt_stack_mem = malloc(ALT_STACK_SIZE);
        if (!alt_stack_mem) { perror("malloc alt stack"); exit(1); }
    }
    stack_t ss = {
        .ss_sp    = alt_stack_mem,
        .ss_size  = ALT_STACK_SIZE,
        .ss_flags = 0,
    };
    if (sigaltstack(&ss, NULL) < 0) {
        perror("sigaltstack");
        exit(1);
    }

    /* Install SA_SIGINFO handler. */
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = uth_preempt_handler;
    sa.sa_flags     = SA_SIGINFO | SA_RESTART | SA_ONSTACK;
    sigemptyset(&sa.sa_mask);
    /* SIGALRM is automatically blocked while the handler runs. */
    if (sigaction(SIGALRM, &sa, NULL) < 0) {
        perror("sigaction SIGALRM");
        exit(1);
    }

    /* Start the interval timer. */
    struct itimerval itv;
    itv.it_value.tv_sec     = 0;
    itv.it_value.tv_usec    = interval_ms * 1000;
    itv.it_interval.tv_sec  = 0;
    itv.it_interval.tv_usec = interval_ms * 1000;
    if (setitimer(ITIMER_REAL, &itv, NULL) < 0) {
        perror("setitimer");
        exit(1);
    }

    preempt_enabled = 1;
}

void uth_preempt_disable(void)
{
    if (!preempt_enabled)
        return;

    /* Stop the timer. */
    struct itimerval zero = {0};
    setitimer(ITIMER_REAL, &zero, NULL);

    /* Restore default SIGALRM disposition. */
    signal(SIGALRM, SIG_DFL);

    preempt_enabled = 0;
}

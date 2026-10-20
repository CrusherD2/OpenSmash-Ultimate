/* nn::os fibers, natively.
 *
 * On the console a fiber switch is a hand-written routine, DispatchFiberContext(save, load):
 * it pushes x0, x19-x30 and q8-q15 as a 0xF0-byte frame, stores the stack pointer in *save,
 * loads the stack pointer from *load, pops that frame and returns into it. Recompiled code
 * cannot do that - a guest call is a host call, so the host stack holds the real frames -
 * so each guest fiber runs on a Windows fiber with its own host stack and its own guest
 * register context, and this routine is the only part of the SDK's fiber code replaced.
 *
 * *save receives a token naming the suspended context. A load of anything else is a frame
 * prepared by nn::os::InitializeFiber on the new fiber's stack: a host fiber is created that
 * loads that frame exactly as the original would and jumps to the saved x30.
 *
 * A fiber nn::os::FinalizeFiber is done with gives its slot to the next new one: the game's
 * script coroutines come and go by the thousand in a match. Its host fiber is deleted only
 * then - it stays where the guest fiber last stopped, and is never switched to again.
 *
 * Fibers may resume on a different thread from the one they were suspended on, so the
 * thread's identity (kernel thread, thread-local region, TPIDR_EL0) moves with every switch. */
#define _CRT_SECURE_NO_WARNINGS
#include <stdio.h>

#include "host.h"

#define TOKEN_TAG 0xF1BE000000000000ull
#define TOKEN_USE 0x0000FFFF00000000ull   /* how often the slot was used: an old fiber's token is not the new one's */
#define FRAME 0xF0
#define CONTEXT 0x50                      /* (nn::os::FiberType: where the context is kept) */
#define MAX_FIBERS 16384

typedef struct Fib { LPVOID host; Cpu *cpu; u64 token; u64 guest; int finalized; } Fib;

static Fib *fibs[MAX_FIBERS];
static u32 fib_count;
static Fib *spare[MAX_FIBERS];   /* finalized: their slots go to new fibers */
static u32 nspare;
static u64 started;
static SRWLOCK lock = SRWLOCK_INIT;

static Fib *by_token(u64 token) {
    if ((token & 0xFFFF000000000000ull) != TOKEN_TAG) return NULL;
    u64 i = token & 0xFFFFFFFF;
    Fib *f = i < fib_count ? fibs[i] : NULL;
    return f && f->token == token ? f : NULL;
}

static Fib *new_fib(Cpu *cpu) {
    Fib *f = calloc(1, sizeof *f);
    f->cpu = cpu;
    AcquireSRWLockExclusive(&lock);
    if (fib_count == MAX_FIBERS) rt_fatal("too many fibers");
    f->token = TOKEN_TAG | fib_count;
    fibs[fib_count++] = f;
    ReleaseSRWLockExclusive(&lock);
    return f;
}

void fiber_counts(u32 *slots, u32 *finalized, u64 *made) {
    AcquireSRWLockShared(&lock);
    *slots = fib_count;
    *finalized = nspare;
    *made = started;
    ReleaseSRWLockShared(&lock);
}

static void WINAPI fiber_main(void *param) {
    Fib *f = param;
    rt_lookup(f->cpu->x[30])(f->cpu);
    rt_fatal("a fiber's entry function returned");
}

/* nn::os::FinalizeFiber(fiber). */
/* signature: f81e0ff3 a9017bfd 910043fd 39408408 aa0003f3 34.....8 a9438261 f9402662 9....... f9401e68 3900867f f9002268 3900827f a9417bfd f84207f3 d65f03c0 */
HLE_WRAP(710932f130) {
    u64 guest = c->x[0];
    f_710932f130_orig(c);
    Fib *f = by_token(ld64(guest + CONTEXT));   /* (none if it never ran) */
    if (!f || f->guest != guest) return;
    AcquireSRWLockExclusive(&lock);
    if (!f->finalized) {
        f->finalized = 1;
        spare[nspare++] = f;
    }
    ReleaseSRWLockExclusive(&lock);
}

HLE(_ZN2nn2os6detail20DispatchFiberContextEPPNS1_22FiberContextForHorizonES4_) {
    u64 save = c->x[0], load = c->x[1], target = ld64(load);

    /* The context being left: this host fiber (converting the thread on first use). */
    Fib *me;
    if (IsThreadAFiber() && GetFiberData()) me = GetFiberData();
    else {
        me = new_fib(c);
        me->host = ConvertThreadToFiberEx(me, FIBER_FLAG_FLOAT_SWITCH);
        if (!me->host) rt_fatal("cannot make a fiber of a guest thread (error %lu)", (unsigned long)GetLastError());
    }
    me->cpu = c;
    st64(save, me->token);

    Fib *next = by_token(target);
    if (!next) {   /* a fresh fiber: the frame InitializeFiber prepared at `target` - in a finalized one's slot if there is one */
        AcquireSRWLockExclusive(&lock);
        next = nspare ? spare[--nspare] : NULL;
        if (next == me) next = NULL;   /* (finalized while it runs: a guest mistake - never deleted under itself) */
        if (next) {
            next->token = TOKEN_TAG | ((next->token + (1ull << 32)) & TOKEN_USE) | (next->token & 0xFFFFFFFF);
            next->finalized = 0;
        }
        started++;
        ReleaseSRWLockExclusive(&lock);
        Cpu *n;
        if (next) {
            DeleteFiber(next->host);
            n = next->cpu;
        } else {
            n = _aligned_malloc(sizeof *n, 64);
            next = new_fib(n);
        }
        memset(n, 0, sizeof *n);
        n->x[0] = ld64(target);
        for (int r = 19; r <= 30; r++) n->x[r] = ld64(target + 8ull * (r - 18));
        for (int q = 8; q <= 15; q++) memcpy(&n->v[q], P(void, target + 0x70 + 16ull * (q - 8)), 16);
        n->sp = target + FRAME;
        next->guest = load - CONTEXT;
        next->host = CreateFiberEx(64 << 10, 64 << 20, FIBER_FLAG_FLOAT_SWITCH, fiber_main, next);
        if (!next->host) rt_fatal("cannot create a host fiber (error %lu)", (unsigned long)GetLastError());
    } else {
        next->cpu->x[0] = 0;   /* what the frame of a suspended context holds in x0 */
    }
    Cpu *n = next->cpu;
    n->thread = c->thread; n->tpidrro = c->tpidrro; n->tpidr = c->tpidr; n->fpcr = c->fpcr; n->fpsr = c->fpsr;
    SwitchToFiber(next->host);
    /* Resumed - possibly on another thread, whose identity the switcher handed us. */
}

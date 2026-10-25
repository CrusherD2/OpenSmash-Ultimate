/* Offline checks of the native kernel's waits (runtime/kernel.c's wait queues), through its system
 * calls, with made-up guest threads (C functions standing in for guest code) and no game.
 *
 *   Windows: clang-cl /nologo /O1 tools\dev\waittest.c /Febuild\waittest.exe   (with cc.BASE_CFLAGS)
 *   Linux:   clang -O1 -I runtime/macos/include -I runtime/linux/include -I runtime tools/dev/waittest.c runtime/linux/win32.c -lpthread -o build/waittest
 *   macOS:   the same with runtime/macos/memory.c and runtime/macos/platform.c in place of -lpthread
 *   (runtime/macos/include: the Windows types without the Linux build's Direct3D headers)
 *
 *   waittest   (with SSBU_THREAD_RECYCLING=1, and without)
 *       WaitSynchronization on events and on threads ending, svcArbitrateLock/Unlock, the condition
 *       variables (svcWaitProcessWideKeyAtomic, svcSignalProcessWideKey) and the address arbiter
 *       (svcWaitForAddress, svcSignalToAddress): a signal before the wait, timeouts, cancels, many
 *       waiters and one signal (first come first served, only the waiters of what was signalled
 *       woken, and each once), a wait on several objects, an event set and cleared before its
 *       waiters run (they wait on, as before), waiters that exit, the result codes; the freeze
 *       report's states; then a stress run of each, with timeouts down to a millisecond, for lost
 *       wake-ups (a timeout of 5 s that passes, or a wait 2 s late, is one). After each part no
 *       thread is left in a queue, and every queue made is free again.
 *
 * Exit code 0 when every check passes. tests/test_kernel_waits.py builds and runs it. */
#include <stdarg.h>
#include "../../runtime/kernel.c"
#include "../../runtime/clock.h"

/* ---- what kernel.c needs from the rest of the runtime ---------------------------------- */
int rt_trace;
u64 rt_clock_ns(void) { return clock_now_ns(); }
void rt_call(Cpu *c, u64 guest_addr) { ((GuestFn *)(uintptr_t)guest_addr)(c); }   /* (an "entry" is a C function here) */
const char *rt_describe(u64 guest_addr, char *buf, size_t n) { snprintf(buf, n, "test function %#llx", (unsigned long long)guest_addr); return buf; }
void rt_backtrace_thread(HANDLE thread, int max_frames) { (void)thread; (void)max_frames; }
_Noreturn void rt_fatal(const char *fmt, ...) {
    va_list a;
    va_start(a, fmt);
    printf("FATAL: ");
    vprintf(fmt, a);
    va_end(a);
    printf("\n");
    fflush(stdout);
    exit(3);
}
u64 rt_callsym(Cpu *c, const char *symbol, int n, ...) { (void)c; (void)symbol; (void)n; return 0; }
void *ipc_connect(const char *port) { (void)port; return NULL; }
u32 ipc_request(Cpu *c, void *session, u64 message, u64 size) { (void)c; (void)session; (void)message; (void)size; return 0; }
void ipc_close(void *session) { (void)session; }
void gpu_memory_unmapping(u64 addr, u64 size) { (void)addr; (void)size; }
#ifdef __APPLE__
void rt_upnp_remove(void) {}
#endif

static int failures, checks;
#define CHECK(cond, ...) do { checks++; if (!(cond)) { failures++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); fflush(stdout); } } while (0)

/* ---- guest memory: the heap ---------------------------------------------------------------- */
#define SHARED (HEAP_BASE + 0x1000)
#define MUTEX (SHARED)                  /* a userspace mutex word */
#define CV (SHARED + 4)                 /* a condition variable's key */
#define WORD (SHARED + 8)               /* the address arbiter's word */
#define TURN (SHARED + 12)              /* (the ring) whose turn it is */
#define INSIDE (SHARED + 16)            /* 1 while a thread is inside the mutex */
#define BAD (SHARED + 20)               /* what went wrong on the threads */
#define PASSED (SHARED + 24)            /* how many threads got through, in ORDER */
#define WORD2 (SHARED + 28)             /* (the stress run) another arbiter word */
#define LATE (SHARED + 32)              /* (the stress run) waits that ended SLOW or later */
#define ORDER (SHARED + 0x100)          /* the ids of the threads in the order they got through */
#define MAIN_ARRAY (HEAP_BASE + 0x2000)  /* the main thread's WaitSynchronization handles */
#define RECORDS (HEAP_BASE + 0x10000)    /* a record a thread */
#define REC(i) (RECORDS + 0x100ull * (i))
/* in a record */
#define F_COUNT 0      /* how many handles it waits on (or threads in the ring) */
#define F_RESULT 4     /* its wait's result */
#define F_TIMEOUT 8    /* its timeout (ns, i64) */
#define F_INDEX 16     /* the index WaitSynchronization gave */
#define F_DONE 20      /* 1 once it is done */
#define F_ID 24        /* its number in the test */
#define F_HELD 28      /* 1 if it held the mutex when its condition wait ended */
#define F_TYPE 32      /* (the address arbiter) the wait's type, or (ping-pong) 1 for the thread that starts */
#define F_VALUE 36     /* (the address arbiter) the value, or how many turns */
#define F_HANDLES 64   /* its handles, up to 16 */
#define SECOND 1000000000ll
#define MS 1000000ll
#define STALL (5 * SECOND)   /* the stress runs' timeout: one that passes is a wake-up lost */
#define SLOW (2 * SECOND)    /* ... and so is a wait that ends this late (taken out of its queue, never woken) */

static u32 svc(Cpu *c, u32 n) { rt_svc(c, n); return (u32)c->x[0]; }
static u32 wait_on(Cpu *c, u64 array, u32 h, i64 timeout) {
    st32(array, h);
    c->x[1] = array; c->x[2] = 1; c->x[3] = (u64)timeout;
    return svc(c, 0x18);
}
static u32 wait_many(Cpu *c, u64 array, u32 count, i64 timeout) { c->x[1] = array; c->x[2] = count; c->x[3] = (u64)timeout; return svc(c, 0x18); }
static u32 create(Cpu *c, GuestFn *fn, u64 arg, u32 *h) {
    c->x[1] = (u64)(uintptr_t)fn; c->x[2] = arg; c->x[3] = 0; c->x[4] = 44; c->x[5] = 0;
    u32 r = svc(c, 0x08);
    *h = (u32)c->x[1];
    return r;
}
static u32 start(Cpu *c, u32 h) { c->x[0] = h; return svc(c, 0x09); }
static u32 close_h(Cpu *c, u32 h) { c->x[0] = h; return svc(c, 0x16); }
static u32 new_event(Cpu *c) { svc(c, 0x45); return (u32)c->x[1]; }
static void close_event(Cpu *c, u32 h) { close_h(c, h); close_h(c, h); }   /* (svcCreateEvent's two handles) */
static u32 signal_event(Cpu *c, u32 h) { c->x[0] = h; return svc(c, 0x11); }
static u32 clear_event(Cpu *c, u32 h) { c->x[0] = h; return svc(c, 0x12); }
static u32 cancel(Cpu *c, u32 h) { c->x[0] = h; return svc(c, 0x19); }
static u32 cv_wait(Cpu *c, u32 me, i64 timeout) { c->x[0] = MUTEX; c->x[1] = CV; c->x[2] = me; c->x[3] = (u64)timeout; return svc(c, 0x1C); }
static void cv_signal(Cpu *c, i32 count) { c->x[0] = CV; c->x[1] = (u64)(i64)count; svc(c, 0x1D); }
static u32 wait_address(Cpu *c, u64 a, u32 type, i32 value, i64 timeout) { c->x[0] = a; c->x[1] = type; c->x[2] = (u64)(i64)value; c->x[3] = (u64)timeout; return svc(c, 0x34); }
static u32 signal_address(Cpu *c, u64 a, u32 type, i32 value, i32 count) { c->x[0] = a; c->x[1] = type; c->x[2] = (u64)(i64)value; c->x[3] = (u64)(i64)count; return svc(c, 0x35); }
static void yield(Cpu *c) { c->x[0] = 0; rt_svc(c, 0x0B); }

/* userspace mutexes as the SDK's work: the word holds the owner's handle, the kernel queues
 * the others (svcArbitrateLock) and hands the mutex over (svcArbitrateUnlock) */
static u32 own_handle(Cpu *c) { return ((KThread *)c->thread)->handle; }
static void guest_lock(Cpu *c, u64 m, u32 me) {
    for (;;) {
        u32 v = ld32(m);
        if (v == 0) { if (cas32(m, 0, me)) return; continue; }
        if ((v & ~WAITERS) == me) return;   /* (handed over) */
        if (!(v & WAITERS) && !cas32(m, v, v | WAITERS)) continue;
        c->x[0] = v & ~WAITERS; c->x[1] = m; c->x[2] = me;
        rt_svc(c, 0x1A);
    }
}
static void guest_unlock(Cpu *c, u64 m, u32 me) {
    if (cas32(m, me, 0)) return;
    c->x[0] = m;
    rt_svc(c, 0x1B);
}

/* kernel state, under its lock */
static KThread *thread_of(u32 h) { EnterCriticalSection(&klock); KThread *t = objects[index_of(h)].thread; LeaveCriticalSection(&klock); return t; }
static int state_of(KThread *t) { EnterCriticalSection(&klock); int w = t->wait; LeaveCriticalSection(&klock); return w; }
static u64 wakes_of(KThread *t) { EnterCriticalSection(&klock); u64 n = t->wakes; LeaveCriticalSection(&klock); return n; }
static int cancelled_of(KThread *t) { EnterCriticalSection(&klock); int n = t->cancelled; LeaveCriticalSection(&klock); return n; }
/* the threads in object h's queue, in order (up to max), and how many */
static u32 object_queue(u32 h, KThread **out, u32 max) {
    EnterCriticalSection(&klock);
    u32 n = 0;
    for (KWaitNode *w = objects[index_of(h)].first; w; w = w->next, n++)
        if (n < max) out[n] = w->thread;
    LeaveCriticalSection(&klock);
    return n;
}
/* the threads waiting for (kind, key), in order */
static u32 key_queue(int kind, u64 key, KThread **out, u32 max) {
    EnterCriticalSection(&klock);
    u32 n = 0, count;
    for (KThread *t = first_waiting(kind, key, &count); t; t = t->qnext, n++)
        if (n < max) out[n] = t;
    LeaveCriticalSection(&klock);
    return n == count ? n : ~0u;
}
/* every thread in a queue: of a mutex, a condition variable, an address, or an object */
static u32 queued(void) {
    EnterCriticalSection(&klock);
    u32 n = 0;
    for (u32 i = 0; i < QUEUES; i++)
        for (WaitQueue *q = queues[i]; q; q = q->next) n += q->count + !q->count;   /* (an empty queue left in the table counts too) */
    for (u32 k = 0; k < 4096; k++)
        for (KWaitNode *w = objects[k].first; w; w = w->next) n++;
    LeaveCriticalSection(&klock);
    return n;
}
static int queues_free(void) {   /* every queue made back among the spares */
    EnterCriticalSection(&klock);
    u32 n = 0;
    for (WaitQueue *q = spare_queues; q; q = q->next) n++;
    int all = n == queues_made;
    LeaveCriticalSection(&klock);
    return all;
}
/* up to 10 s for cond (a loaded machine); 0 if it never held */
static int until(int (*cond)(void *), void *arg) {
    for (u64 end = rt_clock_ns() + 10 * SECOND; !cond(arg);) {
        if (rt_clock_ns() > end) return 0;
        Sleep(1);
    }
    return 1;
}
struct want { KThread *t; int wait; };
static int in_state(void *p) { struct want *w = p; return state_of(w->t) == w->wait; }
static int until_waits(KThread *t, int wait) { struct want w = {t, wait}; return until(in_state, &w); }
static int is_done(void *rec) { return (int)__atomic_load_n((u32 *)G2H((u64)(uintptr_t)rec + F_DONE), __ATOMIC_SEQ_CST); }
static int until_done(int i) { return until(is_done, (void *)(uintptr_t)REC(i)); }
static int done_now(int i) { return is_done((void *)(uintptr_t)REC(i)); }
static u32 result_of(int i) { return ld32(REC(i) + F_RESULT); }

/* a record for thread i (its other fields the test's), then the thread made and started */
static u32 spawn(Cpu *c, GuestFn *fn, int i, KThread **kt) {
    u32 h;
    st32(REC(i) + F_ID, (u32)i);
    st32(REC(i) + F_DONE, 0);
    st32(REC(i) + F_RESULT, ~0u);
    st32(REC(i) + F_HELD, 0);
    CHECK(create(c, fn, REC(i), &h) == R_OK, "CreateThread");
    *kt = thread_of(h);
    CHECK(start(c, h) == R_OK, "StartThread");
    return h;
}
static void objects_record(int i, u32 count, const u32 *handles, i64 timeout) {
    st32(REC(i) + F_COUNT, count);
    st64(REC(i) + F_TIMEOUT, (u64)timeout);
    for (u32 k = 0; k < count; k++) st32(REC(i) + F_HANDLES + 4 * k, handles[k]);
}
static int join(Cpu *c, u32 h) { return wait_on(c, MAIN_ARRAY, h, 10 * SECOND) == R_OK && close_h(c, h) == R_OK; }
static int order_is(u32 n) {   /* threads 0 to n - 1 got through in that order */
    if (ld32(PASSED) != n) return 0;
    for (u32 i = 0; i < n; i++) if (ld32(ORDER + 4 * i) != i) return 0;
    return 1;
}
static void after(const char *part) {
    CHECK(queued() == 0, "%s: %u threads left in a queue", part, queued());
    CHECK(queues_free(), "%s: a queue made not free again", part);
}

/* ---- made-up guest threads ------------------------------------------------------------------ */
static void done(u64 rec, u32 result) {
    st32(rec + F_RESULT, result);
    __atomic_store_n((u32 *)G2H(rec + F_DONE), 1, __ATOMIC_SEQ_CST);
}
static void bad(void) { __atomic_fetch_add((u32 *)G2H(BAD), 1, __ATOMIC_SEQ_CST); }
static void slow(u64 t0) { if (rt_clock_ns() - t0 >= SLOW) __atomic_fetch_add((u32 *)G2H(LATE), 1, __ATOMIC_SEQ_CST); }
static void passed(u64 rec) { u32 n = __atomic_fetch_add((u32 *)G2H(PASSED), 1, __ATOMIC_SEQ_CST); st32(ORDER + 4 * n, ld32(rec + F_ID)); }

static void g_objects(Cpu *c) {   /* WaitSynchronization on the handles in its record */
    u64 rec = c->x[0];
    u32 r = wait_many(c, rec + F_HANDLES, ld32(rec + F_COUNT), (i64)ld64(rec + F_TIMEOUT));
    st32(rec + F_INDEX, (u32)c->x[1]);
    done(rec, r);
}
static void g_lock(Cpu *c) {   /* takes the mutex, gets through, lets it go; then waits on its handles, if any */
    u64 rec = c->x[0];
    u32 me = own_handle(c);
    guest_lock(c, MUTEX, me);
    passed(rec);
    guest_unlock(c, MUTEX, me);
    u32 r = R_OK;
    if (ld32(rec + F_COUNT)) r = wait_many(c, rec + F_HANDLES, ld32(rec + F_COUNT), (i64)ld64(rec + F_TIMEOUT));
    done(rec, r);
}
static void g_condition(Cpu *c) {   /* waits on the condition variable with the mutex; gets through if signalled */
    u64 rec = c->x[0];
    u32 me = own_handle(c);
    guest_lock(c, MUTEX, me);
    u32 r = cv_wait(c, me, (i64)ld64(rec + F_TIMEOUT));
    if (r == R_OK) {
        st32(rec + F_HELD, (ld32(MUTEX) & ~WAITERS) == me);
        passed(rec);
        guest_unlock(c, MUTEX, me);
    }
    done(rec, r);   /* (a timed-out wait returns without the mutex) */
}
static void g_address(Cpu *c) {   /* svcWaitForAddress on WORD, its type and value from the record */
    u64 rec = c->x[0];
    u32 r = wait_address(c, WORD, ld32(rec + F_TYPE), (i32)ld32(rec + F_VALUE), (i64)ld64(rec + F_TIMEOUT));
    if (r == R_OK) passed(rec);
    done(rec, r);
}

/* ---- WaitSynchronization ---------------------------------------------------------------- */
static void objects_basics(Cpu *c) {
    KThread *me = c->thread, *t;
    u32 ev = new_event(c), ev2 = new_event(c);
    /* signalled before the wait: at once, without sleeping */
    signal_event(c, ev);
    u64 w = wakes_of(me);
    CHECK(wait_on(c, MAIN_ARRAY, ev, -1) == R_OK && c->x[1] == 0 && wakes_of(me) == w && state_of(me) == WAIT_NONE, "a wait on a signalled event");
    CHECK(wait_on(c, MAIN_ARRAY, ev, 0) == R_OK, "a signalled event stays signalled");
    clear_event(c, ev);
    /* timeouts */
    CHECK(wait_on(c, MAIN_ARRAY, ev, 0) == R_TIMEOUT, "a timeout of 0");
    u64 t0 = rt_clock_ns();
    u32 r = wait_on(c, MAIN_ARRAY, ev, 30 * MS);
    u64 took = rt_clock_ns() - t0;
    CHECK(r == R_TIMEOUT && took >= 30 * MS && took < 5 * SECOND, "a timeout of 30 ms gave %#x after %.1f ms", r, took / 1e6);
    CHECK(state_of(me) == WAIT_NONE && queued() == 0, "a timed-out wait left in a queue");
    /* a thread's timeout */
    objects_record(0, 1, &ev, 20 * MS);
    u32 h = spawn(c, g_objects, 0, &t);
    CHECK(until_done(0) && result_of(0) == R_TIMEOUT, "a thread's timeout gave %#x", result_of(0));
    CHECK(join(c, h), "joining it");
    /* a native event, signalled by native code */
    u32 ne = kernel_create_event();
    objects_record(1, 1, &ne, 10 * SECOND);
    h = spawn(c, g_objects, 1, &t);
    CHECK(until_waits(t, WAIT_OBJECTS), "the waiter never waited");
    kernel_signal_event(ne);
    CHECK(until_done(1) && result_of(1) == R_OK, "a wait on a native event gave %#x", result_of(1));
    CHECK(join(c, h), "joining it");
    close_h(c, ne);
    /* the event closed during the wait: without recycling refused at the timeout, as before;
     * with it the wait holds the event and times out (tools/dev/kerneltest.c) */
    objects_record(2, 1, &ev2, 50 * MS);
    h = spawn(c, g_objects, 2, &t);
    CHECK(until_waits(t, WAIT_OBJECTS), "the waiter never waited");
    close_event(c, ev2);
    CHECK(until_done(2) && result_of(2) == (recycle ? R_TIMEOUT : R_INVALID_HANDLE), "a wait on a closed event gave %#x", result_of(2));
    CHECK(join(c, h), "joining it");
    close_event(c, ev);
    after("WaitSynchronization's basics");
}

/* 12 threads wait on one event, 4 on another: a signal ends the 12 waits, waking each thread
 * once, in the order they came, and wakes none of the 4 */
static void objects_many(Cpu *c) {
    u32 ev = new_event(c), other = new_event(c), h[16];
    KThread *t[16], *q[16];
    for (int i = 0; i < 16; i++) {
        objects_record(i, 1, i < 12 ? &ev : &other, 10 * SECOND);
        h[i] = spawn(c, g_objects, i, &t[i]);
        CHECK(until_waits(t[i], WAIT_OBJECTS), "waiter %d never waited", i);
    }
    u32 n = object_queue(ev, q, 16);
    int ok = n == 12;
    for (int i = 0; ok && i < 12; i++) ok = q[i] == t[i];
    CHECK(ok, "the event's queue: %u threads, or not in the order they came", n);
    u64 w[16];
    for (int i = 0; i < 16; i++) w[i] = wakes_of(t[i]);
    signal_event(c, ev);
    for (int i = 0; i < 12; i++)
        CHECK(until_done(i) && result_of(i) == R_OK && ld32(REC(i) + F_INDEX) == 0 && wakes_of(t[i]) == w[i] + 1,
              "waiter %d: %#x, index %u, woken %llu times", i, result_of(i), ld32(REC(i) + F_INDEX), (unsigned long long)(wakes_of(t[i]) - w[i]));
    for (int i = 12; i < 16; i++)
        CHECK(state_of(t[i]) == WAIT_OBJECTS && wakes_of(t[i]) == w[i] && !done_now(i), "a waiter of the other event woken (%d)", i);
    signal_event(c, other);
    for (int i = 12; i < 16; i++) CHECK(until_done(i) && result_of(i) == R_OK, "waiter %d of the other event: %#x", i, result_of(i));
    for (int i = 0; i < 16; i++) CHECK(join(c, h[i]), "joining waiter %d", i);
    close_event(c, ev);
    close_event(c, other);
    after("many waiters");
}

static void objects_several(Cpu *c) {
    u32 e[3] = {new_event(c), new_event(c), new_event(c)}, h;
    KThread *t, *q[4];
    /* a wait on three: in each queue; a signal of the second ends it with its index, out of all three */
    objects_record(0, 3, e, 10 * SECOND);
    h = spawn(c, g_objects, 0, &t);
    CHECK(until_waits(t, WAIT_OBJECTS), "the waiter never waited");
    CHECK(object_queue(e[0], q, 4) == 1 && object_queue(e[1], q, 4) == 1 && object_queue(e[2], q, 4) == 1 && q[0] == t, "not in each object's queue");
    signal_event(c, e[1]);
    CHECK(until_done(0) && result_of(0) == R_OK && ld32(REC(0) + F_INDEX) == 1, "a signal of the second: %#x, index %u", result_of(0), ld32(REC(0) + F_INDEX));
    CHECK(queued() == 0, "the wait left in another object's queue");
    CHECK(join(c, h), "joining it");
    /* signalled before the wait: the first signalled in the handles */
    signal_event(c, e[2]);
    for (int i = 0; i < 3; i++) st32(MAIN_ARRAY + 4 * i, e[i]);
    CHECK(wait_many(c, MAIN_ARRAY, 3, 10 * SECOND) == R_OK && c->x[1] == 1, "the second and third signalled: index %llu", (unsigned long long)c->x[1]);
    for (int i = 0; i < 3; i++) clear_event(c, e[i]);
    /* one waits on the first and third, another on the third: a signal of the first ends only the first wait */
    u32 one_three[2] = {e[0], e[2]}, h2;
    KThread *t2;
    objects_record(1, 2, one_three, 10 * SECOND);
    objects_record(2, 1, &e[2], 10 * SECOND);
    h = spawn(c, g_objects, 1, &t);
    CHECK(until_waits(t, WAIT_OBJECTS), "the waiter never waited");
    h2 = spawn(c, g_objects, 2, &t2);
    CHECK(until_waits(t2, WAIT_OBJECTS), "the waiter never waited");
    u64 w2 = wakes_of(t2);
    signal_event(c, e[0]);
    CHECK(until_done(1) && result_of(1) == R_OK && ld32(REC(1) + F_INDEX) == 0, "a signal of the first: %#x, index %u", result_of(1), ld32(REC(1) + F_INDEX));
    CHECK(object_queue(e[2], q, 4) == 1 && q[0] == t2 && wakes_of(t2) == w2 && state_of(t2) == WAIT_OBJECTS, "the third's queue after the first's signal");
    signal_event(c, e[2]);
    CHECK(until_done(2) && result_of(2) == R_OK, "the third's waiter: %#x", result_of(2));
    CHECK(join(c, h) && join(c, h2), "joining them");
    clear_event(c, e[0]);
    clear_event(c, e[2]);
    /* the same handle twice: the first */
    u32 twice[2] = {e[0], e[0]};
    objects_record(3, 2, twice, 10 * SECOND);
    h = spawn(c, g_objects, 3, &t);
    CHECK(until_waits(t, WAIT_OBJECTS), "the waiter never waited");
    CHECK(object_queue(e[0], q, 4) == 2 && q[0] == t && q[1] == t, "a handle named twice: not twice in the queue");
    signal_event(c, e[0]);
    CHECK(until_done(3) && result_of(3) == R_OK && ld32(REC(3) + F_INDEX) == 0, "a handle named twice: %#x, index %u", result_of(3), ld32(REC(3) + F_INDEX));
    CHECK(join(c, h), "joining it");
    for (int i = 0; i < 3; i++) close_event(c, e[i]);
    after("waits on several objects");
}

/* a thread's end: every wait on its handle ends, each waiter woken once */
static void objects_thread_end(Cpu *c) {
    u32 gate = new_event(c), h[8];
    KThread *t[8];
    objects_record(0, 1, &gate, 10 * SECOND);
    h[0] = spawn(c, g_objects, 0, &t[0]);
    CHECK(until_waits(t[0], WAIT_OBJECTS), "the thread never waited");
    for (int i = 1; i < 8; i++) {
        objects_record(i, 1, &h[0], 10 * SECOND);
        h[i] = spawn(c, g_objects, i, &t[i]);
        CHECK(until_waits(t[i], WAIT_OBJECTS), "joiner %d never waited", i);
    }
    u64 w[8];
    for (int i = 1; i < 8; i++) w[i] = wakes_of(t[i]);
    signal_event(c, gate);
    for (int i = 1; i < 8; i++)
        CHECK(until_done(i) && result_of(i) == R_OK && wakes_of(t[i]) == w[i] + 1, "joiner %d: %#x, woken %llu times", i, result_of(i),
              (unsigned long long)(wakes_of(t[i]) - w[i]));
    for (int i = 7; i >= 0; i--) CHECK(join(c, h[i]), "joining thread %d", i);
    /* a waiter that times out and exits leaves nothing behind: its objects signalled after */
    u32 two[2] = {gate, new_event(c)};
    clear_event(c, gate);
    objects_record(0, 2, two, 20 * MS);
    h[0] = spawn(c, g_objects, 0, &t[0]);
    CHECK(until_done(0) && result_of(0) == R_TIMEOUT, "a waiter's timeout: %#x", result_of(0));
    CHECK(join(c, h[0]), "joining it");
    CHECK(queued() == 0, "an exited waiter left in a queue");
    signal_event(c, two[0]);
    signal_event(c, two[1]);
    close_event(c, two[1]);
    close_event(c, gate);
    after("a thread's end");
}

/* SignalEvent and ClearEvent before the waiters run (the main thread holds the kernel's lock
 * across both): each is woken once, finds the event clear and waits on, as before (on the
 * console the signal would have ended their waits); the next signal ends them */
static void objects_pulse(Cpu *c) {
    u32 ev = new_event(c), h[4];
    KThread *t[4];
    u64 w[4];
    for (int i = 0; i < 4; i++) {
        objects_record(i, 1, &ev, 10 * SECOND);
        h[i] = spawn(c, g_objects, i, &t[i]);
        CHECK(until_waits(t[i], WAIT_OBJECTS), "waiter %d never waited", i);
        w[i] = wakes_of(t[i]);
    }
    EnterCriticalSection(&klock);
    signal_event(c, ev);
    clear_event(c, ev);
    LeaveCriticalSection(&klock);
    Sleep(20);
    for (int i = 0; i < 4; i++)
        CHECK(until_waits(t[i], WAIT_OBJECTS) && wakes_of(t[i]) == w[i] + 1 && !done_now(i), "waiter %d after an event set and cleared: woken %llu times, done %d", i,
              (unsigned long long)(wakes_of(t[i]) - w[i]), done_now(i));
    signal_event(c, ev);
    for (int i = 0; i < 4; i++)
        CHECK(until_done(i) && result_of(i) == R_OK && wakes_of(t[i]) == w[i] + 2, "waiter %d after the next signal: %#x, woken %llu times", i, result_of(i),
              (unsigned long long)(wakes_of(t[i]) - w[i]));
    for (int i = 0; i < 4; i++) CHECK(join(c, h[i]), "joining waiter %d", i);
    close_event(c, ev);
    after("an event set and cleared");
}

static void objects_cancel(Cpu *c) {
    KThread *me = c->thread, *t;
    u32 ev = new_event(c), h;
    /* a wait in progress ends with R_CANCELLED, and leaves nothing for the next */
    objects_record(0, 1, &ev, 10 * SECOND);
    h = spawn(c, g_objects, 0, &t);
    CHECK(until_waits(t, WAIT_OBJECTS), "the waiter never waited");
    u64 w = wakes_of(t);
    CHECK(cancel(c, h) == R_OK, "CancelSynchronization");
    CHECK(until_done(0) && result_of(0) == R_CANCELLED && wakes_of(t) == w + 1 && !cancelled_of(t), "a cancelled wait: %#x, woken %llu times, %d left",
          result_of(0), (unsigned long long)(wakes_of(t) - w), cancelled_of(t));
    CHECK(join(c, h), "joining it");
    CHECK(queued() == 0, "a cancelled wait left in a queue");
    /* asked before the wait: the next one ends at once - after a signalled object, before a timeout of 0, as before */
    CHECK(cancel(c, CUR_THREAD) == R_OK && cancelled_of(me), "CancelSynchronization of the current thread");
    signal_event(c, ev);
    CHECK(wait_on(c, MAIN_ARRAY, ev, 10 * SECOND) == R_OK && cancelled_of(me), "a signalled event before a cancel");
    clear_event(c, ev);
    CHECK(wait_on(c, MAIN_ARRAY, ev, 0) == R_CANCELLED && !cancelled_of(me), "a cancel before a timeout of 0");
    CHECK(wait_on(c, MAIN_ARRAY, ev, 0) == R_TIMEOUT, "a cancel counted twice");
    cancel(c, CUR_THREAD);
    u64 t0 = rt_clock_ns();
    CHECK(wait_on(c, MAIN_ARRAY, ev, 10 * SECOND) == R_CANCELLED && rt_clock_ns() - t0 < SECOND, "a cancel asked before the wait");
    close_event(c, ev);
    after("cancels");
}

/* ---- userspace mutexes -------------------------------------------------------------------- */
static void mutexes(Cpu *c, u32 main_handle) {
    KThread *t[6], *q[8];
    u32 h[6];
    st32(MUTEX, 0);
    st32(PASSED, 0);
    /* six queue behind the main thread: handed over in the order they came, each woken once */
    guest_lock(c, MUTEX, main_handle);
    for (int i = 0; i < 6; i++) {
        st32(REC(i) + F_COUNT, 0);
        h[i] = spawn(c, g_lock, i, &t[i]);
        CHECK(until_waits(t[i], WAIT_MUTEX), "locker %d never waited", i);
    }
    int ok = key_queue(WAIT_MUTEX, MUTEX, q, 8) == 6;
    for (int i = 0; ok && i < 6; i++) ok = q[i] == t[i];
    CHECK(ok && ld32(MUTEX) == (main_handle | WAITERS), "the mutex's queue, or its word %#x", ld32(MUTEX));
    u64 w[6];
    for (int i = 0; i < 6; i++) w[i] = wakes_of(t[i]);
    guest_unlock(c, MUTEX, main_handle);
    for (int i = 0; i < 6; i++) CHECK(until_done(i) && wakes_of(t[i]) == w[i] + 1, "locker %d woken %llu times", i, (unsigned long long)(wakes_of(t[i]) - w[i]));
    CHECK(order_is(6) && ld32(MUTEX) == 0, "the hand-over order, or the word %#x at the end", ld32(MUTEX));
    for (int i = 0; i < 6; i++) CHECK(join(c, h[i]), "joining locker %d", i);
    /* the word changed before ArbitrateLock: it returns at once */
    st32(MUTEX, 0x1234 | WAITERS);
    c->x[0] = 0x5678; c->x[1] = MUTEX; c->x[2] = main_handle;
    CHECK(svc(c, 0x1A) == R_OK && queued() == 0, "ArbitrateLock on a word that changed");
    st32(MUTEX, 0);
    /* a waiter cancelled: not woken, still queued; its next WaitSynchronization ends at once */
    u32 ev = new_event(c);
    st32(PASSED, 0);
    guest_lock(c, MUTEX, main_handle);
    objects_record(0, 1, &ev, 10 * SECOND);
    h[0] = spawn(c, g_lock, 0, &t[0]);
    CHECK(until_waits(t[0], WAIT_MUTEX), "the locker never waited");
    w[0] = wakes_of(t[0]);
    CHECK(cancel(c, h[0]) == R_OK, "CancelSynchronization");
    Sleep(20);
    CHECK(state_of(t[0]) == WAIT_MUTEX && wakes_of(t[0]) == w[0] && cancelled_of(t[0]), "a cancel woke a mutex waiter");
    guest_unlock(c, MUTEX, main_handle);
    CHECK(until_done(0) && result_of(0) == R_CANCELLED && order_is(1), "after the mutex, its WaitSynchronization gave %#x", result_of(0));
    CHECK(join(c, h[0]), "joining it");
    close_event(c, ev);
    after("mutexes");
}

/* ---- condition variables ----------------------------------------------------------------- */
static void conditions(Cpu *c, u32 main_handle) {
    KThread *me = c->thread, *t[8], *q[8];
    u32 h[8];
    st32(MUTEX, 0);
    st32(CV, 0);
    /* a timeout: without the mutex, the key cleared when nobody else waits */
    guest_lock(c, MUTEX, main_handle);
    u64 t0 = rt_clock_ns();
    u32 r = cv_wait(c, main_handle, 30 * MS);
    u64 took = rt_clock_ns() - t0;
    CHECK(r == R_TIMEOUT && took >= 30 * MS && ld32(MUTEX) == 0 && ld32(CV) == 0 && state_of(me) == WAIT_NONE && queued() == 0,
          "a timeout of 30 ms: %#x after %.1f ms, the mutex %#x, the key %u", r, took / 1e6, ld32(MUTEX), ld32(CV));
    guest_lock(c, MUTEX, main_handle);
    CHECK(cv_wait(c, main_handle, 0) == R_TIMEOUT && ld32(MUTEX) == 0 && ld32(CV) == 0, "a timeout of 0");
    /* signalled before the wait: nothing kept */
    cv_signal(c, 1);
    CHECK(ld32(CV) == 0, "the key after a signal with nobody waiting: %u", ld32(CV));
    guest_lock(c, MUTEX, main_handle);
    CHECK(cv_wait(c, main_handle, 20 * MS) == R_TIMEOUT, "a signal before the wait kept");
    /* another waiter keeps the key at a timeout */
    st32(PASSED, 0);
    st64(REC(0) + F_TIMEOUT, 10 * SECOND);
    h[0] = spawn(c, g_condition, 0, &t[0]);
    CHECK(until_waits(t[0], WAIT_CONDITION), "the waiter never waited");
    guest_lock(c, MUTEX, main_handle);
    CHECK(cv_wait(c, main_handle, 20 * MS) == R_TIMEOUT && ld32(CV) == 1, "the key with another waiter: %u", ld32(CV));
    u64 w0 = wakes_of(t[0]);
    t0 = rt_clock_ns();
    cv_signal(c, 1);   /* (the mutex free: the waiter takes it from the signal) */
    CHECK(until_done(0) && result_of(0) == R_OK && ld32(REC(0) + F_HELD) && ld32(CV) == 0, "the other waiter: %#x, held %u, the key %u", result_of(0),
          ld32(REC(0) + F_HELD), ld32(CV));
    CHECK(wakes_of(t[0]) == w0 + 1 && rt_clock_ns() - t0 < 2 * SECOND, "the other waiter woken %llu times, after %.1f ms", (unsigned long long)(wakes_of(t[0]) - w0),
          (rt_clock_ns() - t0) / 1e6);
    CHECK(join(c, h[0]), "joining it");

    /* six waiters; signals of 1, 2 and all while the main thread holds the mutex: first come first
     * served, each queued on the mutex and woken once, when it is handed the mutex */
    st32(PASSED, 0);
    for (int i = 0; i < 6; i++) {
        st64(REC(i) + F_TIMEOUT, 10 * SECOND);
        h[i] = spawn(c, g_condition, i, &t[i]);
        CHECK(until_waits(t[i], WAIT_CONDITION), "waiter %d never waited", i);
    }
    u64 w[8];
    for (int i = 0; i < 6; i++) w[i] = wakes_of(t[i]);
    guest_lock(c, MUTEX, main_handle);
    cv_signal(c, 1);
    CHECK(state_of(t[0]) == WAIT_MUTEX && state_of(t[1]) == WAIT_CONDITION && ld32(CV) == 1, "a signal of one");
    cv_signal(c, 2);
    CHECK(state_of(t[2]) == WAIT_MUTEX && state_of(t[3]) == WAIT_CONDITION && key_queue(WAIT_CONDITION, CV, q, 8) == 3, "a signal of two");
    int ok = key_queue(WAIT_MUTEX, MUTEX, q, 8) == 3;
    for (int i = 0; ok && i < 3; i++) ok = q[i] == t[i];
    CHECK(ok, "the mutex's queue after the signals");
    cv_signal(c, -1);
    CHECK(key_queue(WAIT_CONDITION, CV, q, 8) == 0 && key_queue(WAIT_MUTEX, MUTEX, q, 8) == 6 && ld32(CV) == 0, "a signal of all");
    ok = 1;
    for (int i = 0; i < 6; i++) ok = ok && wakes_of(t[i]) == w[i];
    CHECK(ok, "a waiter woken while the mutex was held");
    guest_unlock(c, MUTEX, main_handle);
    for (int i = 0; i < 6; i++)
        CHECK(until_done(i) && result_of(i) == R_OK && ld32(REC(i) + F_HELD) && wakes_of(t[i]) == w[i] + 1, "waiter %d: %#x, held %u, woken %llu times", i,
              result_of(i), ld32(REC(i) + F_HELD), (unsigned long long)(wakes_of(t[i]) - w[i]));
    CHECK(order_is(6) && ld32(MUTEX) == 0, "the order through the mutex, or its word %#x", ld32(MUTEX));
    for (int i = 0; i < 6; i++) CHECK(join(c, h[i]), "joining waiter %d", i);

    /* a signal with the mutex free: the waiter takes it at once */
    st32(PASSED, 0);
    h[0] = spawn(c, g_condition, 0, &t[0]);
    CHECK(until_waits(t[0], WAIT_CONDITION), "the waiter never waited");
    w0 = wakes_of(t[0]);
    t0 = rt_clock_ns();
    cv_signal(c, 1);
    CHECK(until_done(0) && result_of(0) == R_OK && ld32(REC(0) + F_HELD) && wakes_of(t[0]) == w0 + 1 && rt_clock_ns() - t0 < 2 * SECOND,
          "a signal with the mutex free: %#x, woken %llu times, after %.1f ms", result_of(0), (unsigned long long)(wakes_of(t[0]) - w0), (rt_clock_ns() - t0) / 1e6);
    CHECK(join(c, h[0]), "joining it");
    /* a cancel neither ends a condition wait nor wakes it */
    h[0] = spawn(c, g_condition, 0, &t[0]);
    CHECK(until_waits(t[0], WAIT_CONDITION), "the waiter never waited");
    w[0] = wakes_of(t[0]);
    CHECK(cancel(c, h[0]) == R_OK, "CancelSynchronization");
    Sleep(20);
    CHECK(state_of(t[0]) == WAIT_CONDITION && wakes_of(t[0]) == w[0] && !done_now(0), "a cancel ended or woke a condition wait");
    cv_signal(c, 1);
    CHECK(until_done(0) && result_of(0) == R_OK, "the cancelled waiter, signalled: %#x", result_of(0));
    CHECK(join(c, h[0]), "joining it");
    after("condition variables");
}

/* ---- the address arbiter ---------------------------------------------------------------- */
static void addresses(Cpu *c) {
    KThread *me = c->thread, *t[10], *q[10];
    u32 h[10];
    /* the result codes */
    st32(WORD, 5);
    CHECK(wait_address(c, WORD, 0, 5, 10 * SECOND) == R_INVALID_STATE, "WaitIfLessThan 5 on 5");
    CHECK(wait_address(c, WORD, 2, 4, 10 * SECOND) == R_INVALID_STATE, "WaitIfEqual 4 on 5");
    CHECK(wait_address(c, WORD, 3, 4, 10 * SECOND) == R_INVALID_ENUM, "a wait of type 3");
    CHECK(wait_address(c, WORD, 0, 6, 0) == R_TIMEOUT && ld32(WORD) == 5, "WaitIfLessThan with a timeout of 0");
    CHECK(wait_address(c, WORD, 1, 6, 0) == R_TIMEOUT && ld32(WORD) == 4, "DecrementAndWaitIfLessThan with a timeout of 0: %d", (i32)ld32(WORD));
    u64 t0 = rt_clock_ns();
    u32 r = wait_address(c, WORD, 2, 4, 30 * MS);
    u64 took = rt_clock_ns() - t0;
    CHECK(r == R_TIMEOUT && took >= 30 * MS && took < 5 * SECOND && state_of(me) == WAIT_NONE && queued() == 0, "a timeout of 30 ms: %#x after %.1f ms", r, took / 1e6);
    /* signalled before the wait: nothing kept */
    CHECK(signal_address(c, WORD, 0, 0, -1) == R_OK && wait_address(c, WORD, 2, 4, 20 * MS) == R_TIMEOUT, "a signal before the wait kept");
    CHECK(signal_address(c, WORD, 1, 3, 1) == R_INVALID_STATE && ld32(WORD) == 4, "SignalAndIncrementIfEqual 3 on 4");
    CHECK(signal_address(c, WORD, 1, 4, 1) == R_OK && ld32(WORD) == 5, "SignalAndIncrementIfEqual 4 on 4");
    CHECK(signal_address(c, WORD, 2, 4, 1) == R_INVALID_STATE && ld32(WORD) == 5, "SignalAndModifyByWaitingCountIfEqual 4 on 5");
    CHECK(signal_address(c, WORD, 2, 5, 1) == R_OK && ld32(WORD) == 6, "SignalAndModifyByWaitingCountIfEqual with nobody waiting: %d", (i32)ld32(WORD));
    CHECK(signal_address(c, WORD, 3, 6, 1) == R_INVALID_ENUM, "a signal of type 3");

    /* six wait while the word is 0: signals of 1, 2 and 3 end the waits first come first served,
     * waking each waiter once and the others not at all */
    st32(WORD, 0);
    st32(PASSED, 0);
    for (int i = 0; i < 6; i++) {
        st32(REC(i) + F_TYPE, 2);
        st32(REC(i) + F_VALUE, 0);
        st64(REC(i) + F_TIMEOUT, 10 * SECOND);
        h[i] = spawn(c, g_address, i, &t[i]);
        CHECK(until_waits(t[i], WAIT_ADDRESS), "waiter %d never waited", i);
    }
    int ok = key_queue(WAIT_ADDRESS, WORD, q, 10) == 6;
    for (int i = 0; ok && i < 6; i++) ok = q[i] == t[i];
    CHECK(ok, "the address's queue");
    u64 w[10];
    for (int i = 0; i < 6; i++) w[i] = wakes_of(t[i]);
    CHECK(signal_address(c, WORD, 2, 0, 1) == R_OK && ld32(WORD) == 0, "SignalAndModifyByWaitingCountIfEqual, 6 waiting, 1 signalled: %d", (i32)ld32(WORD));
    CHECK(until_done(0) && result_of(0) == R_OK, "the first waiter: %#x", result_of(0));
    for (int i = 1; i < 6; i++) CHECK(state_of(t[i]) == WAIT_ADDRESS && wakes_of(t[i]) == w[i], "waiter %d woken by a signal of one", i);
    CHECK(signal_address(c, WORD, 1, 0, 2) == R_OK && ld32(WORD) == 1, "SignalAndIncrementIfEqual, 2 signalled: %d", (i32)ld32(WORD));
    CHECK(until_done(1) && until_done(2) && result_of(1) == R_OK && result_of(2) == R_OK, "the second and third waiters");
    for (int i = 3; i < 6; i++) CHECK(state_of(t[i]) == WAIT_ADDRESS && wakes_of(t[i]) == w[i], "waiter %d woken by a signal of two", i);
    CHECK(signal_address(c, WORD, 2, 1, 3) == R_OK && ld32(WORD) == 0, "SignalAndModifyByWaitingCountIfEqual, 3 waiting, 3 signalled: %d", (i32)ld32(WORD));
    for (int i = 0; i < 6; i++)
        CHECK(until_done(i) && result_of(i) == R_OK && wakes_of(t[i]) == w[i] + 1, "waiter %d: %#x, woken %llu times", i, result_of(i),
              (unsigned long long)(wakes_of(t[i]) - w[i]));
    for (int i = 0; i < 6; i++) CHECK(join(c, h[i]), "joining waiter %d", i);
    /* all signalled with some waiting: the value less 2; a waiter that decremented first */
    st32(REC(6) + F_TYPE, 2); st32(REC(6) + F_VALUE, 0); st64(REC(6) + F_TIMEOUT, 10 * SECOND);
    h[6] = spawn(c, g_address, 6, &t[6]);
    CHECK(until_waits(t[6], WAIT_ADDRESS), "the waiter never waited");
    st32(REC(7) + F_TYPE, 1); st32(REC(7) + F_VALUE, 1); st64(REC(7) + F_TIMEOUT, 10 * SECOND);
    h[7] = spawn(c, g_address, 7, &t[7]);
    CHECK(until_waits(t[7], WAIT_ADDRESS) && (i32)ld32(WORD) == -1, "the decrementing waiter: the word %d", (i32)ld32(WORD));
    CHECK(signal_address(c, WORD, 2, -1, -1) == R_OK && (i32)ld32(WORD) == -3, "SignalAndModifyByWaitingCountIfEqual, all signalled: %d", (i32)ld32(WORD));
    CHECK(until_done(6) && until_done(7) && result_of(6) == R_OK && result_of(7) == R_OK, "the two waiters");
    CHECK(join(c, h[6]) && join(c, h[7]), "joining them");
    /* a cancel neither ends an address wait nor wakes it */
    st32(WORD, 0);
    st32(REC(8) + F_TYPE, 0); st32(REC(8) + F_VALUE, 1); st64(REC(8) + F_TIMEOUT, 10 * SECOND);
    h[8] = spawn(c, g_address, 8, &t[8]);
    CHECK(until_waits(t[8], WAIT_ADDRESS), "the waiter never waited");
    w[8] = wakes_of(t[8]);
    CHECK(cancel(c, h[8]) == R_OK, "CancelSynchronization");
    Sleep(20);
    CHECK(state_of(t[8]) == WAIT_ADDRESS && wakes_of(t[8]) == w[8] && !done_now(8), "a cancel ended or woke an address wait");
    CHECK(signal_address(c, WORD, 0, 0, 1) == R_OK && until_done(8) && result_of(8) == R_OK, "the cancelled waiter, signalled: %#x", result_of(8));
    CHECK(join(c, h[8]), "joining it");
    after("the address arbiter");
}

/* ---- the freeze report's states ---------------------------------------------------------- */
static char report[4096];
static void report_line(const char *text) { strncat(report, text, sizeof report - strlen(report) - 1); }
static void states(Cpu *c, u32 main_handle) {
    KThread *t[3];
    u32 h[3];
    st32(MUTEX, 0);
    st32(WORD, 0);
    st64(REC(0) + F_TIMEOUT, 10 * SECOND);
    h[0] = spawn(c, g_condition, 0, &t[0]);
    st32(REC(1) + F_TYPE, 2); st32(REC(1) + F_VALUE, 0); st64(REC(1) + F_TIMEOUT, 10 * SECOND);
    h[1] = spawn(c, g_address, 1, &t[1]);
    CHECK(until_waits(t[0], WAIT_CONDITION) && until_waits(t[1], WAIT_ADDRESS), "the waiters never waited");
    guest_lock(c, MUTEX, main_handle);
    st32(REC(2) + F_COUNT, 0);
    h[2] = spawn(c, g_lock, 2, &t[2]);
    CHECK(until_waits(t[2], WAIT_MUTEX), "the locker never waited");
    report[0] = 0;
    kernel_thread_states(report_line);
    CHECK(strstr(report, ": waiting on a condition\n") && strstr(report, ": waiting on an address\n") && strstr(report, ": waiting for a mutex\n"),
          "the freeze report's states:\n%s", report);
    guest_unlock(c, MUTEX, main_handle);
    cv_signal(c, 1);
    signal_address(c, WORD, 0, 0, 1);
    for (int i = 0; i < 3; i++) CHECK(until_done(i) && join(c, h[i]), "thread %d did not end", i);
    after("the freeze report");
}

/* ---- stress: lost wake-ups --------------------------------------------------------------- */
static void g_ring(Cpu *c) {   /* its turns round a ring of threads: the mutex, the condition variable, all woken at each turn
                                * (with the mutex held, or every other turn after it is let go: the woken take it themselves) */
    u64 rec = c->x[0];
    u32 me = own_handle(c), id = ld32(rec + F_ID), n = ld32(rec + F_COUNT), turns = ld32(rec + F_VALUE);
    for (u32 r = 0; r < turns; r++) {
        guest_lock(c, MUTEX, me);
        while (ld32(TURN) % n != id) {
            u64 t0 = rt_clock_ns();
            if (cv_wait(c, me, STALL) != R_OK) { bad(); guest_lock(c, MUTEX, me); }
            slow(t0);
        }
        if (ld32(INSIDE)) bad();
        st32(INSIDE, 1);
        st32(TURN, ld32(TURN) + 1);
        st32(INSIDE, 0);
        if (r % 2 == 0) cv_signal(c, -1);
        guest_unlock(c, MUTEX, me);
        if (r % 2) cv_signal(c, -1);
    }
    done(rec, R_OK);
}
static void g_consumer(Cpu *c) {   /* takes its tokens from WORD, waiting on the address while there are none */
    u64 rec = c->x[0];
    for (u32 got = 0, want = ld32(rec + F_VALUE); got < want;) {
        i32 v = (i32)ld32(WORD);
        if (v > 0) { if (cas32(WORD, (u32)v, (u32)(v - 1))) got++; continue; }
        u64 t0 = rt_clock_ns();
        u32 r = wait_address(c, WORD, 0, 1, STALL);
        if (r != R_OK && r != R_INVALID_STATE) bad();
        slow(t0);
    }
    done(rec, R_OK);
}
static void g_producer(Cpu *c) {   /* puts its tokens in WORD one at a time, a waiter woken for each */
    u64 rec = c->x[0];
    for (u32 i = 0, n = ld32(rec + F_VALUE); i < n; i++) {
        __atomic_fetch_add((u32 *)G2H(WORD), 1, __ATOMIC_SEQ_CST);
        signal_address(c, WORD, 0, 0, 1);
        if (i % 64 == 0) yield(c);
    }
    done(rec, R_OK);
}
static void g_ping(Cpu *c) {   /* round trips over two events: its own (handle 0) and the other thread's (1) */
    u64 rec = c->x[0];
    u32 mine = ld32(rec + F_HANDLES), other = ld32(rec + F_HANDLES + 4), first = ld32(rec + F_TYPE);
    for (u32 i = 0, n = ld32(rec + F_VALUE); i < n; i++) {
        if (first) signal_event(c, other);
        u64 t0 = rt_clock_ns();
        if (wait_on(c, rec + 0x80, mine, STALL) != R_OK) bad();
        slow(t0);
        clear_event(c, mine);
        if (!first) signal_event(c, other);
    }
    done(rec, R_OK);
}
static void g_flicker_wait(Cpu *c) {   /* waits of a millisecond on an event another thread keeps setting and clearing */
    u64 rec = c->x[0];
    u32 ok = 0;
    for (u32 i = 0, n = ld32(rec + F_VALUE); i < n; i++) {
        u32 r = wait_on(c, rec + 0x80, ld32(rec + F_HANDLES), MS);
        if (r == R_OK) ok++;
        else if (r != R_TIMEOUT) bad();
    }
    st32(rec + F_INDEX, ok);
    done(rec, R_OK);
}
static void g_flicker_signal(Cpu *c) {
    u64 rec = c->x[0];
    u32 ev = ld32(rec + F_HANDLES);
    for (u32 i = 0; !ld32(rec + F_COUNT); i++) {   /* (until the waiters are done; now and then a pause of 1 or 2 ms, about a timeout) */
        signal_event(c, ev);
        yield(c);
        clear_event(c, ev);
        if (i % 4 == 0) Sleep(1 + i / 4 % 2);
        else yield(c);
    }
    done(rec, R_OK);
}
static void g_brief_condition(Cpu *c) {   /* waits of a millisecond on the condition variable, with the mutex */
    u64 rec = c->x[0];
    u32 me = own_handle(c), ok = 0;
    for (u32 i = 0, n = ld32(rec + F_VALUE); i < n; i++) {
        guest_lock(c, MUTEX, me);
        if (ld32(INSIDE)) bad();
        u32 r = cv_wait(c, me, MS);
        if (r == R_OK) {
            if ((ld32(MUTEX) & ~WAITERS) != me || ld32(INSIDE)) bad();
            st32(INSIDE, 1);
            st32(INSIDE, 0);
            guest_unlock(c, MUTEX, me);
            ok++;
        } else if (r != R_TIMEOUT) bad();
    }
    st32(rec + F_INDEX, ok);
    done(rec, R_OK);
}
static void g_brief_address(Cpu *c) {   /* waits of a millisecond on WORD2 while it is 0 */
    u64 rec = c->x[0];
    u32 ok = 0;
    for (u32 i = 0, n = ld32(rec + F_VALUE); i < n; i++) {
        u32 r = wait_address(c, WORD2, 2, 0, MS);
        if (r == R_OK) ok++;
        else if (r != R_TIMEOUT) bad();
    }
    st32(rec + F_INDEX, ok);
    done(rec, R_OK);
}
static void g_brief_signal(Cpu *c) {   /* signals of one, then of all, on the condition variable and WORD2, until told to stop */
    u64 rec = c->x[0];
    u32 me = own_handle(c);
    for (u32 i = 0; !ld32(rec + F_COUNT); i++) {
        if (i % 3 == 0) {   /* (with the mutex, sometimes: the woken thread queued on it) */
            guest_lock(c, MUTEX, me);
            cv_signal(c, i % 2 ? 1 : -1);
            guest_unlock(c, MUTEX, me);
        } else cv_signal(c, i % 2 ? 1 : -1);
        signal_address(c, WORD2, 0, 0, i % 2 ? 1 : -1);
        if (i % 4 == 0) Sleep(1 + i / 4 % 2);   /* (now and then a pause about a timeout) */
        else yield(c);
    }
    done(rec, R_OK);
}

static int all_done(int from, int to) {
    for (int i = from; i < to; i++) if (!until_done(i)) return 0;
    return 1;
}
static void stress(Cpu *c) {
    KThread *t[24];
    u32 h[24];
    u64 t0 = rt_clock_ns();
    memset(G2H(SHARED), 0, 0x100);
    /* the ring: 8 threads, 1500 turns each, each turn waking the 7 others */
    for (int i = 0; i < 8; i++) {
        st32(REC(i) + F_COUNT, 8);
        st32(REC(i) + F_VALUE, 1500);
        h[i] = spawn(c, g_ring, i, &t[i]);
    }
    CHECK(all_done(0, 8) && ld32(TURN) == 8 * 1500 && ld32(BAD) == 0 && ld32(MUTEX) == 0, "the ring: %u turns of %u, %u wrong, the mutex %#x", ld32(TURN), 8 * 1500,
          ld32(BAD), ld32(MUTEX));
    CHECK(ld32(LATE) == 0, "%u waits ended 2 s late or more: wake-ups lost", ld32(LATE));
    for (int i = 0; i < 8; i++) CHECK(join(c, h[i]), "joining ring thread %d", i);
    after("the ring");
    st32(BAD, 0);
    st32(LATE, 0);
    /* tokens through the address arbiter: 4 producers of 20000, 4 consumers of 20000 */
    st32(WORD, 0);
    for (int i = 0; i < 8; i++) {
        st32(REC(i) + F_VALUE, 20000);
        h[i] = spawn(c, i < 4 ? g_consumer : g_producer, i, &t[i]);
    }
    CHECK(all_done(0, 8) && ld32(WORD) == 0 && ld32(BAD) == 0, "the tokens: %d left, %u wrong", (i32)ld32(WORD), ld32(BAD));
    CHECK(ld32(LATE) == 0, "%u waits ended 2 s late or more: wake-ups lost", ld32(LATE));
    for (int i = 0; i < 8; i++) CHECK(join(c, h[i]), "joining token thread %d", i);
    after("the tokens");
    st32(BAD, 0);
    st32(LATE, 0);
    /* ping-pong over events: 4 pairs, 10000 round trips each */
    u32 ev[8];
    for (int i = 0; i < 8; i++) ev[i] = new_event(c);
    for (int i = 0; i < 8; i++) {
        st32(REC(i) + F_HANDLES, ev[i]);
        st32(REC(i) + F_HANDLES + 4, ev[i ^ 1]);
        st32(REC(i) + F_TYPE, (u32)(i % 2 == 0));
        st32(REC(i) + F_VALUE, 10000);
        h[i] = spawn(c, g_ping, i, &t[i]);
    }
    CHECK(all_done(0, 8) && ld32(BAD) == 0, "ping-pong: %u wrong", ld32(BAD));
    CHECK(ld32(LATE) == 0, "%u waits ended 2 s late or more: wake-ups lost", ld32(LATE));
    for (int i = 0; i < 8; i++) CHECK(join(c, h[i]), "joining ping-pong thread %d", i);
    for (int i = 0; i < 8; i++) close_event(c, ev[i]);
    after("ping-pong");
    st32(BAD, 0);
    st32(LATE, 0);
    /* waits of a millisecond on an event set and cleared over and over: 8 waiters of 500 */
    u32 fev = new_event(c);
    for (int i = 0; i < 8; i++) {
        st32(REC(i) + F_HANDLES, fev);
        st32(REC(i) + F_VALUE, 500);
        h[i] = spawn(c, g_flicker_wait, i, &t[i]);
    }
    st32(REC(8) + F_HANDLES, fev);
    st32(REC(8) + F_COUNT, 0);
    h[8] = spawn(c, g_flicker_signal, 8, &t[8]);
    CHECK(all_done(0, 8), "the event's waiters never ended");
    st32(REC(8) + F_COUNT, 1);
    CHECK(until_done(8) && join(c, h[8]), "the event's signaller never ended");
    u32 got = 0;
    for (int i = 0; i < 8; i++) got += ld32(REC(i) + F_INDEX);
    CHECK(ld32(BAD) == 0, "the event's waits: %u wrong", ld32(BAD));
    printf("an event set and cleared: %u of 4000 waits of a millisecond signalled, the others timed out\n", got);
    for (int i = 0; i < 8; i++) CHECK(join(c, h[i]), "joining event waiter %d", i);
    close_event(c, fev);
    after("an event set and cleared");
    st32(BAD, 0);
    st32(LATE, 0);
    /* waits of a millisecond on the condition variable and on an address, signals racing them */
    st32(MUTEX, 0);
    st32(CV, 0);
    st32(WORD2, 0);
    for (int i = 0; i < 16; i++) {
        st32(REC(i) + F_VALUE, 500);
        h[i] = spawn(c, i < 8 ? g_brief_condition : g_brief_address, i, &t[i]);
    }
    st32(REC(16) + F_COUNT, 0);
    h[16] = spawn(c, g_brief_signal, 16, &t[16]);
    CHECK(all_done(0, 16), "the brief waiters never ended");
    st32(REC(16) + F_COUNT, 1);
    CHECK(until_done(16) && join(c, h[16]), "the brief signaller never ended");
    CHECK(ld32(BAD) == 0 && ld32(MUTEX) == 0 && ld32(CV) == 0, "brief waits: %u wrong, the mutex %#x, the key %u", ld32(BAD), ld32(MUTEX), ld32(CV));
    u32 cv_ok = 0, addr_ok = 0;
    for (int i = 0; i < 16; i++) *(i < 8 ? &cv_ok : &addr_ok) += ld32(REC(i) + F_INDEX);
    printf("brief waits: %u of 4000 on the condition variable signalled, %u of 4000 on the address, the others timed out\n", cv_ok, addr_ok);
    for (int i = 0; i < 16; i++) CHECK(join(c, h[i]), "joining brief waiter %d", i);
    after("brief waits");
    printf("stress runs: %.1f s\n", (double)(rt_clock_ns() - t0) / 1e9);
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
#ifdef __APPLE__
    if (!rt_guest_window()) { printf("cannot reserve the guest's address space\n"); return 2; }
#endif
    kernel_init();
    u32 main_handle;
    Cpu *c = kernel_main_thread(&main_handle);
    c->x[1] = 16 << 20;
    CHECK(svc(c, 0x01) == R_OK, "svcSetHeapSize");
    printf("thread recycling %s\n", recycle ? "on" : "off");
    u64 t0 = rt_clock_ns();
    objects_basics(c);
    objects_many(c);
    objects_several(c);
    objects_thread_end(c);
    objects_pulse(c);
    objects_cancel(c);
    mutexes(c, main_handle);
    conditions(c, main_handle);
    addresses(c);
    states(c, main_handle);
    stress(c);
    printf("%u queues made\n%.1f s\n%d checks, %d failed\n", queues_made, (double)(rt_clock_ns() - t0) / 1e9, checks, failures);
    return failures ? 1 : 0;
}

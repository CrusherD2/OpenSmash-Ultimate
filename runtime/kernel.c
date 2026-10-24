/* A native stand-in for the console's kernel, at the system-call boundary.
 *
 * The recompiled SDK reaches the kernel through a few dozen `svc` wrappers; implementing
 * those calls here lets the SDK's own threading, locking, timing and memory code run
 * unmodified on host threads. Register conventions follow the SDK's wrappers: arguments in
 * x0-x7, the result code in w0, further outputs in x1 onwards.
 *
 * Blocking is deliberately simple for now: one lock guards all kernel state and one
 * condition variable is broadcast on every change, so each waiter re-checks its own
 * condition. Correct, not fast.
 *
 * Calls that would talk to a system service (IPC) are not implemented here: they stop the
 * run and say so, since services are replaced at the SDK function level instead. */
#define _CRT_SECURE_NO_WARNINGS
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

#include "host.h"
#include "gpu.h"   /* (gpu_memory_unmapping) */

#ifdef __APPLE__
void rt_upnp_remove(void);   /* (linux/upnp.c: the netplay relay's port mapping, if it made one) */
#endif

enum { R_OK = 0, R_TIMEOUT = 0xEA01, R_CANCELLED = 0xEC01, R_INVALID_HANDLE = 0xE401, R_INVALID_STATE = 0xFA01,
       R_INVALID_ADDRESS = 0xCC01, R_INVALID_ENUM = 0xF001, R_OUT_OF_MEMORY = 0xD001 };
enum { GM_FREE = 0, GM_CODE = 3, GM_CODE_DATA = 4, GM_HEAP = 5, GM_STACK = 0xB, GM_TLS = 0xC, GM_INACCESSIBLE = 0x10 };
enum { OBJ_FREE, OBJ_THREAD, OBJ_EVENT, OBJ_SESSION, OBJ_MEMORY };

#define WAITERS 0x40000000u          /* "has waiters" bit of a userspace mutex word */
#define CUR_THREAD 0xFFFF8000u
#define CUR_PROCESS 0xFFFF8001u
#define HOST_STACK (256ull << 20)    /* guest call depth is host call depth */
#define MAIN_STACK (1u << 20)     /* from the program's metadata */

typedef struct KThread {
    u32 handle;
    HANDLE host;
    Cpu *cpu;
    u64 entry, arg, stack_top, tlr, id;
    i32 priority, core;
    int started, exited, cancelled;
    u64 mutex_addr;     /* userspace mutex this thread is queued on, or 0 */
    u32 mutex_tag;      /* value to store in the mutex word when it is handed over */
    u64 cv_key;         /* condition variable this thread waits on, or 0 */
    u64 cv_mutex;       /* the mutex to re-acquire when that wait is signalled */
    u64 arb_addr;       /* address-arbiter address this thread waits on, or 0 */
    u64 order;          /* arrival order, so hand-over is first-come-first-served */
    CONDITION_VARIABLE woken;   /* what this thread sleeps on (kwait): wakes go to the threads concerned */
    u64 wait_handles;   /* while in WaitSynchronization: its handle array (guest) and count */
    u32 wait_count;
    struct KThread *next_dead;   /* (recycling) the next thread waiting for its host thread to end */
} KThread;

/* refs: handles open on it; waits: WaitSynchronization calls holding it; gen: how often its slot
 * was used, in the handle's upper bits (these three for recycling: see new_handle) */
typedef struct KObject { int type, signaled; KThread *thread; void *session; u32 gen, refs, waits; } KObject;
typedef struct Region { u64 base, size; u32 state, perm; } Region;

static CRITICAL_SECTION klock;
/* The threads running or named by a handle (an exited one stays while its handle is open: it may
 * still be waited on), or without recycling every thread the kernel made. Waits and hand-overs
 * look through these rather than through every handle. */
static KThread *threads[1024];
static u32 nthreads;
static KObject objects[4096];
#define REGIONS 16384
static Region regions[REGIONS];
static u32 region_count;
static struct { u64 dst, src, size; } maps[1024];
static u64 heap_size, tlr_next = KERNEL_BASE, next_thread_id = 1, wait_order;
#define TLR_AREA 0x1000000ull                /* thread-local regions: a page each from KERNEL_BASE */
static int recycle;                          /* SSBU_THREAD_RECYCLING=1 (see "thread lifetime") */
static KThread *dead;                        /* (recycling) threads freed but for their host thread */
static u64 spare_tlrs[TLR_AREA / 0x1000];    /* (recycling) regions of freed threads, for the next ones */
static u32 nspare_tlrs;
static int said_half_full;                   /* (recycling) see say_half_full */

/* ---- time ------------------------------------------------------------------------ */
static u64 now_ns(void) { return rt_clock_ns(); }

u64 rt_ticks(void) { return rt_clock_ns() * 12 / 625; }   /* (19.2 MHz) */

static u64 deadline_of(i64 timeout_ns) { return timeout_ns < 0 ? ~0ull : now_ns() + (u64)timeout_ns; }

/* Sleep until something wakes this thread (kwake) or the deadline passes; returns 0 once it
 * has passed. Each thread sleeps on its own condition variable: a mutex handed over, a
 * condition variable signalled, an event set wakes just the threads waiting for it (one shared
 * one, woken in full, sent every sleeping thread round the kernel lock at every change). The
 * callers look at their condition again either way. */
static int kwait(KThread *me, u64 deadline) {
    DWORD ms = INFINITE;
    if (deadline != ~0ull) {
        u64 now = now_ns();
        if (now >= deadline) return 0;
        ms = (DWORD)((deadline - now + 999999) / 1000000);
    }
    SleepConditionVariableCS(&me->woken, &klock, ms);
    return 1;
}

static void kwake(KThread *t) { WakeConditionVariable(&t->woken); }

/* Object `handle` became signalled: wake the threads waiting for it in WaitSynchronization. */
static void kwake_handle(u32 handle) {
    for (u32 i = 0; i < nthreads; i++) {
        KThread *t = threads[i];
        for (u32 k = 0; k < t->wait_count; k++)
            if (ld32(t->wait_handles + 4ull * k) == handle) { kwake(t); break; }
    }
}

/* ---- handles ---------------------------------------------------------------------
 * Without recycling a handle is 0xA000 plus its object's slot, as before. With it, the console's
 * layout as Atmosphere's open-source kernel has it (mesosphere's kern_k_handle_table.hpp: index
 * in bits 0-14, a linear id 1 to 0x7FFF in bits 15-29, bits 30-31 zero) - the linear id here is
 * the slot's use count, so that a handle kept past CloseHandle names nothing rather than the
 * slot's next object. Never 0, and below bit 30: mutex words hold owner handles under their
 * WAITERS bit, and the pseudo-handles (0xFFFF800x) have the top bits set. */
static void say_half_full(void);

static u32 new_handle(int type, KThread *t) {
    for (u32 i = 1; i < 4096; i++)
        if (objects[i].type == OBJ_FREE) {
            u32 gen = objects[i].gen % 0x7FFF + 1;
            objects[i] = (KObject){type, 0, t, NULL, gen, 1, 0};
            if (recycle && i >= 2048 && !said_half_full) say_half_full();
            return recycle ? gen << 15 | i : 0xA000 + i;
        }
    rt_fatal("kernel: out of handles");
}

/* (recycling) Said once, when the first free slot is past half the table. A handle nothing
 * closes keeps its slot for good - one of svcCreateEvent's two values never closed, or one the
 * game moves to a native service (ipc.c keeps none) - and a slow leak ends at "out of handles"
 * (without recycling an event's first CloseHandle frees it). */
static void say_half_full(void) {
    u32 n[OBJ_MEMORY + 1] = {0};
    for (u32 i = 1; i < 4096; i++) n[objects[i].type]++;
    said_half_full = 1;
    fprintf(stderr, "[kernel] half the handle table in use: %u threads, %u events, %u sessions, %u transfer memories (4095 stop the run)\n",
            n[OBJ_THREAD], n[OBJ_EVENT], n[OBJ_SESSION], n[OBJ_MEMORY]);
}

static u32 index_of(u32 handle) { return recycle ? handle & 0x7FFF : handle - 0xA000; }

/* The slot of the object an open handle names, or 0 (never a slot). With recycling a closed
 * handle names nothing, though its object may live on (a running thread, one a wait holds). */
static u32 slot_of(u32 handle) {
    u32 i = index_of(handle);
    if (i >= 4096 || objects[i].type == OBJ_FREE) return 0;
    return !recycle || (objects[i].refs && handle >> 15 == objects[i].gen) ? i : 0;
}

static KObject *object_of(Cpu *c, u32 handle, int type) {
    u32 i = handle == CUR_THREAD && type == OBJ_THREAD ? index_of(((KThread *)c->thread)->handle) : slot_of(handle);
    return i && objects[i].type == type ? &objects[i] : NULL;
}

static void drop_thread(KThread *t);

/* (recycling) Free an object nothing names any more: no handle open, no wait holding it, and
 * for a thread, not running (exited, or never started). */
static void release(u32 i) {
    KObject *o = &objects[i];
    if (o->refs || o->waits) return;
    if (o->type == OBJ_THREAD) {
        if (o->thread->started && !o->thread->exited) return;
        drop_thread(o->thread);
    }
    o->type = OBJ_FREE;   /* (gen stays: the slot's next object gets the next one) */
}

/* (recycling) CloseHandle. svcCreateEvent's object counts its two handles, one value here: the
 * SDK closes both when it destroys a system event (inferred, not seen in the game: Atmosphere's
 * libstratosphere, an open reimplementation of nn::os, does so in DestroyInterProcessEvent, and
 * aborts if a CloseHandle fails - so the second close must succeed too). A handle native code
 * hands out again counts once more each time (kernel_copy_handle: on the console each copy is a
 * handle of its own). */
static u32 close_handle(u32 handle) {
    u32 k = slot_of(handle);
    if (!k) return R_INVALID_HANDLE;
    if (--objects[k].refs == 0 && objects[k].type == OBJ_SESSION) ipc_close(objects[k].session);
    release(k);
    return R_OK;
}

/* ---- memory map ------------------------------------------------------------------ */
/* The regions are sorted and apart. Memory the game maps and unmaps as it goes (heap, alias
 * memory, stacks, thread-local regions) is merged with a neighbour of the same kind, as the
 * console's kernel does: the game grows its pools a piece at a time, and with one region per
 * piece a long session of matches ran out of them ("too many memory regions", ten minutes and
 * ~90 fighters in). */
static int merges(u32 state) { return state == GM_HEAP || state == GM_STACK || state == GM_TLS; }

static u32 insert_region(u64 base, u64 size, u32 state, u32 perm) {
    if (region_count == REGIONS) rt_fatal("kernel: too many memory regions");
    u32 i = region_count++;
    for (; i > 0 && regions[i - 1].base > base; i--) regions[i] = regions[i - 1];
    regions[i] = (Region){base, size, state, perm};
    return i;
}

static void erase_region(u32 i) { memmove(&regions[i], &regions[i + 1], (--region_count - i) * sizeof *regions); }

/* Forget [base, base + size): regions inside it go, regions straddling its ends are trimmed. */
static void unmap_range(u64 base, u64 size) {
    u64 end = base + size;
    for (u32 i = 0; i < region_count;) {
        Region r = regions[i];
        u64 rend = r.base + r.size;
        if (rend <= base || r.base >= end || !merges(r.state)) { i++; continue; }
        erase_region(i);
        if (r.base < base) insert_region(r.base, base - r.base, r.state, r.perm);
        if (rend > end) insert_region(end, rend - end, r.state, r.perm);
        i = 0;
    }
}

/* (also from the loader, outside system calls: the lock keeps QueryMemory's search off a
 * half-moved table) */
void kernel_add_region(u64 base, u64 size, u32 state, u32 perm) {
    if (!size) return;
    EnterCriticalSection(&klock);
    if (!merges(state)) insert_region(base, size, state, perm);
    else {
        unmap_range(base, size);   /* (mapped again over itself: the new mapping is the one there) */
        u32 i = insert_region(base, size, state, perm);
        if (i + 1 < region_count && regions[i + 1].base == base + size && regions[i + 1].state == state && regions[i + 1].perm == perm) {
            regions[i].size += regions[i + 1].size;
            erase_region(i + 1);
        }
        if (i > 0 && regions[i - 1].base + regions[i - 1].size == base && regions[i - 1].state == state && regions[i - 1].perm == perm) {
            regions[i - 1].size += regions[i].size;
            erase_region(i);
        }
    }
    LeaveCriticalSection(&klock);
}

static void query_memory(u64 info, u64 addr) {
    Region r = {0, 0, GM_FREE, 0};
    u64 end = ASPACE_BASE + ASPACE_SIZE, prev = 0;
    if (addr >= end) r = (Region){end, 0 - end, GM_INACCESSIBLE, 0};
    else {
        /* the first region ending above addr: the regions are sorted and apart, so their ends
         * are in order too (thousands of them, and C++ exceptions ask tens of thousands of
         * times a second - libunwind finds each frame's module through QueryMemory) */
        u32 i = 0, hi = region_count;
        while (i < hi) {
            u32 mid = (i + hi) / 2;
            if (regions[mid].base + regions[mid].size <= addr) i = mid + 1;
            else hi = mid;
        }
        if (i) prev = regions[i - 1].base + regions[i - 1].size;
        if (i < region_count && regions[i].base <= addr) r = regions[i];
        else r = (Region){prev, (i < region_count ? regions[i].base : end) - prev, GM_FREE, 0};
    }
    st64(info, r.base);
    st64(info + 8, r.size);
    st32(info + 16, r.state);
    st32(info + 20, 0);
    st32(info + 24, r.perm);
    st32(info + 28, 0);
    st64(info + 32, 0);
}

static int map_guarded(u64 addr, u64 size);   /* (see "memory mappings") */

/* 8 bytes of guest memory at addr if a region holds them: 0 if that memory is not mapped (hle_fiber.c
 * looks into fiber objects the game may have freed), or is a mapping's source SSBU_MAP_GUARD made
 * inaccessible. Under the lock, so it cannot be unmapped between the look and the read. */
int kernel_read64(u64 addr, u64 *out) {
    int ok = 0;
    EnterCriticalSection(&klock);
    u32 i = 0, hi = region_count;
    while (i < hi) {
        u32 mid = (i + hi) / 2;
        if (regions[mid].base + regions[mid].size <= addr) i = mid + 1;
        else hi = mid;
    }
    if (i < region_count && regions[i].base <= addr && addr + 8 <= regions[i].base + regions[i].size && (regions[i].perm & 1) && !map_guarded(addr, 8)) {
        *out = ld64(addr);
        ok = 1;
    }
    LeaveCriticalSection(&klock);
    return ok;
}

/* ---- memory mappings (svcMapMemory) ------------------------------------------------
 * On the console a mapping's pages appear at its destination, and its source is locked (no
 * access at all) until the unmap. Here the destination is a copy, written back over the source
 * at the unmap: the same to a game that keeps to the console's rules, for two copies and the
 * destination's memory. (An alias needs the source in memory that can be mapped twice: a
 * section on Windows, whose views cannot be decommitted with VirtualFree or write-watched; a
 * shared mapping on Linux; on macOS anonymous memory will do, but in 16 KiB pages, not the
 * guest's 4 KiB. The heap and the alias area would have to be allocated that way.) The status
 * line says what the copies cost.
 *
 * SSBU_MAP_GUARD=1 makes the source inaccessible while it is mapped, as on the console, so that
 * a stray access to it faults - and the crash report names the guest function - rather than
 * read what the copy left there or write what the unmap overwrites. Guarded: a source that is
 * read-write memory in the memory map, outside the alias area (where the write watch sets the
 * pages' protection itself, on macOS and on Linux before 6.7); on macOS only the 16 KiB host
 * pages wholly inside it (a host page it shares with other memory stays as it is). svcQueryMemory
 * reports the source as without the switch; kernel_read64 treats the guarded pages as not mapped
 * (it would fault on them). A system call writing into them (a file read, say) fails with an
 * error rather than faulting. */
#ifdef __APPLE__
#define HOST_PAGE 0x4000ull
#else
#define HOST_PAGE 0x1000ull
#endif
static struct { u64 lo, hi; } guarded[1024];   /* (a slot of maps each) the source's pages made inaccessible */
static int map_guard;
static u64 maps_made, maps_guarded, map_bytes, map_copy_ns;
static u32 maps_live, maps_peak;

/* Whether the memory map has all of [base, base + size) as read-write memory. */
static int read_write(u64 base, u64 size) {
    u32 i = 0, hi = region_count;
    while (i < hi) {
        u32 mid = (i + hi) / 2;
        if (regions[mid].base + regions[mid].size <= base) i = mid + 1;
        else hi = mid;
    }
    for (u64 at = base; at < base + size; i++) {
        if (i == region_count || regions[i].base > at || (regions[i].perm & 3) != 3) return 0;
        at = regions[i].base + regions[i].size;
    }
    return 1;
}

static void guard_source(u32 i) {
    u64 lo = (maps[i].src + HOST_PAGE - 1) & ~(HOST_PAGE - 1), hi = (maps[i].src + maps[i].size) & ~(HOST_PAGE - 1);
    DWORD old;
    if (lo >= hi || !read_write(maps[i].src, maps[i].size) || (maps[i].src < ALIAS_BASE + ALIAS_SIZE && ALIAS_BASE < maps[i].src + maps[i].size) ||
        !VirtualProtect(G2H(lo), hi - lo, PAGE_NOACCESS, &old))
        return;
    guarded[i].lo = lo; guarded[i].hi = hi;
    maps_guarded++;
}

/* Whether a guarded source has any of [addr, addr + size) (kernel_read64 must not read there). */
static int map_guarded(u64 addr, u64 size) {
    for (u32 i = 0; map_guard && i < 1024; i++)
        if (guarded[i].hi && addr < guarded[i].hi && guarded[i].lo < addr + size) return 1;
    return 0;
}

static void unguard_source(u32 i) {
    DWORD old;
    if (guarded[i].hi && !VirtualProtect(G2H(guarded[i].lo), guarded[i].hi - guarded[i].lo, PAGE_READWRITE, &old))
        rt_fatal("kernel: cannot make the source of the mapping at %#llx accessible again (error %lu)", maps[i].dst, (unsigned long)GetLastError());
    guarded[i].lo = guarded[i].hi = 0;
}

/* For the status lines: the mappings made and what their copies took. */
void kernel_map_status(void) {
    EnterCriticalSection(&klock);
    fprintf(stderr, "[status] memory mappings: %llu made (%.1f MiB in all), %u mapped now (at most %u at once, of 1024); %.1f ms copying",
            maps_made, (double)map_bytes / (1 << 20), maps_live, maps_peak, (double)map_copy_ns / 1e6);
    if (map_guard) fprintf(stderr, "; %llu sources made inaccessible while mapped (SSBU_MAP_GUARD)", maps_guarded);
    fprintf(stderr, "\n");
    LeaveCriticalSection(&klock);
}

static void *commit(u64 at, u64 size, const char *what) {
    void *p = VirtualAlloc(G2H(at), size, MEM_COMMIT, PAGE_READWRITE);
    if (!p) rt_fatal("kernel: cannot commit %llu KiB at %#llx for %s (error %lu)", size >> 10, at, what, (unsigned long)GetLastError());
    return p;
}

/* ---- thread lifetime ---------------------------------------------------------------
 * With SSBU_THREAD_RECYCLING=1, as on the console: a thread object lives while its thread runs
 * or a handle names it, and once it has exited and its handle is closed, its place in threads[],
 * its handle and its thread-local region go to the next ones. Without it nothing of a thread is
 * ever freed, as before: a session that had made 1024 threads, running or not, stopped at the
 * next ("too many threads").
 *
 * A thread leaves the tables at once. Its KThread, Cpu and thread-local region, and its host
 * thread's handle, wait until its host thread has ended, which the next thread made or object
 * freed checks (WaitForSingleObject(host, 0)): exit_thread runs on the exiting thread itself,
 * which is still on its way out then. Windows signals a thread's handle once it has ended; the
 * POSIX shim once its start routine has returned, after which it touches nothing of ours (it
 * never signals the guest main thread's, a DuplicateHandle: that one is never freed there). The
 * only other holder of a thread's Cpu is the fiber made of the thread itself (hle_fiber.c), which
 * runs on that thread's own stack: nothing can resume it once the thread has ended.
 *
 * Not freed with a thread, outside this file: the Linux and macOS platform layer's 256 KiB signal
 * stack of each host thread (linux/win32.c, address space only until a signal runs on it), and
 * hle_fiber.c's slot of a thread made a fiber (16384 slots, "too many fibers"). */
static void reap(void) {
    for (KThread **p = &dead; *p;) {
        KThread *t = *p;
        if (t->host && WaitForSingleObject(t->host, 0) != WAIT_OBJECT_0) { p = &t->next_dead; continue; }
        *p = t->next_dead;
        if (t->host) CloseHandle(t->host);
        spare_tlrs[nspare_tlrs++] = t->tlr;   /* (committed, its memory-map region kept) */
        _aligned_free(t->cpu);
        free(t);
    }
}

static void drop_thread(KThread *t) {
    for (u32 i = 0; i < nthreads; i++)
        if (threads[i] == t) { threads[i] = threads[--nthreads]; break; }
    t->next_dead = dead;
    dead = t;
    reap();
}

/* ---- threads --------------------------------------------------------------------- */
static KThread *new_thread(u64 entry, u64 arg, u64 stack_top, i32 priority, i32 core) {
    KThread *t = calloc(1, sizeof *t);
    if (nthreads == sizeof threads / sizeof *threads) rt_fatal("kernel: too many threads");
    threads[nthreads++] = t;
    InitializeConditionVariable(&t->woken);
    t->entry = entry; t->arg = arg; t->stack_top = stack_top; t->priority = priority; t->core = core;
    t->id = next_thread_id++;
    if (recycle) reap();
    if (nspare_tlrs) {
        t->tlr = spare_tlrs[--nspare_tlrs];
        memset(G2H(t->tlr), 0, 0x1000);   /* (a new thread's starts zeroed) */
    } else {
        if (tlr_next == KERNEL_BASE + TLR_AREA) rt_fatal("kernel: out of thread-local regions");
        t->tlr = tlr_next;
        tlr_next += 0x1000;
        commit(t->tlr, 0x1000, "a thread-local region");
        kernel_add_region(t->tlr, 0x1000, GM_TLS, 3);
    }
    t->cpu = _aligned_malloc(sizeof *t->cpu, 64);
    memset(t->cpu, 0, sizeof *t->cpu);
    t->cpu->thread = t;
    t->cpu->tpidrro = t->tlr;
    t->cpu->sp = stack_top;
    t->cpu->x[0] = arg;
    t->handle = new_handle(OBJ_THREAD, t);
    return t;
}

#ifndef _WIN32
/* Linux and macOS: a thread leaves from wherever it is by jumping back to its start (ExitThread
 * would be pthread_exit, which unwinds every guest frame on the way out). (RT_SETJMP: recomp.h's,
 * which on macOS leaves the signal mask alone - nothing changes it on a guest thread.) */
static __thread jmp_buf *exit_point;
#endif

static _Noreturn void exit_thread(KThread *t) {
    EnterCriticalSection(&klock);
    t->exited = 1;
    objects[index_of(t->handle)].signaled = 1;
    kwake_handle(t->handle);
    if (recycle) release(index_of(t->handle));   /* (freed now if its handle was closed already) */
    LeaveCriticalSection(&klock);
#ifndef _WIN32
    if (exit_point) RT_LONGJMP(*exit_point, 1);
#endif
    ExitThread(0);
}

static DWORD WINAPI thread_main(LPVOID param) {
    KThread *t = param;
    char where[96];
    WCHAR desc[128];   /* for the profiler and debuggers */
#ifdef _WIN32
    _snwprintf(desc, 127, L"guest thread %llu (%hs, priority %d)", t->id, rt_describe(t->entry, where, sizeof where), t->priority);
#else
    swprintf(desc, 127, L"guest thread %llu (%s, priority %d)", (unsigned long long)t->id, rt_describe(t->entry, where, sizeof where), t->priority);
#endif
    desc[127] = 0;
    SetThreadDescription(GetCurrentThread(), desc);
#ifndef _WIN32
    jmp_buf out;
    exit_point = &out;
    if (RT_SETJMP(out)) return 0;
#endif
    rt_call(t->cpu, t->entry);
    exit_thread(t);
}

Cpu *kernel_main_thread(u32 *handle) {
    u64 top = STACK_BASE + STACK_SIZE - 0x10000;
    commit(top - MAIN_STACK, MAIN_STACK, "the main stack");
    EnterCriticalSection(&klock);
    kernel_add_region(top - MAIN_STACK, MAIN_STACK, GM_STACK, 3);
    KThread *t = new_thread(0, 0, top, 44, 0);
    t->started = 1;
    DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &t->host, 0, FALSE, DUPLICATE_SAME_ACCESS);
    LeaveCriticalSection(&klock);
    *handle = t->handle;
    return t->cpu;
}

/* ---- userspace mutexes (svcArbitrateLock / Unlock) --------------------------------
 * The mutex word holds the owner's thread handle, plus WAITERS when the kernel has queued
 * threads on it. Unlocking hands the mutex directly to the longest-waiting thread. */
static KThread *threads_waiting(u64 KThread_field_offset, u64 addr, u32 *count) {
    KThread *first = NULL;
    *count = 0;
    for (u32 i = 0; i < nthreads; i++) {
        KThread *t = threads[i];
        if (t->exited || *(u64 *)((u8 *)t + KThread_field_offset) != addr) continue;
        ++*count;
        if (!first || t->order < first->order) first = t;
    }
    return first;
}
#define WAITING(field, addr, count) threads_waiting(offsetof(KThread, field), addr, count)

static void mutex_release(u64 addr) {
    u32 count;
    KThread *next = WAITING(mutex_addr, addr, &count);
    if (!next) { st32(addr, 0); return; }
    next->mutex_addr = 0;   /* this is what the waiter is watching for */
    st32(addr, next->mutex_tag | (count > 1 ? WAITERS : 0));
    kwake(next);
}

/* Give `t` the mutex if it is free, otherwise queue it behind the current owner. */
static void mutex_acquire_or_queue(KThread *t, u64 addr, u32 tag) {
    for (;;) {
        u32 v = ld32(addr);
        if (v == 0) {
            if (cas32(addr, 0, tag)) return;
        } else if (cas32(addr, v, v | WAITERS)) {
            t->mutex_tag = tag;
            t->order = ++wait_order;
            t->mutex_addr = addr;
            return;
        }
    }
}

/* ---- the system calls ------------------------------------------------------------ */
static u32 get_info(Cpu *c, u32 id, u64 sub, u64 *out) {
    (void)c;
    u64 image = 0;
    for (u32 i = 0; i < region_count; i++)
        if (regions[i].state == GM_CODE || regions[i].state == GM_CODE_DATA) image += regions[i].size;
    switch (id) {
    case 0: *out = 0x7; break;                          /* cores an application may use */
    case 1: *out = 0x0FFFFFFFF0000000ull; break;        /* priorities 28..59 */
    case 2: *out = ALIAS_BASE; break;
    case 3: *out = ALIAS_SIZE; break;
    case 4: *out = HEAP_BASE; break;
    case 5: *out = HEAP_LIMIT; break;
    case 6: case 21: *out = 0xCD500000ull; break;       /* memory an application gets on the console */
    case 7: case 22: *out = image + heap_size; break;
    case 8: *out = 0; break;                            /* no debugger */
    case 11: *out = 0x9E3779B97F4A7C15ull * (sub + 1) ^ now_ns(); break;   /* entropy */
    case 12: *out = ASPACE_BASE; break;
    case 13: *out = ASPACE_SIZE; break;
    case 14: *out = STACK_BASE; break;
    case 15: *out = STACK_SIZE; break;
    case 16: *out = 0x1000000; break;                   /* system resource size, from the program's metadata */
    case 17: *out = 0; break;
    case 18: *out = 0x01006A800016E000ull; break;       /* program id */
    case 20: *out = 0; break;
    case 23: *out = 1; break;                           /* is an application */
    default:
        fprintf(stderr, "[kernel] svcGetInfo: unknown id %u (sub %llu)\n", id, sub);
        return R_INVALID_ENUM;
    }
    return R_OK;
}

static u32 wait_objects(Cpu *c, u64 handles, u32 count, i64 timeout, u32 *index) {
    KThread *me = c->thread;
    u64 deadline = deadline_of(timeout);
    for (;;) {
        for (u32 i = 0; i < count; i++) {
            u32 h = ld32(handles + 4 * i), k = recycle ? index_of(h) : slot_of(h);   /* (recycling: held, closed or not) */
            if (!k) return R_INVALID_HANDLE;
            if (objects[k].signaled) { *index = i; return R_OK; }
        }
        if (me->cancelled) { me->cancelled = 0; return R_CANCELLED; }
        if (timeout == 0) return R_TIMEOUT;
        me->wait_handles = handles;
        me->wait_count = count;
        int more = kwait(me, deadline);
        me->wait_count = 0;
        if (!more) return R_TIMEOUT;
    }
}

/* With recycling the wait holds its objects, as the console's does: one whose handle another
 * thread closes meanwhile is still waited on (an exiting thread whose handle was closed is still
 * seen signalled), and is freed when the wait ends. (The handle array is the waiting thread's
 * own: it does not change under the wait - kwake_handle reads it too.) */
static u32 wait_synchronization(Cpu *c, u64 handles, u32 count, i64 timeout, u32 *index) {
    if (!recycle) return wait_objects(c, handles, count, timeout, index);
    for (u32 i = 0; i < count; i++)
        if (!slot_of(ld32(handles + 4 * i))) return R_INVALID_HANDLE;
    for (u32 i = 0; i < count; i++) objects[index_of(ld32(handles + 4 * i))].waits++;
    u32 r = wait_objects(c, handles, count, timeout, index);
    for (u32 i = 0; i < count; i++) {
        u32 k = index_of(ld32(handles + 4 * i));
        objects[k].waits--;
        release(k);
    }
    return r;
}

static u32 wait_for_address(Cpu *c, u64 addr, u32 type, i32 value, i64 timeout) {
    KThread *me = c->thread;
    i32 v = (i32)ld32(addr);
    if (type == 0 || type == 1) { if (!(v < value)) return R_INVALID_STATE; }
    else if (type == 2) { if (v != value) return R_INVALID_STATE; }
    else return R_INVALID_ENUM;
    if (type == 1) st32(addr, (u32)(v - 1));
    if (timeout == 0) return R_TIMEOUT;
    u64 deadline = deadline_of(timeout);
    me->order = ++wait_order;
    me->arb_addr = addr;
    while (me->arb_addr)
        if (!kwait(me, deadline)) { me->arb_addr = 0; return R_TIMEOUT; }
    return R_OK;
}

static u32 signal_to_address(u64 addr, u32 type, i32 value, i32 count) {
    u32 waiting;
    KThread *t = WAITING(arb_addr, addr, &waiting);
    if (type == 1) {
        if ((i32)ld32(addr) != value) return R_INVALID_STATE;
        st32(addr, (u32)(value + 1));
    } else if (type == 2) {
        if ((i32)ld32(addr) != value) return R_INVALID_STATE;
        i32 next = value + 1;
        if (waiting) next = count <= 0 ? value - 2 : ((i32)waiting <= count ? value - 1 : value);
        st32(addr, (u32)next);
    } else if (type != 0) return R_INVALID_ENUM;
    for (i32 n = 0; t && (count <= 0 || n < count); n++) {
        t->arb_addr = 0;
        kwake(t);
        t = WAITING(arb_addr, addr, &waiting);
    }
    return R_OK;
}

static u32 wait_condition(Cpu *c, u64 mutex, u64 key, u32 tag, i64 timeout) {
    KThread *me = c->thread;
    u64 deadline = deadline_of(timeout);
    u32 others;
    mutex_release(mutex);
    st32(key, 1);
    me->mutex_tag = tag;
    me->cv_mutex = mutex;
    me->order = ++wait_order;
    me->cv_key = key;
    while (me->cv_key)
        if (!kwait(me, deadline)) {
            if (!me->cv_key) break;                 /* signalled just as the wait expired */
            me->cv_key = 0;
            WAITING(cv_key, key, &others);
            if (!others) st32(key, 0);
            return R_TIMEOUT;                       /* the caller re-locks the mutex itself */
        }
    while (me->mutex_addr) kwait(me, ~0ull);        /* the signaller queued us on the mutex */
    return R_OK;
}

static void signal_condition(u64 key, i32 count) {
    u32 waiting;
    KThread *t = WAITING(cv_key, key, &waiting);
    for (i32 n = 0; t && (count <= 0 || n < count); n++) {
        t->cv_key = 0;
        /* the woken thread must own the mutex again before it returns */
        mutex_acquire_or_queue(t, t->cv_mutex, t->mutex_tag);
        kwake(t);   /* (queued on the mutex instead, it waits on there) */
        t = WAITING(cv_key, key, &waiting);
    }
    if (!t) st32(key, 0);
}

void rt_svc(Cpu *c, u32 n) {
    KThread *me = c->thread;
    KObject *o;
    u64 *x = c->x;
    if (rt_trace) fprintf(stderr, "[svc %#04x] thread %llu x0=%#llx x1=%#llx x2=%#llx x3=%#llx\n", n, me->id, x[0], x[1], x[2], x[3]);

    /* calls that never touch shared kernel state */
    switch (n) {
    case 0x0B:   /* SleepThread(ns): non-positive values are yield requests */
        if ((i64)x[0] > 0) Sleep((DWORD)((x[0] + 999999) / 1000000)); else SwitchToThread();
        return;
    case 0x10: x[0] = 0; return;                         /* GetCurrentProcessorNumber */
    case 0x1E: x[0] = rt_ticks(); return;                /* GetSystemTick */
    case 0x27:                                           /* OutputDebugString(ptr, len) */
        fprintf(stderr, "[guest] %.*s%s", (int)x[1], P(const char, x[0]), x[1] && P(const char, x[0])[x[1] - 1] == '\n' ? "" : "\n");
        x[0] = R_OK;
        return;
    case 0x26:                                           /* Break(reason, address, size) */
        if (x[2] == 4 && x[1])   /* the info is a failed result */
            rt_fatal("the guest aborted on result %#x (module %u, description %u)", ld32(x[1]), ld32(x[1]) & 0x1FF, (ld32(x[1]) >> 9) & 0x1FFF);
        rt_fatal("the guest called svcBreak (reason %#llx, info %#llx, size %#llx)", x[0], x[1], x[2]);
#ifdef __APPLE__   /* (not exit(): atexit handlers and C++ destructors under the threads still running - Metal's, AppKit's - can crash on the way out) */
    case 0x07: fprintf(stderr, "guest process exited\n"); rt_upnp_remove(); ExitProcess(0);
#else
    case 0x07: fprintf(stderr, "guest process exited\n"); exit(0);
#endif
    case 0x0A: exit_thread(me);
    case 0x1F: {   /* ConnectToNamedPort(name) -> session handle */
        void *s = ipc_connect(P(const char, x[1]));
        if (!s) rt_fatal("svcConnectToNamedPort(\"%s\"): no such port here", P(const char, x[1]));
        x[0] = R_OK; x[1] = kernel_new_session(s);
        return;
    }
    case 0x21: case 0x22: {   /* SendSyncRequest(handle) / ...WithUserBuffer(buffer, size, handle) */
        u32 h = (u32)(n == 0x21 ? x[0] : x[2]);
        void *s = kernel_session_of(h);
        if (!s) { x[0] = R_INVALID_HANDLE; return; }
        x[0] = ipc_request(c, s, n == 0x21 ? c->tpidrro : x[0], n == 0x21 ? 0x100 : x[1]);
        return;
    }
    }

    EnterCriticalSection(&klock);
    switch (n) {
    case 0x01: {   /* SetHeapSize(size) -> address */
        u64 size = x[1];
        if (size > HEAP_LIMIT) { x[0] = R_OUT_OF_MEMORY; break; }
        if (size > heap_size) commit(HEAP_BASE + heap_size, size - heap_size, "the heap");
        if (heap_size) unmap_range(HEAP_BASE, heap_size);
        heap_size = size;
        if (size) kernel_add_region(HEAP_BASE, size, GM_HEAP, 3);
        x[0] = R_OK; x[1] = HEAP_BASE;
        break;
    }
    case 0x02: case 0x03: x[0] = R_OK; break;            /* SetMemoryPermission / SetMemoryAttribute */
    case 0x04: {   /* MapMemory(dst, src, size): the pages appear at dst; here, as a copy (see "memory mappings") */
        u32 i = 0;
        while (i < 1024 && maps[i].size) i++;
        if (i == 1024) rt_fatal("kernel: too many memory mappings");
        commit(x[0], x[2], "a memory mapping");
        u64 t0 = now_ns();
        memcpy(G2H(x[0]), G2H(x[1]), x[2]);
        map_copy_ns += now_ns() - t0;
        maps[i].dst = x[0]; maps[i].src = x[1]; maps[i].size = x[2];
        if (map_guard) guard_source(i);
        kernel_add_region(x[0], x[2], GM_STACK, 3);
        maps_made++;
        map_bytes += x[2];
        if (++maps_live > maps_peak) maps_peak = maps_live;
        x[0] = R_OK;
        break;
    }
    case 0x05: {   /* UnmapMemory(dst, src, size) */
        for (u32 i = 0; i < 1024; i++)
            if (maps[i].size && maps[i].dst == x[0]) {
                unguard_source(i);
                u64 t0 = now_ns();
                memcpy(G2H(maps[i].src), G2H(maps[i].dst), maps[i].size);
                map_copy_ns += now_ns() - t0;
                gpu_memory_unmapping(maps[i].dst, maps[i].size);   /* (queued GPU work and the renderer may read it) */
                VirtualFree(G2H(maps[i].dst), maps[i].size, MEM_DECOMMIT);
                unmap_range(maps[i].dst, maps[i].size);
                maps[i].size = 0;
                maps_live--;
            }
        x[0] = R_OK;
        break;
    }
    case 0x06: query_memory(x[0], x[2]); x[0] = R_OK; x[1] = 0; break;
    case 0x08: {   /* CreateThread(entry, arg, stack top, priority, core) -> handle */
        KThread *t = new_thread(x[1], x[2], x[3], (i32)x[4], (i32)x[5]);
        x[0] = R_OK; x[1] = t->handle;
        break;
    }
    case 0x09:     /* StartThread(handle) */
        if (!(o = object_of(c, (u32)x[0], OBJ_THREAD))) { x[0] = R_INVALID_HANDLE; break; }
        o->thread->started = 1;
        o->thread->host = CreateThread(NULL, HOST_STACK, thread_main, o->thread, STACK_SIZE_PARAM_IS_A_RESERVATION, NULL);
        if (!o->thread->host) rt_fatal("kernel: cannot start a host thread (error %lu)", (unsigned long)GetLastError());
        x[0] = R_OK;
        break;
    case 0x0C:     /* GetThreadPriority(handle) -> priority */
        if (!(o = object_of(c, (u32)x[1], OBJ_THREAD))) { x[0] = R_INVALID_HANDLE; break; }
        x[0] = R_OK; x[1] = (u32)o->thread->priority;
        break;
    case 0x0D:     /* SetThreadPriority(handle, priority) */
        if (!(o = object_of(c, (u32)x[0], OBJ_THREAD))) { x[0] = R_INVALID_HANDLE; break; }
        o->thread->priority = (i32)x[1];
        x[0] = R_OK;
        break;
    case 0x0E:     /* GetThreadCoreMask(handle) -> core, mask */
        if (!(o = object_of(c, (u32)x[2], OBJ_THREAD))) { x[0] = R_INVALID_HANDLE; break; }
        x[0] = R_OK; x[1] = (u32)o->thread->core; x[2] = 0x7;
        break;
    case 0x0F:     /* SetThreadCoreMask(handle, core, mask): not kept - host threads run on any core */
        x[0] = recycle && !object_of(c, (u32)x[0], OBJ_THREAD) ? R_INVALID_HANDLE : R_OK;   /* (without recycling any handle, as before) */
        break;
    case 0x11:     /* SignalEvent(handle) */
        if (!(o = object_of(c, (u32)x[0], OBJ_EVENT))) { x[0] = R_INVALID_HANDLE; break; }
        o->signaled = 1;
        kwake_handle((u32)x[0]);
        x[0] = R_OK;
        break;
    case 0x12:     /* ClearEvent(handle) */
        if (!(o = object_of(c, (u32)x[0], OBJ_EVENT))) { x[0] = R_INVALID_HANDLE; break; }
        o->signaled = 0;
        x[0] = R_OK;
        break;
    case 0x16: {   /* CloseHandle(handle) */
        if (recycle) { x[0] = close_handle((u32)x[0]); break; }
        u32 k = (u32)x[0] - 0xA000;
        if (k < 4096 && objects[k].type == OBJ_SESSION) { ipc_close(objects[k].session); objects[k].type = OBJ_FREE; }
        if (k < 4096 && (objects[k].type == OBJ_EVENT || objects[k].type == OBJ_MEMORY)) objects[k].type = OBJ_FREE;
        x[0] = R_OK;   /* thread objects stay: their thread may still be running */
        break;
    }
    case 0x17: {   /* ResetSignal(handle) */
        u32 k = slot_of((u32)x[0]);
        if (!k) { x[0] = R_INVALID_HANDLE; break; }
        x[0] = objects[k].signaled && objects[k].type == OBJ_EVENT ? R_OK : R_INVALID_STATE;
        if (objects[k].type == OBJ_EVENT) objects[k].signaled = 0;
        break;
    }
    case 0x18: {   /* WaitSynchronization(handles, count, timeout) -> index */
        u32 index = 0;
        x[0] = wait_synchronization(c, x[1], (u32)x[2], (i64)x[3], &index);
        x[1] = index;
        break;
    }
    case 0x19:     /* CancelSynchronization(thread) */
        if (!(o = object_of(c, (u32)x[0], OBJ_THREAD))) { x[0] = R_INVALID_HANDLE; break; }
        o->thread->cancelled = 1;
        kwake(o->thread);
        x[0] = R_OK;
        break;
    case 0x1A:     /* ArbitrateLock(owner handle, mutex address, own handle) */
        if (ld32(x[1]) == ((u32)x[0] | WAITERS)) {
            me->mutex_tag = (u32)x[2];
            me->order = ++wait_order;
            me->mutex_addr = x[1];
            while (me->mutex_addr) kwait(me, ~0ull);
        }
        x[0] = R_OK;
        break;
    case 0x1B: mutex_release(x[0]); x[0] = R_OK; break;  /* ArbitrateUnlock(mutex address) */
    case 0x1C: x[0] = wait_condition(c, x[0], x[1], (u32)x[2], (i64)x[3]); break;
    case 0x1D: signal_condition(x[0], (i32)x[1]); x[0] = R_OK; break;
    case 0x25:     /* GetThreadId(handle) -> id */
        if (!(o = object_of(c, (u32)x[1], OBJ_THREAD))) { x[0] = R_INVALID_HANDLE; break; }
        x[0] = R_OK; x[1] = o->thread->id;
        break;
    case 0x24: x[0] = R_OK; x[1] = 0x51; break;          /* GetProcessId */
    case 0x29: {   /* GetInfo(id, handle, sub id) -> value */
        u64 value = 0;
        x[0] = get_info(c, (u32)x[1], x[3], &value);
        x[1] = value;
        break;
    }
    case 0x2C:     /* MapPhysicalMemory(address, size): back part of the alias area with memory */
        commit(x[0], x[1], "physical memory");
        kernel_add_region(x[0], x[1], GM_HEAP, 3);
        x[0] = R_OK;
        break;
    case 0x2D:     /* UnmapPhysicalMemory(address, size) */
        gpu_memory_unmapping(x[0], x[1]);   /* (queued GPU work and the renderer may read it) */
        VirtualFree(G2H(x[0]), x[1], MEM_DECOMMIT);
        unmap_range(x[0], x[1]);
        x[0] = R_OK;
        break;
    case 0x34: x[0] = wait_for_address(c, x[0], (u32)x[1], (i32)x[2], (i64)x[3]); break;
    case 0x35: x[0] = signal_to_address(x[0], (u32)x[1], (i32)x[2], (i32)x[3]); break;
    case 0x15: {   /* CreateTransferMemory(address, size, permission) -> handle: lent memory stays where it is */
        x[1] = new_handle(OBJ_MEMORY, NULL);
        x[0] = R_OK;
        break;
    }
    case 0x45: {   /* CreateEvent() -> writable handle, readable handle (one object here) */
        u32 h = new_handle(OBJ_EVENT, NULL);
        objects[index_of(h)].refs = 2;
        x[0] = R_OK; x[1] = h; x[2] = h;
        break;
    }
    default:
        LeaveCriticalSection(&klock);
        rt_fatal("system call %#x is not implemented by the native kernel", n);
    }
    LeaveCriticalSection(&klock);
    if (rt_trace) fprintf(stderr, "[svc %#04x] -> %#llx (x1=%#llx)\n", n, x[0], x[1]);
}

/* For diagnosis: where is every guest thread right now? */
DWORD kernel_lock_owner(void) { return (DWORD)(uintptr_t)klock.OwningThread; }

/* For a freeze report: every guest thread's state and host thread id (its stack is in the host
 * threads' part of the report). Without waiting for the kernel's lock: says so when it is held. */
void kernel_thread_states(void (*line)(const char *text)) {
    char text[200];
    if (!TryEnterCriticalSection(&klock)) {
        snprintf(text, sizeof text, "  (the kernel's lock is held by thread %lu: guest thread states not read)\n", (unsigned long)kernel_lock_owner());
        line(text);
        return;
    }
    for (u32 i = 1; i < 4096; i++) {
        KThread *t = objects[i].type == OBJ_THREAD ? objects[i].thread : NULL;
        if (!t || !t->started || t->exited || !t->host) continue;
        const char *state = t->mutex_addr ? "waiting for a mutex" : t->cv_key ? "waiting on a condition" :
                            t->arb_addr ? "waiting on an address" : "running or in a system call";
        snprintf(text, sizeof text, "  guest thread %llu (host thread %lu, priority %d): %s\n", t->id, (unsigned long)GetThreadId(t->host), t->priority, state);
        line(text);
    }
    LeaveCriticalSection(&klock);
}

void kernel_dump_threads(void) {
    EnterCriticalSection(&klock);
    for (u32 i = 1; i < 4096; i++) {
        KThread *t = objects[i].type == OBJ_THREAD ? objects[i].thread : NULL;
        if (!t || !t->started || t->exited || !t->host) continue;
        const char *state = t->mutex_addr ? "waiting for a mutex" : t->cv_key ? "waiting on a condition" :
                            t->arb_addr ? "waiting on an address" : "running or in a system call";
        fprintf(stderr, "  thread %llu (priority %d): %s\n", t->id, t->priority, state);
        rt_backtrace_thread(t->host, 6);
    }
    LeaveCriticalSection(&klock);
}

u32 kernel_new_session(void *session) {
    EnterCriticalSection(&klock);
    u32 h = new_handle(OBJ_SESSION, NULL);
    objects[index_of(h)].session = session;
    LeaveCriticalSection(&klock);
    return h;
}

void *kernel_session_of(u32 handle) {
    u32 k = slot_of(handle);
    return k && objects[k].type == OBJ_SESSION ? objects[k].session : NULL;
}

/* A handle native code gives the guest once more (an IPC copy handle, ipc.c): on the console a
 * handle of its own, so one more CloseHandle to come. */
void kernel_copy_handle(u32 handle) {
    EnterCriticalSection(&klock);
    u32 k = slot_of(handle);
    if (k) objects[k].refs++;
    LeaveCriticalSection(&klock);
}

u32 kernel_create_event(void) {
    EnterCriticalSection(&klock);
    u32 h = new_handle(OBJ_EVENT, NULL);
    LeaveCriticalSection(&klock);
    return h;
}

/* (an event the guest has closed: nothing, as one it was handed and closed - rt_attach_system_event's
 * - names nothing any more) */
void kernel_signal_event(u32 handle) {
    EnterCriticalSection(&klock);
    u32 k = slot_of(handle);
    if (k && objects[k].type == OBJ_EVENT) { objects[k].signaled = 1; kwake_handle(handle); }
    LeaveCriticalSection(&klock);
}

u32 rt_attach_system_event(Cpu *c, u64 system_event, int auto_clear) {
    u32 h = kernel_create_event();
    /* nn::os::AttachReadableHandleToSystemEvent(event, handle, manage the handle, clear mode) */
    rt_callsym(c, "_ZN2nn2os33AttachReadableHandleToSystemEventEPNS0_15SystemEventTypeEjbNS0_14EventClearModeE", 4,
               system_event, (u64)h, (u64)1, (u64)(auto_clear ? 1 : 0));
    return h;
}

void kernel_init(void) {
    InitializeCriticalSection(&klock);
    recycle = getenv("SSBU_THREAD_RECYCLING") && !strcmp(getenv("SSBU_THREAD_RECYCLING"), "1");
    if (recycle) fprintf(stderr, "[kernel] exited threads and their handles recycled (SSBU_THREAD_RECYCLING=1)\n");
    rt_clock_ns();   /* (the clock set up - and its line logged - before the game asks it) */
    /* Reserve the areas the guest maps into; pages are committed as they are used. The alias
     * area, where the game keeps its GPU memory, has its writes watched so that the renderer
     * learns which textures the CPU changed (GetWriteWatch). */
    static const struct { u64 base, size; const char *what; DWORD flags; } areas[] = {
        {HEAP_BASE, HEAP_LIMIT, "heap", 0}, {ALIAS_BASE, ALIAS_SIZE, "alias", MEM_WRITE_WATCH}, {STACK_BASE, STACK_SIZE, "stack", 0},
        {KERNEL_BASE, TLR_AREA, "thread-local", 0},
    };
    for (u32 i = 0; i < sizeof areas / sizeof *areas; i++)
        if (VirtualAlloc(G2H(areas[i].base), areas[i].size, MEM_RESERVE | areas[i].flags, PAGE_READWRITE) != G2H(areas[i].base))
            rt_fatal("kernel: cannot reserve the %s area at %#llx", areas[i].what, areas[i].base);
    map_guard = getenv("SSBU_MAP_GUARD") && !strcmp(getenv("SSBU_MAP_GUARD"), "1");
    if (map_guard) fprintf(stderr, "[kernel] a memory mapping's source is inaccessible while it is mapped (SSBU_MAP_GUARD=1)\n");
}

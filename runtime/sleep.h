/* The sleep behind the guest's svcSleepThread (kernel.c includes this once; tools/dev/sleeptest.c
 * checks it): a span in nanoseconds, never shorter, and as little longer as the system allows.
 *
 * Sleep() counts whole milliseconds on the system timer's grid: 1 ms while some thread has asked
 * for it with timeBeginPeriod(1) (the vsync thread does; Windows 11 may not keep to that for a
 * hidden or minimised window), 15.6 ms otherwise. The guest asks in nanoseconds, and a span under
 * a millisecond took a whole tick. So on Windows the sleep is on a high-resolution waitable timer
 * (Windows 10 1803 and later), due in 100 ns units: one a thread, made at its first sleep and never
 * closed (the kernel makes 1024 threads at most); without one, Sleep() as before. Linux sleeps
 * with clock_nanosleep to a time on the monotonic clock, macOS on a critical kqueue timer
 * (macos/platform.c's rt_sleep_until_ns): what the shim's waitable timers use.
 *
 * "Never shorter" is on the system timer the guest's system tick follows (Windows'
 * QueryPerformanceCounter, Linux's CLOCK_MONOTONIC, macOS's mach_absolute_time): a wait that
 * ends before the time is up is taken up again for the rest. Spans past 2^60 ns (36 years) are
 * taken as that, so that the arithmetic stays in 64 bits.
 *
 * Needs <windows.h> (or the Linux port's) first. */
#ifndef SSBU_SLEEP_H
#define SSBU_SLEEP_H
#include <stdint.h>

#define SLEEP_MAX_NS (1ull << 60)

#ifdef _WIN32
#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif
static __declspec(thread) HANDLE sleep_timer;   /* this thread's high-resolution timer */
static __declspec(thread) int sleep_untimed;    /* 1: it could not have one (before Windows 10 1803): Sleep() */

static void sleep_ns(uint64_t ns) {
    if (!ns) return;
    if (ns > SLEEP_MAX_NS) ns = SLEEP_MAX_NS;
    LARGE_INTEGER f, t;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&t);
    uint64_t hz = (uint64_t)f.QuadPart;
    /* the deadline in counter ticks, rounded up */
    uint64_t end = (uint64_t)t.QuadPart + ns / 1000000000ull * hz + (ns % 1000000000ull * hz + 999999999) / 1000000000ull;
    if (!sleep_timer && !sleep_untimed) {
        sleep_timer = CreateWaitableTimerExW(NULL, NULL, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
        sleep_untimed = !sleep_timer;
    }
    for (;;) {
        LARGE_INTEGER due = {.QuadPart = -(LONGLONG)((ns + 99) / 100)};   /* (relative) */
        if (!sleep_timer || !SetWaitableTimer(sleep_timer, &due, 0, NULL, NULL, FALSE) ||
            WaitForSingleObject(sleep_timer, INFINITE) != WAIT_OBJECT_0)
            Sleep(ns >= 0xFFFFFFFEull * 1000000 ? 0xFFFFFFFE : (DWORD)((ns + 999999) / 1000000));
        QueryPerformanceCounter(&t);
        if ((uint64_t)t.QuadPart >= end) return;
        uint64_t left = end - (uint64_t)t.QuadPart;   /* (ticks) */
        ns = left / hz * 1000000000ull + (left % hz * 1000000000ull + hz - 1) / hz;
    }
}
#elif defined(__APPLE__)
uint64_t rt_uptime_ns(void);           /* (macos/platform.c: mach_absolute_time in nanoseconds) */
void rt_sleep_until_ns(uint64_t t);

static void sleep_ns(uint64_t ns) {
    if (!ns) return;
    uint64_t end = rt_uptime_ns() + (ns > SLEEP_MAX_NS ? SLEEP_MAX_NS : ns);
    do rt_sleep_until_ns(end); while (rt_uptime_ns() < end);   /* (which rounds the time down to a counter tick) */
}
#else
#include <errno.h>
#include <time.h>

static void sleep_ns(uint64_t ns) {
    if (!ns) return;
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    uint64_t end = (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec + (ns > SLEEP_MAX_NS ? SLEEP_MAX_NS : ns);
    ts.tv_sec = (time_t)(end / 1000000000ull);
    ts.tv_nsec = (long)(end % 1000000000ull);
    while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, NULL) == EINTR) {}
}
#endif

#endif

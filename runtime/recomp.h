/* Runtime contract for statically recompiled AArch64 code.
 *
 * Guest data addresses are host addresses offset by RT_MEMBASE (modules are mapped at their
 * guest base, that far up), so memory accesses are plain dereferences. Guest code addresses
 * are only ever looked up (rt_lookup) - recompiled functions are ordinary C functions taking
 * the CPU context. */
#pragma once
#ifdef _WIN32
#include <excpt.h>
#endif
#include <math.h>
#include <setjmp.h>
#include <stdint.h>
#include <string.h>

#if defined(_WIN32) && !defined(__FMA__) && !defined(__cplusplus)
/* x86-64-v2 on Windows: fma() and fmaf() are the C runtime's. On a processor without FMA3 its
 * software fma() returns a * b + c when c is a zero, so a product too small for a double (-2^-1100)
 * plus +0 gives +0, not -0 (tests/test_isa.py, test_software_fma). For a zero c the fused result is
 * the product rounded once, or for a zero product the sum of the two zeros, so that is computed here
 * instead; every other c goes to the C runtime. Windows' avx2 build has the instruction. */
static inline double rt_fma_v2(double a, double b, double c) {
    return c != 0 ? (fma)(a, b, c) : a == 0 || b == 0 ? a * b + c : a * b;
}
static inline float rt_fmaf_v2(float a, float b, float c) {
    return c != 0 ? (fmaf)(a, b, c) : a == 0 || b == 0 ? a * b + c : a * b;
}
#define fma(a, b, c) rt_fma_v2(a, b, c)
#define fmaf(a, b, c) rt_fmaf_v2(a, b, c)
#endif

/* Linux and macOS builds compile the generated files that have exception landing sites as
 * C++ (see rt_eh_filter below); everything here keeps C linkage either way. */
#ifdef __cplusplus
#define _Alignas(n) alignas(n)
#define _Noreturn __attribute__((noreturn))
extern "C" {
#endif

typedef uint8_t u8;   typedef uint16_t u16; typedef uint32_t u32; typedef uint64_t u64;
typedef int8_t i8;    typedef int16_t i16;  typedef int32_t i32;  typedef int64_t i64;
typedef float f32;    typedef double f64;

typedef union V128 {
    u8 b[16]; u16 h[8]; u32 s[4]; u64 d[2];
    i8 ib[16]; i16 ih[8]; i32 is[4]; i64 id[2];
    f32 f[4]; f64 g[2];
} V128;

typedef struct Cpu {
    u64 x[31];
    u64 sp;
    _Alignas(16) V128 v[32];
    u32 fpcr, fpsr;
    u64 tpidr;     /* TPIDR_EL0: free for the guest's use */
    u64 tpidrro;   /* TPIDRRO_EL0: address of the thread's thread-local region */
    void *thread;  /* the native kernel's object for the thread running on this context */
} Cpu;

typedef void GuestFn(Cpu *);

GuestFn *rt_lookup(u64 guest_addr);
/* A branch site's memory of its last target, which saves the lookup while the target stays
 * the same. The key is the address mixed with the function, so an entry another thread is
 * halfway through writing never matches. */
typedef struct RtCache { volatile u64 key; GuestFn *volatile fn; } RtCache;
#define RT_CACHE_MIX(fn) ((u64)(fn) * 0x9E3779B97F4A7C15ull)
GuestFn *rt_lookup_cached(u64 guest_addr, RtCache *cache);   /* rt_lookup, remembering the result when it may be */
static inline GuestFn *rt_target(u64 guest_addr, RtCache *cache) {
    GuestFn *fn = cache->fn;
    return (cache->key ^ RT_CACHE_MIX(fn)) == guest_addr ? fn : rt_lookup_cached(guest_addr, cache);
}
jmp_buf *rt_setjmp(Cpu *c);   /* a guest setjmp(X0): its host buffer, the guest registers saved */
/* The host setjmp a guest setjmp becomes (tools/lift.py) and the longjmp back to it
 * (hle_setjmp.c). macOS's pair saves and restores the signal mask, a system call each (a
 * hundred times the cost, and every Lua pcall is a setjmp): _setjmp/_longjmp leave it be,
 * as glibc's setjmp does. Always used as a pair. */
#ifdef __APPLE__
#define RT_SETJMP(env) _setjmp(env)
#define RT_LONGJMP(env, value) _longjmp(env, value)
#else
#define RT_SETJMP(env) setjmp(env)
#define RT_LONGJMP(env, value) longjmp(env, value)
#endif
/* C++ exceptions: a call that may unwind into landing pad `lp` of its function catches the
 * host exception rt_unwind_jumpto raises for that pad and this frame's stack pointer. */
#ifdef _WIN32
struct _EXCEPTION_POINTERS;
int rt_eh_filter(struct _EXCEPTION_POINTERS *e, u64 sp, u64 lp);
#ifndef GetExceptionInformation
#define GetExceptionInformation() ((struct _EXCEPTION_POINTERS *)_exception_info())
#endif
#else
/* Linux and macOS have no structured exception handling in C: the landing sites' __try /
 * __except become a C++ try and a catch of the exception rt_unwind_jumpto throws
 * (runtime/linux/eh.cpp), which goes on unwinding unless the filter matches this frame - the
 * same test, the same result. */
typedef struct RtEhSignal { u64 lp, sp; } RtEhSignal;
int rt_eh_filter(const RtEhSignal *e, u64 sp, u64 lp);
#ifdef __cplusplus
#define __try try
#define __except(filter) catch (const RtEhSignal &rt_e_) { if (!(filter)) throw; lp_ = 1; } if (0)
#define GetExceptionInformation() (&rt_e_)
#endif
#endif
_Noreturn void rt_trap(Cpu *c, u64 pc, const char *why);
void rt_svc(Cpu *c, u32 number);   /* a guest system call */
u64 rt_ticks(void);                /* the 19.2 MHz system counter */

#define X(n) (c->x[n])
#define SP (c->sp)
#define RS(n) (c->v[n].f[0])
#define RD(n) (c->v[n].g[0])
#define TAIL(f) { __attribute__((musttail)) return f(c); }

/* ---- memory -------------------------------------------------------------------- */
/* Guest address a is host address a + RT_MEMBASE. Windows and Linux map the guest at its own
 * addresses; on macOS the system holds that range (a kernel guard below 0x7000000000, malloc's
 * and Metal's reservations above), so there the guest lives 1 TiB up - the layout it sees is
 * the same everywhere. (Macros, which elsewhere add nothing: the files built at -O0 compile as
 * before.) */
#ifdef __APPLE__
#define RT_MEMBASE 0x10000000000ull
#define RT_HOST(a) ((void *)(uintptr_t)((u64)(a) + RT_MEMBASE))
#else
#define RT_MEMBASE 0ull
#define RT_HOST(a) ((void *)(uintptr_t)(a))
#endif

/* lda: a load-acquire (ldar, ldaxr). AArch64's never moves ahead of an earlier store-release
 * (stlr: sta) either - which C's seq_cst load promises and its acquire does not (arm64
 * compilers make an acquire the weaker ldapr). On x86 both are a plain load. */
#define RT_MEM(T, N) \
    static inline T ld##N(u64 a) { T v; memcpy(&v, RT_HOST(a), sizeof v); return v; } \
    static inline void st##N(u64 a, T v) { memcpy(RT_HOST(a), &v, sizeof v); } \
    static inline u32 cas##N(u64 a, u64 expect, u64 val) { \
        T e = (T)expect; \
        return __atomic_compare_exchange_n((T *)RT_HOST(a), &e, (T)val, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST); } \
    static inline void sta##N(u64 a, T v) { __atomic_store_n((T *)RT_HOST(a), v, __ATOMIC_SEQ_CST); } \
    static inline T lda##N(u64 a) { return __atomic_load_n((T *)RT_HOST(a), __ATOMIC_SEQ_CST); }
RT_MEM(u8, 8) RT_MEM(u16, 16) RT_MEM(u32, 32) RT_MEM(u64, 64)

/* dc zva: zero the 64-byte block holding the address (DCZID_EL0 reads 4: tools/lift.py). */
static inline void rt_zva(u64 a) { memset(RT_HOST(a & ~63ull), 0, 64); }

static inline void ldv8(Cpu *c, int n, u64 a)   { c->v[n].d[0] = ld8(a);  c->v[n].d[1] = 0; }
static inline void ldv16(Cpu *c, int n, u64 a)  { c->v[n].d[0] = ld16(a); c->v[n].d[1] = 0; }
static inline void ldv32(Cpu *c, int n, u64 a)  { c->v[n].d[0] = ld32(a); c->v[n].d[1] = 0; }
static inline void ldv64(Cpu *c, int n, u64 a)  { c->v[n].d[0] = ld64(a); c->v[n].d[1] = 0; }
static inline void ldv128(Cpu *c, int n, u64 a) { memcpy(&c->v[n], RT_HOST(a), 16); }
static inline void stv8(Cpu *c, int n, u64 a)   { st8(a, c->v[n].b[0]); }
static inline void stv16(Cpu *c, int n, u64 a)  { st16(a, c->v[n].h[0]); }
static inline void stv32(Cpu *c, int n, u64 a)  { st32(a, c->v[n].s[0]); }
static inline void stv64(Cpu *c, int n, u64 a)  { st64(a, c->v[n].d[0]); }
static inline void stv128(Cpu *c, int n, u64 a) { memcpy(RT_HOST(a), &c->v[n], 16); }

/* Scalar FP writes clear the rest of the vector register, as on AArch64. */
static inline void wS(Cpu *c, int n, f32 v) { u32 b; memcpy(&b, &v, 4); c->v[n].d[0] = b; c->v[n].d[1] = 0; }
static inline void wD(Cpu *c, int n, f64 v) { c->v[n].g[0] = v; c->v[n].d[1] = 0; }

/* ---- integer helpers ----------------------------------------------------------- */
static inline u32 ror32(u32 v, u32 n) { n &= 31; return n ? (v >> n) | (v << (32 - n)) : v; }
static inline u64 ror64(u64 v, u32 n) { n &= 63; return n ? (v >> n) | (v << (64 - n)) : v; }
static inline u32 udiv32(u32 a, u32 b) { return b ? a / b : 0; }
static inline u64 udiv64(u64 a, u64 b) { return b ? a / b : 0; }
static inline u32 sdiv32(u32 a, u32 b) {
    if (!b) return 0;
    if (a == 0x80000000u && b == 0xFFFFFFFFu) return a;
    return (u32)((i32)a / (i32)b);
}
static inline u64 sdiv64(u64 a, u64 b) {
    if (!b) return 0;
    if (a == 0x8000000000000000ull && b == ~0ull) return a;
    return (u64)((i64)a / (i64)b);
}
static inline u32 clz32(u32 v) { return v ? (u32)__builtin_clz(v) : 32; }
static inline u64 clz64(u64 v) { return v ? (u64)__builtin_clzll(v) : 64; }
static inline u32 cls32(u32 v) { return clz32(v ^ (u32)((i32)v >> 1)) - 1; }
static inline u64 cls64(u64 v) { return clz64(v ^ (u64)((i64)v >> 1)) - 1; }
static inline u32 rev16_32(u32 v) { return ((v & 0xFF00FF00u) >> 8) | ((v & 0x00FF00FFu) << 8); }
static inline u64 rev16_64(u64 v) { return ((v & 0xFF00FF00FF00FF00ull) >> 8) | ((v & 0x00FF00FF00FF00FFull) << 8); }
static inline u64 rev32_64(u64 v) { return ((u64)__builtin_bswap32((u32)(v >> 32)) << 32) | __builtin_bswap32((u32)v); }
static inline u64 umulh64(u64 a, u64 b) { return (u64)(((unsigned __int128)a * b) >> 64); }
static inline u64 smulh64(u64 a, u64 b) { return (u64)(((__int128)(i64)a * (i64)b) >> 64); }

/* crc32b/h/w/x and crc32cb/ch/cw/cx: the reflected CRC of the low 8, 16, 32 or 64 bits of v,
 * least significant bit first, continuing acc, with no inversion before or after. The bitwise
 * form is the definition; the host's instruction where it has one (x86's crc32 is CRC-32C only),
 * else for CRC-32 tables made from the bitwise form. */
#define RT_CRC32_POLY 0xEDB88320u    /* 0x04C11DB7, reflected */
#define RT_CRC32C_POLY 0x82F63B78u   /* 0x1EDC6F41 (Castagnoli), reflected */
static inline u32 rt_crc32_bits(u32 acc, u64 v, int bits, u32 poly) {
    u64 x = acc ^ v;
    for (int i = 0; i < bits; i++) x = (x >> 1) ^ (poly & (0u - (u32)(x & 1)));
    return (u32)x;
}
#if defined(__ARM_FEATURE_CRC32)
static inline u32 rt_crc32b(u32 acc, u8 v)  { return __builtin_arm_crc32b(acc, v); }
static inline u32 rt_crc32h(u32 acc, u16 v) { return __builtin_arm_crc32h(acc, v); }
static inline u32 rt_crc32w(u32 acc, u32 v) { return __builtin_arm_crc32w(acc, v); }
static inline u32 rt_crc32x(u32 acc, u64 v) { return __builtin_arm_crc32d(acc, v); }
static inline u32 rt_crc32cb(u32 acc, u8 v)  { return __builtin_arm_crc32cb(acc, v); }
static inline u32 rt_crc32ch(u32 acc, u16 v) { return __builtin_arm_crc32ch(acc, v); }
static inline u32 rt_crc32cw(u32 acc, u32 v) { return __builtin_arm_crc32cw(acc, v); }
static inline u32 rt_crc32cx(u32 acc, u64 v) { return __builtin_arm_crc32cd(acc, v); }
#else
/* CRC-32 by slicing: t[256 * k + b] is the CRC of byte b followed by k zero bytes. The tables are
 * built on first use by one thread (the others take the bitwise form meanwhile): 8 KB in each
 * file that uses them. (The building is out of line, not in every crc32 the lifter writes.) */
static inline __attribute__((noinline)) void rt_crc32_build(u32 *t) {
    for (int i = 0; i < 8 * 256; i++) t[i] = rt_crc32_bits(0, (u64)(i & 255), 8 + 8 * (i >> 8), RT_CRC32_POLY);
}
static inline const u32 *rt_crc32_tables(void) {
    static u32 t[8 * 256];
    static int state;   /* 0 not built, 1 being built, 2 built */
    int s = __atomic_load_n(&state, __ATOMIC_ACQUIRE);
    if (s == 2) return t;
    if (s || !__atomic_compare_exchange_n(&state, &s, 1, 0, __ATOMIC_RELAXED, __ATOMIC_RELAXED)) return 0;
    rt_crc32_build(t);
    __atomic_store_n(&state, 2, __ATOMIC_RELEASE);
    return t;
}
static inline u32 rt_crc32_sliced(u32 acc, u64 v, int bytes) {
    const u32 *t = rt_crc32_tables();
    if (!t) return rt_crc32_bits(acc, v, 8 * bytes, RT_CRC32_POLY);
    u64 x = acc ^ v;
    u32 r = bytes < 4 ? (u32)(x >> 8 * bytes) : 0;   /* (the bits of acc above v's, shifted down) */
    for (int i = 0; i < bytes; i++) r ^= t[256 * (bytes - 1 - i) + (x >> 8 * i & 255)];
    return r;
}
static inline u32 rt_crc32b(u32 acc, u8 v)  { return rt_crc32_sliced(acc, v, 1); }
static inline u32 rt_crc32h(u32 acc, u16 v) { return rt_crc32_sliced(acc, v, 2); }
static inline u32 rt_crc32w(u32 acc, u32 v) { return rt_crc32_sliced(acc, v, 4); }
static inline u32 rt_crc32x(u32 acc, u64 v) { return rt_crc32_sliced(acc, v, 8); }
#if defined(__SSE4_2__)
static inline u32 rt_crc32cb(u32 acc, u8 v)  { return __builtin_ia32_crc32qi(acc, v); }
static inline u32 rt_crc32ch(u32 acc, u16 v) { return __builtin_ia32_crc32hi(acc, v); }
static inline u32 rt_crc32cw(u32 acc, u32 v) { return __builtin_ia32_crc32si(acc, v); }
static inline u32 rt_crc32cx(u32 acc, u64 v) { return (u32)__builtin_ia32_crc32di(acc, v); }
#else
static inline u32 rt_crc32cb(u32 acc, u8 v)  { return rt_crc32_bits(acc, v, 8, RT_CRC32C_POLY); }
static inline u32 rt_crc32ch(u32 acc, u16 v) { return rt_crc32_bits(acc, v, 16, RT_CRC32C_POLY); }
static inline u32 rt_crc32cw(u32 acc, u32 v) { return rt_crc32_bits(acc, v, 32, RT_CRC32C_POLY); }
static inline u32 rt_crc32cx(u32 acc, u64 v) { return rt_crc32_bits(acc, v, 64, RT_CRC32C_POLY); }
#endif
#endif

/* ---- floating point: AArch64 semantics where they differ from x86 -------------- */
static inline i32 f2i32(f64 v) { return v != v ? 0 : v >= 2147483648.0 ? INT32_MAX : v <= -2147483649.0 ? INT32_MIN : (i32)v; }
static inline u32 f2u32(f64 v) { return v != v ? 0 : v >= 4294967296.0 ? UINT32_MAX : v <= -1.0 ? 0 : (u32)v; }
static inline i64 f2i64(f64 v) { return v != v ? 0 : v >= 9223372036854775808.0 ? INT64_MAX : v < -9223372036854775808.0 ? INT64_MIN : (i64)v; }
static inline u64 f2u64(f64 v) { return v != v ? 0 : v >= 18446744073709551616.0 ? UINT64_MAX : v <= -1.0 ? 0 : (u64)v; }
static inline f32 fmaxS(f32 a, f32 b) { return a != a || b != b ? NAN : a > b ? a : b > a ? b : signbit(a) ? b : a; }
static inline f32 fminS(f32 a, f32 b) { return a != a || b != b ? NAN : a < b ? a : b < a ? b : signbit(a) ? a : b; }
static inline f64 fmaxD(f64 a, f64 b) { return a != a || b != b ? NAN : a > b ? a : b > a ? b : signbit(a) ? b : a; }
static inline f64 fminD(f64 a, f64 b) { return a != a || b != b ? NAN : a < b ? a : b < a ? b : signbit(a) ? a : b; }
static inline f32 fmaxnmS(f32 a, f32 b) { return a != a ? b : b != b ? a : fmaxS(a, b); }
static inline f32 fminnmS(f32 a, f32 b) { return a != a ? b : b != b ? a : fminS(a, b); }
static inline f64 fmaxnmD(f64 a, f64 b) { return a != a ? b : b != b ? a : fmaxD(a, b); }
static inline f64 fminnmD(f64 a, f64 b) { return a != a ? b : b != b ? a : fminD(a, b); }

/* fmulx / Newton-Raphson step helpers: fused, and inf * 0 has a defined result. */
#define RT_INF_ZERO(a, b) ((isinf(a) && (b) == 0) || ((a) == 0 && isinf(b)))
static inline f32 rt_fmulx32(f32 a, f32 b) { return RT_INF_ZERO(a, b) ? (signbit(a) != signbit(b) ? -2.0f : 2.0f) : a * b; }
static inline f64 rt_fmulx64(f64 a, f64 b) { return RT_INF_ZERO(a, b) ? (signbit(a) != signbit(b) ? -2.0 : 2.0) : a * b; }
static inline f32 rt_frecps32(f32 a, f32 b) { return RT_INF_ZERO(a, b) ? 2.0f : fmaf(-a, b, 2.0f); }
static inline f64 rt_frecps64(f64 a, f64 b) { return RT_INF_ZERO(a, b) ? 2.0 : fma(-a, b, 2.0); }
/* (3 - a*b) / 2 with one rounding: halve whichever operand is larger, exactly, then fuse. */
static inline f32 rt_frsqrts32(f32 a, f32 b) {
    if (RT_INF_ZERO(a, b)) return 1.5f;
    return fabsf(a) >= fabsf(b) ? fmaf(-(a * 0.5f), b, 1.5f) : fmaf(-a, b * 0.5f, 1.5f);
}
static inline f64 rt_frsqrts64(f64 a, f64 b) {
    if (RT_INF_ZERO(a, b)) return 1.5;
    return fabs(a) >= fabs(b) ? fma(-(a * 0.5), b, 1.5) : fma(-a, b * 0.5, 1.5);
}

/* Reciprocal and reciprocal-square-root estimates: the architecture's table algorithm,
 * which x86's rcpps / rsqrtps do not reproduce. */
static inline u32 rt_recip_est(u32 a) { a = a * 2 + 1; return ((1u << 19) / a + 1) / 2; }
/* (The manual's loop - b = 512; while (a * (b + 1)^2 < 2^28) b++; return (b + 1) / 2 - takes up
 * to 500 steps: k = b + 1 is found from sqrt and corrected exactly instead, the same for every a,
 * which tests/test_recomp_helpers.py checks.) */
static inline u32 rt_rsqrt_est(u32 a) {
    a = a < 256 ? a * 2 + 1 : ((a & ~1u) + 1) * 2;
    u32 k = (u32)sqrt((f64)(1u << 28) / a);
    if (k < 513) k = 513;
    while (k > 513 && (u64)a * (k - 1) * (k - 1) >= (1ull << 28)) k--;
    while ((u64)a * k * k < (1ull << 28)) k++;
    return k / 2;
}
#define RT_FRAC52 0xFFFFFFFFFFFFFull
static inline u64 rt_frecpe_frac(u64 frac, int *exp, int top) {
    if (!*exp) {
        if (!(frac >> 51 & 1)) { *exp = -1; frac = (frac << 2) & RT_FRAC52; }
        else frac = (frac << 1) & RT_FRAC52;
    }
    u64 r = (u64)(rt_recip_est(0x100 | (u32)(frac >> 44)) & 0xFF) << 44;
    *exp = top - *exp;
    if (*exp == 0) r = (1ull << 51) | r >> 1;
    else if (*exp == -1) { r = (1ull << 50) | r >> 2; *exp = 0; }
    return r;
}
static inline f32 rt_frecpe32(f32 x) {
    u32 v, out; memcpy(&v, &x, 4);
    u32 sign = v & 0x80000000u, mant = v & 0x7FFFFFu; int exp = (v >> 23) & 0xFF;
    if (exp == 0xFF) out = mant ? (v | 0x400000u) : sign;
    else if (!exp && mant < (1u << 21)) out = sign | 0x7F800000u;   /* zero, or too small: infinity */
    else { u64 r = rt_frecpe_frac((u64)mant << 29, &exp, 253); out = sign | (u32)exp << 23 | (u32)(r >> 29); }
    f32 f; memcpy(&f, &out, 4); return f;
}
static inline f64 rt_frecpe64(f64 x) {
    u64 v, out; memcpy(&v, &x, 8);
    u64 sign = v & (1ull << 63), mant = v & RT_FRAC52; int exp = (v >> 52) & 0x7FF;
    if (exp == 0x7FF) out = mant ? (v | 1ull << 51) : sign;
    else if (!exp && mant < (1ull << 50)) out = sign | 0x7FF0000000000000ull;
    else { u64 r = rt_frecpe_frac(mant, &exp, 2045); out = sign | (u64)exp << 52 | r; }
    f64 f; memcpy(&f, &out, 8); return f;
}
static inline u32 rt_frsqrte_est(u64 frac, int *exp, int top) {
    if (!*exp) {
        while (!(frac >> 51 & 1)) { frac = (frac << 1) & RT_FRAC52; --*exp; }
        frac = (frac << 1) & RT_FRAC52;
    }
    u32 scaled = (*exp & 1) ? (0x80 | (u32)(frac >> 45)) : (0x100 | (u32)(frac >> 44));
    *exp = (top - *exp) / 2;
    return rt_rsqrt_est(scaled) & 0xFF;
}
static inline f32 rt_frsqrte32(f32 x) {
    u32 v, out; memcpy(&v, &x, 4);
    u32 sign = v & 0x80000000u, mant = v & 0x7FFFFFu; int exp = (v >> 23) & 0xFF;
    if (exp == 0xFF && mant) out = v | 0x400000u;
    else if (!exp && !mant) out = sign | 0x7F800000u;
    else if (sign) out = 0x7FC00000u;
    else if (exp == 0xFF) out = 0;
    else { u32 e = rt_frsqrte_est((u64)mant << 29, &exp, 380); out = (u32)exp << 23 | e << 15; }
    f32 f; memcpy(&f, &out, 4); return f;
}
static inline f64 rt_frsqrte64(f64 x) {
    u64 v, out; memcpy(&v, &x, 8);
    u64 sign = v & (1ull << 63), mant = v & RT_FRAC52; int exp = (v >> 52) & 0x7FF;
    if (exp == 0x7FF && mant) out = v | 1ull << 51;
    else if (!exp && !mant) out = sign | 0x7FF0000000000000ull;
    else if (sign) out = 0x7FF8000000000000ull;
    else if (exp == 0x7FF) out = 0;
    else { u64 e = rt_frsqrte_est(mant, &exp, 3068); out = (u64)exp << 52 | e << 44; }
    f64 f; memcpy(&f, &out, 8); return f;
}

/* Half precision <-> wider formats (round to nearest even, single rounding). */
static inline f32 rt_h2f(u16 h) {
    u32 sign = (u32)(h >> 15) << 31, exp = (h >> 10) & 0x1F, man = h & 0x3FF, out;
    if (exp == 31) out = sign | 0x7F800000u | man << 13 | (man ? 0x400000u : 0);
    else if (exp) out = sign | (exp + 112) << 23 | man << 13;
    else if (!man) out = sign;
    else {
        int e = 113;
        do { man <<= 1; e--; } while (!(man & 0x400));
        out = sign | (u32)e << 23 | (man & 0x3FF) << 13;
    }
    f32 f; memcpy(&f, &out, 4); return f;
}
static inline u16 rt_d2h(f64 d) {
    u64 v; memcpy(&v, &d, 8);
    u16 sign = (u16)((v >> 48) & 0x8000);
    int exp = (int)((v >> 52) & 0x7FF);
    u64 man = v & RT_FRAC52;
    if (exp == 0x7FF) return sign | 0x7C00 | (man ? 0x200 | (u16)(man >> 42) : 0);
    if (!exp) return sign;
    int e = exp - 1023;
    if (e > 15) return sign | 0x7C00;
    int shift = e >= -14 ? 42 : 42 + (-14 - e);
    if (shift > 54) return sign;
    man |= 1ull << 52;
    u64 q = man >> shift, rem = man & ((1ull << shift) - 1), half = 1ull << (shift - 1);
    if (rem > half || (rem == half && (q & 1))) q++;
    return sign | (u16)(e >= -14 ? ((u64)(e + 15) << 10) + (q - 0x400) : q);
}

/* ---- AdvSIMD helpers ----------------------------------------------------------- */
/* Variable shifts: the count is a signed byte; negative counts shift right. */
static inline u64 rt_ushl(u64 a, i8 sh, int esize) {
    if (sh >= esize || sh <= -esize) return 0;
    return sh >= 0 ? a << sh : a >> -sh;
}
static inline u64 rt_sshl(i64 a, i8 sh, int esize) {
    if (sh >= esize) return 0;
    if (sh <= -esize) return a < 0 ? ~0ull : 0;
    return sh >= 0 ? (u64)a << sh : (u64)(a >> -sh);
}

static inline void rt_tbl(Cpu *c, int rd, int rn, int len, int rm, int q, int tbx) {
    u8 table[64];
    for (int i = 0; i < len; i++) memcpy(table + 16 * i, &c->v[(rn + i) & 31], 16);
    V128 idx = c->v[rm], r = c->v[rd];
    for (int i = 0; i < (8 << q); i++) {
        u8 k = idx.b[i];
        if (k < 16 * len) r.b[i] = table[k];
        else if (!tbx) r.b[i] = 0;
    }
    if (!q) r.d[1] = 0;
    c->v[rd] = r;
}

/* ld1-ld4 / st1-st4 over whole registers: `selem` registers are interleaved per element,
 * `rpt` repeats that for consecutive registers. */
static inline void rt_ldst_multi(Cpu *c, int load, int rt, int rpt, int selem, int ebytes, int q, u64 addr) {
    int elements = (8 << q) / ebytes;
    for (int r = 0; r < rpt; r++)
        for (int e = 0; e < elements; e++)
            for (int s = 0; s < selem; s++, addr += ebytes) {
                u8 *lane = c->v[(rt + r + s) & 31].b + e * ebytes;
                if (load) memcpy(lane, RT_HOST(addr), ebytes);
                else memcpy(RT_HOST(addr), lane, ebytes);
            }
    if (load && !q)
        for (int k = 0; k < rpt * selem; k++) c->v[(rt + k) & 31].d[1] = 0;
}

/* Single-lane structure access, and ld1r-ld4r (load one element and replicate it). */
static inline void rt_ldst_single(Cpu *c, int load, int replicate, int rt, int selem, int ebytes, int index,
                                  int q, u64 addr) {
    for (int s = 0; s < selem; s++, addr += ebytes) {
        V128 *v = &c->v[(rt + s) & 31];
        if (replicate) {
            V128 r = {0};
            for (int e = 0; e < (8 << q) / ebytes; e++) memcpy(r.b + e * ebytes, RT_HOST(addr), ebytes);
            *v = r;
        } else if (load) memcpy(v->b + index * ebytes, RT_HOST(addr), ebytes);
        else memcpy(RT_HOST(addr), v->b + index * ebytes, ebytes);
    }
}

#ifdef __cplusplus
}
#endif

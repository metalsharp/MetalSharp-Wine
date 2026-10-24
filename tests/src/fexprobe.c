/*
 * fexprobe: what translated x86 code costs under FEX, one measurement per cost, the same source built for
 * native ARM64 (reference), x86_64 (ARM64EC) and i386 (WoW64).
 *
 *   fexprobe.exe [test ...]       prints "name ns/op checksum" per test, best of 5 runs
 *
 * Game code is calls, returns, indirect branches over a large code footprint, flag-heavy integer code,
 * SSE math, small copies, atomics, calls into ntdll and, for Mono and other JITs, freshly written code.
 */
#include <windows.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>
#if defined(__x86_64__) || defined(__i386__)
#include <immintrin.h>
#endif

static double qpc_ns(void)
{
    static LARGE_INTEGER f;
    LARGE_INTEGER c;
    if (!f.QuadPart) QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart * 1e9 / (double)f.QuadPart;
}

#define NOINLINE __attribute__((noinline))
static volatile uint32_t sink32;
static volatile uint64_t sink64;

/* ---- control flow ---- */

NOINLINE static uint32_t leaf(uint32_t x) { return x * 2654435761u + 1; }
NOINLINE static uint32_t mid(uint32_t x) { return leaf(x) ^ leaf(x + 1); }
NOINLINE static uint32_t top(uint32_t x) { return mid(x) + mid(x ^ 7); }

static uint64_t t_calls(unsigned n)          /* 7 calls and returns per op */
{
    uint32_t a = 1;
    for (unsigned i = 0; i < n; i++) a = top(a + i);
    return a;
}

NOINLINE static uint32_t fib(uint32_t n) { return n < 2 ? n : fib(n - 1) + fib(n - 2); }
static uint64_t t_recursion(unsigned n)       /* fib(20): 21891 calls per op */
{
    uint64_t s = 0;
    for (unsigned i = 0; i < n; i++) s += fib(20 + (i & 0));
    return s;
}

/* Indirect calls through a table of distinct functions: virtual dispatch. */
#define F(k) NOINLINE static uint32_t fn##k(uint32_t x) { return x * (2 * k + 1) + k; }
#define F10(d) F(d##0) F(d##1) F(d##2) F(d##3) F(d##4) F(d##5) F(d##6) F(d##7) F(d##8) F(d##9)
F10(1) F10(2) F10(3) F10(4) F10(5) F10(6)
#define R10(d) fn##d##0, fn##d##1, fn##d##2, fn##d##3, fn##d##4, fn##d##5, fn##d##6, fn##d##7, fn##d##8, fn##d##9,
static uint32_t (*const table[64])(uint32_t) = { R10(1) R10(2) R10(3) R10(4) R10(5) R10(6) fn10, fn11, fn12, fn13 };

static uint64_t t_indirect_mono(unsigned n)   /* one target: predictable */
{
    uint32_t a = 3;
    uint32_t (*volatile f)(uint32_t) = table[5];
    for (unsigned i = 0; i < n; i++) a = f(a);
    return a;
}

static uint64_t t_indirect_poly(unsigned n)   /* 64 targets in rotation */
{
    uint32_t a = 3;
    for (unsigned i = 0; i < n; i++) a = table[(a ^ i) & 63](a);
    return a;
}

static uint64_t t_switch(unsigned n)          /* dense jump table */
{
    uint32_t a = 1, s = 0;
    for (unsigned i = 0; i < n; i++)
    {
        a = a * 1103515245u + 12345u;
        switch ((a >> 16) & 15)
        {
        case 0: s += a; break;        case 1: s ^= a; break;        case 2: s -= a >> 3; break;
        case 3: s += a << 2; break;   case 4: s *= 3; break;        case 5: s ^= s >> 7; break;
        case 6: s += 17; break;       case 7: s -= 5; break;        case 8: s = ~s; break;
        case 9: s += i; break;        case 10: s ^= i << 3; break;  case 11: s += a ^ i; break;
        case 12: s = s * 7 + 1; break; case 13: s -= i; break;      case 14: s ^= 0x5555; break;
        default: s += 1; break;
        }
    }
    return s;
}

/* Many distinct hot functions, called in rotation: a large code footprint. */
#define G(k) NOINLINE static uint32_t g##k(uint32_t x) { uint32_t y = x; for (int j = 0; j < 3; j++) y = (y ^ (k * 0x9e3779b9u)) * 31 + j; return y; }
#define G10(d) G(d##0) G(d##1) G(d##2) G(d##3) G(d##4) G(d##5) G(d##6) G(d##7) G(d##8) G(d##9)
#define G100(h) G10(h##0) G10(h##1) G10(h##2) G10(h##3) G10(h##4) G10(h##5) G10(h##6) G10(h##7) G10(h##8) G10(h##9)
G100(1) G100(2) G100(3) G100(4)
#define GR10(d) g##d##0, g##d##1, g##d##2, g##d##3, g##d##4, g##d##5, g##d##6, g##d##7, g##d##8, g##d##9,
#define GR100(h) GR10(h##0) GR10(h##1) GR10(h##2) GR10(h##3) GR10(h##4) GR10(h##5) GR10(h##6) GR10(h##7) GR10(h##8) GR10(h##9)
static uint32_t (*const gtable[400])(uint32_t) = { GR100(1) GR100(2) GR100(3) GR100(4) };

static uint64_t t_footprint(unsigned n)       /* 400 functions, each called in turn */
{
    uint32_t a = 1;
    for (unsigned i = 0; i < n; i++) a = gtable[i % 400](a);
    return a;
}

/* ---- data ---- */

static uint64_t t_flags(unsigned n)           /* 128-bit adds, compares, conditional moves */
{
    uint64_t lo = 1, hi = 0, m = 0;
    for (unsigned i = 0; i < n; i++)
    {
        uint64_t x = lo * 0x9e3779b97f4a7c15ull + i;
        uint64_t old = lo;
        lo += x;
        hi += (lo < old) + (x >> 63);
        m = (lo > hi) ? m + lo : m ^ hi;
    }
    return lo ^ hi ^ m;
}

static uint64_t t_hash(unsigned n)            /* byte loop over 64 bytes per op (FNV-1a) */
{
    static uint8_t buf[64];
    uint32_t h = 2166136261u;
    for (int i = 0; i < 64; i++) buf[i] = (uint8_t)(i * 7);
    for (unsigned i = 0; i < n; i++)
    {
        buf[i & 63] ^= (uint8_t)i;
        for (int j = 0; j < 64; j++) h = (h ^ buf[j]) * 16777619u;
    }
    return h;
}

static uint64_t t_sse_math(unsigned n)        /* float vec4 transform, 16 mul-adds per op */
{
    float m[16], v[4] = { 1, 2, 3, 4 }, r[4];
    for (int i = 0; i < 16; i++) m[i] = 0.25f + i * 0.01f;
    for (unsigned i = 0; i < n; i++)
    {
        for (int k = 0; k < 4; k++) r[k] = m[k] * v[0] + m[4 + k] * v[1] + m[8 + k] * v[2] + m[12 + k] * v[3];
        for (int k = 0; k < 4; k++) v[k] = r[k] * 0.999f + 0.001f;
    }
    return (uint64_t)(v[0] * 1000);
}

/* FP modes the AFP bits implement: MXCSR rounding, DAZ/FTZ, x86 min/max NaN order and scalar ops that keep the
 * upper lanes. The checksum must match between FEX builds (it differs from native arm64 by design). */
static uint64_t t_fpmodes(unsigned n)
{
    uint64_t h = 1469598103934665603ull;
#if defined(__x86_64__) || defined(__i386__)
    static const unsigned modes[] = { 0x1f80, 0x3f80, 0x5f80, 0x7f80, 0x1fc0, 0x9f80, 0x9fc0 };
    unsigned csr = _mm_getcsr();
    volatile float tiny = 1e-39f, big = 3e38f, qnan = NAN;
    for (unsigned i = 0; i < n; i++)
    {
        _mm_setcsr(modes[i % 7]);
        __m128 a = _mm_set_ps(1.5f, -2.5f, tiny, (float)i + 0.5f);
        __m128 b = _mm_set_ps(qnan, 0.75f, tiny, 3.25f);
        __m128 r = _mm_add_ss(a, b);                 /* upper lanes from a */
        r = _mm_mul_ps(r, _mm_set1_ps(big));
        r = _mm_min_ps(r, b);                        /* x86: NaN in either -> second operand */
        r = _mm_max_ss(r, _mm_set_ss(qnan));
        __m128d d = _mm_sqrt_sd(_mm_set_pd(9.0, 2.0), _mm_set_sd((double)tiny * (i + 1)));
        int c = _mm_cvtss_si32(_mm_set_ss((float)i * 0.5f + 0.5f));      /* rounds by MXCSR */
        uint32_t lanes[4]; uint64_t dl[2];
        _mm_storeu_ps((float *)lanes, r); _mm_storeu_pd((double *)dl, d);
        uint64_t v[] = { lanes[0], lanes[1], lanes[2], lanes[3], dl[0], dl[1], (uint32_t)c };
        for (int k = 0; k < 7; k++) h = (h ^ v[k]) * 1099511628211ull;
    }
    _mm_setcsr(csr);
#else
    (void)n;
#endif
    return h;
}

static uint64_t t_double_math(unsigned n)     /* sqrt, div, transcendental-free doubles */
{
    double s = 0, x = 1.5;
    for (unsigned i = 0; i < n; i++)
    {
        x = x * 1.0000001 + 0.5 / (x + 1.0);
        s += sqrt(x);
    }
    return (uint64_t)s;
}

static uint64_t t_memcpy_small(unsigned n)    /* 64-byte copies */
{
    static uint8_t a[256], b[256];
    uint64_t s = 0;
    for (unsigned i = 0; i < n; i++)
    {
        memcpy(b + (i & 63), a + ((i * 7) & 63), 64);
        s += b[i & 127];
        a[i & 255] = (uint8_t)i;
    }
    return s;
}

static uint64_t t_memcpy_large(unsigned n)    /* 64 KiB copies */
{
    static uint8_t a[65536], b[65536];
    for (unsigned i = 0; i < n; i++) { a[i & 65535] = (uint8_t)i; memcpy(b, a, sizeof(a)); }
    return b[n & 65535];
}

static uint64_t t_sort(unsigned n)            /* qsort of 256 ints per op: calls back through a pointer */
{
    static int v[256];
    uint32_t r = 1;
    uint64_t s = 0;
    for (unsigned i = 0; i < n; i++)
    {
        for (int k = 0; k < 256; k++) { r = r * 1103515245u + 12345u; v[k] = (int)(r >> 8); }
        int cmp(const void *x, const void *y);
        qsort(v, 256, sizeof(int), cmp);
        s += (uint32_t)v[128];
    }
    return s;
}
int cmp(const void *x, const void *y) { int a = *(const int *)x, b = *(const int *)y; return (a > b) - (a < b); }

/* ---- synchronization and system ---- */

static uint64_t t_atomic(unsigned n)          /* uncontended locked add and compare-exchange */
{
    static volatile LONG v;
    for (unsigned i = 0; i < n; i++) { InterlockedIncrement(&v); InterlockedCompareExchange(&v, (LONG)i, (LONG)i - 1); }
    return (uint64_t)v;
}

static volatile LONG contended;
static volatile LONG stop_flag;
static DWORD WINAPI hammer(void *arg) { while (!stop_flag) InterlockedIncrement(&contended); return 0; }
static uint64_t t_atomic_contended(unsigned n)   /* the same cache line as another thread */
{
    HANDLE t = CreateThread(NULL, 0, hammer, NULL, 0, NULL);
    for (unsigned i = 0; i < n; i++) InterlockedIncrement(&contended);
    stop_flag = 1;
    WaitForSingleObject(t, INFINITE);
    CloseHandle(t);
    stop_flag = 0;
    return 1;
}

static uint64_t t_critsec(unsigned n)         /* uncontended critical section */
{
    static CRITICAL_SECTION cs;
    static int init;
    uint64_t s = 0;
    if (!init) { InitializeCriticalSection(&cs); init = 1; }
    for (unsigned i = 0; i < n; i++) { EnterCriticalSection(&cs); s += i; LeaveCriticalSection(&cs); }
    return s;
}

static uint64_t t_qpc(unsigned n)             /* QueryPerformanceCounter: an ntdll call */
{
    LARGE_INTEGER c;
    uint64_t s = 0;
    for (unsigned i = 0; i < n; i++) { QueryPerformanceCounter(&c); s += c.QuadPart & 1; }
    return s;
}

static uint64_t t_tls(unsigned n)             /* TlsGetValue: kernelbase */
{
    static DWORD idx = TLS_OUT_OF_INDEXES;
    uint64_t s = 0;
    if (idx == TLS_OUT_OF_INDEXES) { idx = TlsAlloc(); TlsSetValue(idx, (void *)42); }
    for (unsigned i = 0; i < n; i++) s += (uintptr_t)TlsGetValue(idx);
    return s;
}

static HANDLE ev_a, ev_b;
static DWORD WINAPI pong(void *arg)
{
    for (unsigned i = 0; i < (unsigned)(uintptr_t)arg; i++) { WaitForSingleObject(ev_a, INFINITE); SetEvent(ev_b); }
    return 0;
}
static uint64_t t_event_pingpong(unsigned n)  /* thread handoff through two events */
{
    ev_a = CreateEventW(NULL, FALSE, FALSE, NULL);
    ev_b = CreateEventW(NULL, FALSE, FALSE, NULL);
    HANDLE t = CreateThread(NULL, 0, pong, (void *)(uintptr_t)n, 0, NULL);
    for (unsigned i = 0; i < n; i++) { SetEvent(ev_a); WaitForSingleObject(ev_b, INFINITE); }
    WaitForSingleObject(t, INFINITE);
    CloseHandle(t); CloseHandle(ev_a); CloseHandle(ev_b);
    return n;
}

/* ---- generated code (Mono and other JITs) ---- */

#if defined(__x86_64__) || defined(__i386__)
/* mov eax, imm32; ret */
static void emit_ret_imm(uint8_t *p, uint32_t imm) { p[0] = 0xb8; memcpy(p + 1, &imm, 4); p[5] = 0xc3; }
#elif defined(__aarch64__)
static void emit_ret_imm(uint8_t *p, uint32_t imm)
{
    uint32_t i[3] = { 0x52800000u | ((imm & 0xffff) << 5), 0x72a00000u | ((imm >> 16) << 5), 0xd65f03c0u };  /* movz w0; movk w0, lsl 16; ret */
    memcpy(p, i, sizeof(i));
}
#endif
#define CODE_STUB 32

static uint8_t *jit_page(size_t size) { return VirtualAlloc(NULL, size, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE); }

static uint64_t t_jit_new(unsigned n)         /* write a new function and call it once: JIT throughput */
{
    size_t size = (size_t)n * CODE_STUB;
    uint8_t *code = jit_page(size);
    uint64_t s = 0;
    for (unsigned i = 0; i < n; i++)
    {
        uint8_t *p = code + (size_t)i * CODE_STUB;
        emit_ret_imm(p, i);
        FlushInstructionCache(GetCurrentProcess(), p, CODE_STUB);
        s += ((uint32_t (*)(void))p)();
    }
    VirtualFree(code, 0, MEM_RELEASE);
    return s;
}

static uint64_t t_jit_patch(unsigned n)       /* rewrite a function that runs: patching (Mono backpatches calls) */
{
    uint8_t *code = jit_page(4096);
    uint64_t s = 0;
    for (unsigned i = 0; i < n; i++)
    {
        emit_ret_imm(code, i);
        FlushInstructionCache(GetCurrentProcess(), code, CODE_STUB);
        s += ((uint32_t (*)(void))code)();
    }
    VirtualFree(code, 0, MEM_RELEASE);
    return s;
}

static uint64_t t_jit_neighbour(unsigned n)   /* write data next to code that runs: same page, no code change */
{
    uint8_t *code = jit_page(4096);
    uint64_t s = 0;
    emit_ret_imm(code, 7);
    FlushInstructionCache(GetCurrentProcess(), code, CODE_STUB);
    for (unsigned i = 0; i < n; i++)
    {
        code[2048 + (i & 255)] = (uint8_t)i;
        s += ((uint32_t (*)(void))code)();
    }
    VirtualFree(code, 0, MEM_RELEASE);
    return s;
}

/* ---- driver ---- */

struct test { const char *name; uint64_t (*fn)(unsigned); unsigned n; };
static const struct test tests[] =
{
    { "calls",            t_calls,            2000000 },
    { "recursion",        t_recursion,        20 },
    { "indirect_mono",    t_indirect_mono,    5000000 },
    { "indirect_poly",    t_indirect_poly,    5000000 },
    { "switch",           t_switch,           5000000 },
    { "footprint",        t_footprint,        2000000 },
    { "flags",            t_flags,            5000000 },
    { "hash",             t_hash,             100000 },
    { "sse_math",         t_sse_math,         2000000 },
    { "double_math",      t_double_math,      5000000 },
    { "fpmodes",          t_fpmodes,          200000 },
    { "memcpy_small",     t_memcpy_small,     5000000 },
    { "memcpy_large",     t_memcpy_large,     5000 },
    { "sort",             t_sort,             2000 },
    { "atomic",           t_atomic,           5000000 },
    { "atomic_contended", t_atomic_contended, 2000000 },
    { "critsec",          t_critsec,          5000000 },
    { "qpc",              t_qpc,              1000000 },
    { "tls",              t_tls,              5000000 },
    { "event_pingpong",   t_event_pingpong,   20000 },
    { "jit_new",          t_jit_new,          20000 },
    { "jit_patch",        t_jit_patch,        20000 },
    { "jit_neighbour",    t_jit_neighbour,    200000 },
};

int main(int argc, char **argv)
{
    /* FEXPROBE_SCALE multiplies every iteration count, so a test runs long enough to sample. */
    double scale = getenv("FEXPROBE_SCALE") ? atof(getenv("FEXPROBE_SCALE")) : 1.0;
    setvbuf(stdout, NULL, _IONBF, 0);
    for (size_t t = 0; t < sizeof(tests) / sizeof(tests[0]); t++)
    {
        int wanted = argc < 2;
        for (int a = 1; a < argc; a++) if (!strcmp(argv[a], tests[t].name)) wanted = 1;
        if (!wanted) continue;
        double best = 1e300;
        uint64_t sum = 0;
        for (int r = 0; r < 5; r++)
        {
            double t0 = qpc_ns();
            unsigned n = (unsigned)(tests[t].n * scale);
            sum = tests[t].fn(n);
            double dt = (qpc_ns() - t0) / n;
            if (dt < best) best = dt;
        }
        sink64 = sum;
        printf("%-18s %12.2f ns/op  %llu\n", tests[t].name, best, (unsigned long long)sum);
    }
    /* FEXPROBE_DUMP=<hex addr>:<hex len>:<file> writes that memory to a file at exit, such as the JIT code of a
     * hot loop (from a sample of this process) to disassemble. */
    if (getenv("FEXPROBE_DUMP"))
    {
        unsigned long long addr = 0, len = 0;
        char path[512];
        if (sscanf(getenv("FEXPROBE_DUMP"), "%llx:%llx:%511s", &addr, &len, path) == 3)
        {
            FILE *f = fopen(path, "wb");
            if (f) { fwrite((const void *)(uintptr_t)addr, 1, len, f); fclose(f); }
        }
    }
    return 0;
}

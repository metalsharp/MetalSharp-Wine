/* MetalSharp Wine benchmark: built as native ARM64, x86_64 (FEX/ARM64EC) and i386 (FEX/WoW64)
 * so the same workloads can be compared. Each test prints a checksum to catch miscompiles. */
#define _WIN32_WINNT 0x0A00
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdint.h>

static double now(void)
{
    LARGE_INTEGER f, c;
    QueryPerformanceFrequency( &f );
    QueryPerformanceCounter( &c );
    return (double)c.QuadPart / f.QuadPart;
}

/* integer: hashing, branches, divisions */
static uint64_t bench_int( unsigned n )
{
    uint64_t h = 1469598103934665603ull, acc = 0;
    for (unsigned i = 1; i <= n; i++)
    {
        h ^= i; h *= 1099511628211ull;
        if (h & 1) acc += h % (i | 1); else acc ^= h >> 7;
    }
    return acc ^ h;
}

/* floating point: SSE scalar + vectorizable loops, libm */
static double bench_fp( unsigned n )
{
    static float a[4096], b[4096];
    double s = 0;
    for (unsigned i = 0; i < 4096; i++) { a[i] = i * 0.5f; b[i] = 1.0f / (i + 1); }
    for (unsigned r = 0; r < n; r++)
    {
        for (unsigned i = 0; i < 4096; i++) a[i] = a[i] * 0.999f + b[i];
        s += sqrt( a[r & 4095] ) + sin( r * 0.001 );
    }
    return s;
}

/* memory: memcpy / memset bandwidth over 64MB */
static uint64_t bench_mem( unsigned n )
{
    size_t size = 64 << 20;
    char *src = malloc( size ), *dst = malloc( size );
    uint64_t sum = 0;
    memset( src, 0x5a, size );
    for (unsigned r = 0; r < n; r++)
    {
        memcpy( dst, src, size );
        __asm__ __volatile__( "" :: "r"(dst) : "memory" );  /* keep the copy observable */
        src[r % size] = (char)r;
        sum += dst[(r * 4099) % size];
    }
    __asm__ __volatile__( "" :: "r"(src), "r"(dst) : "memory" );
    free( src ); free( dst );
    return sum;
}

/* threads: contended atomics + shared counters (memory-model sensitive) */
static volatile LONG64 shared_counter;
static LONG64 per_thread[8 * 16];
static DWORD WINAPI atomic_thread( void *arg )
{
    unsigned idx = (unsigned)(ULONG_PTR)arg, n = 2000000;
    for (unsigned i = 0; i < n; i++)
    {
        InterlockedIncrement64( &shared_counter );
        per_thread[idx * 16]++;
    }
    return 0;
}
static uint64_t bench_atomic( unsigned threads )
{
    HANDLE h[8];
    shared_counter = 0;
    for (unsigned i = 0; i < threads; i++) h[i] = CreateThread( NULL, 0, atomic_thread, (void *)(ULONG_PTR)i, 0, NULL );
    WaitForMultipleObjects( threads, h, TRUE, INFINITE );
    for (unsigned i = 0; i < threads; i++) CloseHandle( h[i] );
    return shared_counter;
}

/* message passing between two threads: tests store->load ordering (x86 TSO semantics) */
static volatile LONG flag_a, flag_b;
static volatile LONG data_a, data_b;
static volatile LONG violations;
static DWORD WINAPI order_writer( void *arg )
{
    for (LONG i = 1; i <= 2000000; i++)
    {
        data_a = i;      /* plain stores: x86 guarantees they become visible in order */
        flag_a = i;
        while (flag_b != i) YieldProcessor();
    }
    return 0;
}
static DWORD WINAPI order_reader( void *arg )
{
    for (LONG i = 1; i <= 2000000; i++)
    {
        while (flag_a != i) YieldProcessor();
        if (data_a < i) InterlockedIncrement( &violations );  /* would mean store reordering */
        flag_b = i;
    }
    return 0;
}
static LONG bench_order(void)
{
    HANDLE h[2];
    violations = flag_a = flag_b = 0;
    h[0] = CreateThread( NULL, 0, order_writer, NULL, 0, NULL );
    h[1] = CreateThread( NULL, 0, order_reader, NULL, 0, NULL );
    WaitForMultipleObjects( 2, h, TRUE, INFINITE );
    CloseHandle( h[0] ); CloseHandle( h[1] );
    return violations;
}

/* JIT stress: many distinct functions, each executed a few times */
#define F(n) static unsigned __attribute__((noinline)) fn##n( unsigned x ) { return (x * (n + 3)) ^ (x >> ((n % 7) + 1)) ^ n; }
#define F10(n) F(n##0) F(n##1) F(n##2) F(n##3) F(n##4) F(n##5) F(n##6) F(n##7) F(n##8) F(n##9)
#define F100(n) F10(n##0) F10(n##1) F10(n##2) F10(n##3) F10(n##4) F10(n##5) F10(n##6) F10(n##7) F10(n##8) F10(n##9)
F100(1) F100(2) F100(3) F100(4) F100(5)
#define P(n) fn##n,
#define P10(n) P(n##0) P(n##1) P(n##2) P(n##3) P(n##4) P(n##5) P(n##6) P(n##7) P(n##8) P(n##9)
#define P100(n) P10(n##0) P10(n##1) P10(n##2) P10(n##3) P10(n##4) P10(n##5) P10(n##6) P10(n##7) P10(n##8) P10(n##9)
static unsigned (*const fns[])(unsigned) = { P100(1) P100(2) P100(3) P100(4) P100(5) };
static unsigned bench_jit(void)
{
    unsigned acc = 0;
    for (unsigned r = 0; r < 4; r++)
        for (unsigned i = 0; i < ARRAYSIZE(fns); i++) acc += fns[i]( acc + i );
    return acc;
}

#define RUN(name, expr, fmt) do { double t = now(); __typeof__(expr) v = expr; \
    printf( "%-8s %9.3f ms  " fmt "\n", name, (now() - t) * 1000, v ); } while (0)

int main( int argc, char **argv )
{
    printf( "bench: %u-bit\n", (unsigned)sizeof(void *) * 8 );
    RUN( "jit",    bench_jit(),              "checksum=%u" );
    RUN( "int",    bench_int( 100000000 ),   "checksum=%llu" );
    RUN( "fp",     bench_fp( 20000 ),        "checksum=%.6f" );
    RUN( "mem",    bench_mem( 20 ),          "checksum=%llu (1.28 GB copied)" );
    RUN( "atomic", bench_atomic( 8 ),        "count=%llu" );
    RUN( "order",  bench_order(),            "violations=%ld" );
    return 0;
}

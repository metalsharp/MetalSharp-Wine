/* Direct KUSER_SHARED_DATA reads, as done by inlined Windows time functions. */
#define _WIN32_WINNT 0x0A00
#include <windows.h>
#include <stdio.h>

int main(void)
{
    volatile ULONG *tick = (volatile ULONG *)0x7ffe0320;          /* TickCount.LowPart */
    volatile ULONG64 *itime = (volatile ULONG64 *)0x7ffe0008;     /* InterruptTime */
    LARGE_INTEGER f, a, b;
    ULONG64 sum = 0;
    unsigned i, n = 200000;

    QueryPerformanceFrequency( &f );
    QueryPerformanceCounter( &a );
    for (i = 0; i < n; i++) sum += *tick + *itime;
    QueryPerformanceCounter( &b );
    printf( "kuser: %u reads in %.3f ms (%.1f ns/read), tick=%lu vs GetTickCount=%lu, nonzero=%d\n", 2 * n,
            (b.QuadPart - a.QuadPart) * 1000.0 / f.QuadPart, (b.QuadPart - a.QuadPart) * 1e9 / f.QuadPart / (2 * n),
            *tick, GetTickCount(), sum != 0 );
    return 0;
}

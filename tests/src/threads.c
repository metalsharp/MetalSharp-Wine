/* Thread creation/teardown cost. */
#define _WIN32_WINNT 0x0A00
#include <windows.h>
#include <stdio.h>

static DWORD WINAPI nop( void *arg ) { return 0; }

int main(void)
{
    LARGE_INTEGER f, a, b;
    unsigned i, n = 200;
    QueryPerformanceFrequency( &f );
    QueryPerformanceCounter( &a );
    for (i = 0; i < n; i++)
    {
        HANDLE h = CreateThread( NULL, 0, nop, NULL, 0, NULL );
        WaitForSingleObject( h, INFINITE );
        CloseHandle( h );
    }
    QueryPerformanceCounter( &b );
    printf( "threads: %u create+join in %.2f ms (%.1f us each)\n", n,
            (b.QuadPart - a.QuadPart) * 1000.0 / f.QuadPart, (b.QuadPart - a.QuadPart) * 1e6 / f.QuadPart / n );
    return 0;
}

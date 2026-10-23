/* Synchronization latency: event ping-pong between two threads, uncontended mutex and
 * semaphore round trips, and a WaitForMultipleObjects sweep. Compare WINEMSYNC=0 and 1. */
#include <windows.h>
#include <stdio.h>

static HANDLE ping, pong;
static volatile LONG stop;

static double now_ms(void)
{
    LARGE_INTEGER c, f;
    QueryPerformanceCounter( &c );
    QueryPerformanceFrequency( &f );
    return c.QuadPart * 1000.0 / f.QuadPart;
}

static DWORD WINAPI ponger( void *arg )
{
    for (;;)
    {
        WaitForSingleObject( ping, INFINITE );
        if (stop) break;
        SetEvent( pong );
    }
    return 0;
}

int main( int argc, char **argv )
{
    const int n = argc > 1 ? atoi( argv[1] ) : 20000;
    HANDLE thread, mutex, sem, events[8];
    double t0, t;
    int i, ok = 1;

    ping = CreateEventW( NULL, FALSE, FALSE, NULL );
    pong = CreateEventW( NULL, FALSE, FALSE, NULL );
    thread = CreateThread( NULL, 0, ponger, NULL, 0, NULL );
    t0 = now_ms();
    for (i = 0; i < n; i++)
    {
        SetEvent( ping );
        if (WaitForSingleObject( pong, 5000 )) { ok = 0; break; }
    }
    t = now_ms() - t0;
    stop = 1;
    SetEvent( ping );
    WaitForSingleObject( thread, INFINITE );
    printf( "event ping-pong   %8.2f us/round trip\n", t * 1000 / n );

    mutex = CreateMutexW( NULL, FALSE, NULL );
    t0 = now_ms();
    for (i = 0; i < n * 10; i++)
    {
        if (WaitForSingleObject( mutex, 0 )) { ok = 0; break; }
        ReleaseMutex( mutex );
    }
    printf( "mutex acquire+rel %8.3f us\n", (now_ms() - t0) * 1000 / (n * 10) );

    sem = CreateSemaphoreW( NULL, 0, 1000000, NULL );
    t0 = now_ms();
    for (i = 0; i < n * 10; i++)
    {
        ReleaseSemaphore( sem, 1, NULL );
        if (WaitForSingleObject( sem, 0 )) { ok = 0; break; }
    }
    printf( "semaphore rel+wait%8.3f us\n", (now_ms() - t0) * 1000 / (n * 10) );

    for (i = 0; i < 8; i++) events[i] = CreateEventW( NULL, TRUE, FALSE, NULL );
    t0 = now_ms();
    for (i = 0; i < n * 10; i++)
    {
        SetEvent( events[i % 8] );
        if (WaitForMultipleObjects( 8, events, FALSE, 0 ) != WAIT_OBJECT_0 + i % 8) { ok = 0; break; }
        ResetEvent( events[i % 8] );
    }
    printf( "wait-any of 8     %8.3f us\n", (now_ms() - t0) * 1000 / (n * 10) );

    printf( "syncbench: %s\n", ok ? "ok" : "FAILED" );
    return !ok;
}

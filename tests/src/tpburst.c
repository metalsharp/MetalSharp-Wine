/* 512 thread pool waits timing out together: all callbacks must run promptly. */
#define _WIN32_WINNT 0x0A00
#include <windows.h>
#include <stdio.h>

#define N 512
static volatile LONG calls;
static void CALLBACK cb( PTP_CALLBACK_INSTANCE inst, void *ctx, PTP_WAIT wait, TP_WAIT_RESULT res )
{
    InterlockedIncrement( &calls );
}

int main(void)
{
    static HANDLE sems[N];
    static PTP_WAIT waits[N];
    PTP_POOL pool = CreateThreadpool( NULL );
    TP_CALLBACK_ENVIRON env;
    LARGE_INTEGER when;
    FILETIME ft;
    DWORD start, i;

    InitializeThreadpoolEnvironment( &env );
    SetThreadpoolCallbackPool( &env, pool );
    when.QuadPart = -50 * 10000;
    ft.dwLowDateTime = when.LowPart; ft.dwHighDateTime = when.HighPart;
    for (i = 0; i < N; i++)
    {
        sems[i] = CreateSemaphoreW( NULL, 0, 1, NULL );
        waits[i] = CreateThreadpoolWait( cb, NULL, &env );
    }
    start = GetTickCount();
    for (i = 0; i < N; i++) SetThreadpoolWait( waits[i], sems[i], &ft );
    while (calls < N && GetTickCount() - start < 20000) Sleep( 10 );
    printf( "tpburst: %ld/%d callbacks in %lu ms\n", calls, N, GetTickCount() - start );
    return calls != N;
}

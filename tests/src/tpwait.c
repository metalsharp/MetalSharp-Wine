/* Thread pool wait with a timeout on an unsignaled object: the callback must run with WAIT_TIMEOUT. */
#define _WIN32_WINNT 0x0A00
#include <windows.h>
#include <stdio.h>

static HANDLE done;
static LONG result = -1;
static void CALLBACK cb( PTP_CALLBACK_INSTANCE inst, void *ctx, PTP_WAIT wait, TP_WAIT_RESULT res )
{
    result = res;
    SetEvent( done );
}

int main(void)
{
    HANDLE sem = CreateSemaphoreW( NULL, 0, 1, NULL );
    PTP_WAIT wait = CreateThreadpoolWait( cb, NULL, NULL );
    FILETIME ft;
    LARGE_INTEGER when;
    DWORD r;

    done = CreateEventW( NULL, FALSE, FALSE, NULL );
    when.QuadPart = -50 * 10000;   /* 50ms relative */
    ft.dwLowDateTime = when.LowPart; ft.dwHighDateTime = when.HighPart;
    SetThreadpoolWait( wait, sem, &ft );
    r = WaitForSingleObject( done, 3000 );
    printf( "tpwait: %s, callback result %ld (expect %ld)\n", r ? "callback NOT called" : "callback called", result, (long)WAIT_TIMEOUT );
    return r != 0;
}

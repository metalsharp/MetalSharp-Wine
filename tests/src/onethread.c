#include <windows.h>
#include <stdio.h>
static DWORD WINAPI nop( void *arg ) { return 0; }
int main(void)
{
    HANDLE h;
    Sleep( 200 );
    OutputDebugStringA( "=== creating thread ===\n" );
    h = CreateThread( NULL, 0, nop, NULL, 0, NULL );
    WaitForSingleObject( h, INFINITE );
    OutputDebugStringA( "=== thread done ===\n" );
    return 0;
}

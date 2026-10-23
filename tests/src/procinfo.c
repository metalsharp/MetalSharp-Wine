/* NtQueryInformationProcess on the current process for every info class must not crash. */
#include <windows.h>
#include <winternl.h>
#include <stdio.h>

int main(void)
{
    NTSTATUS (WINAPI *query)(HANDLE, ULONG, void *, ULONG, ULONG *) =
        (void *)GetProcAddress( GetModuleHandleA( "ntdll.dll" ), "NtQueryInformationProcess" );
    static char buf[4096];
    ULONG i, len;
    for (i = 0; i < 0x80; i++)
    {
        printf( "procinfo: class %lu\n", i ); fflush( stdout );
        memset( buf, 0, sizeof(buf) );
        query( GetCurrentProcess(), i, buf, sizeof(buf), &len );
    }
    printf( "procinfo: all classes ok\n" );
    return 0;
}

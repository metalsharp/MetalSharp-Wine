#define _WIN32_WINNT 0x0A00
#include <windows.h>
#include <stdio.h>
int main(void)
{
    SYSTEM_INFO si;
    USHORT process_machine = 0, native_machine = 0;
    GetNativeSystemInfo( &si );
    IsWow64Process2( GetCurrentProcess(), &process_machine, &native_machine );
    printf( "hello from x86: sizeof(void*)=%u arch=%u process_machine=%04x native_machine=%04x cpus=%lu\n",
            (unsigned)sizeof(void *), si.wProcessorArchitecture, process_machine, native_machine, si.dwNumberOfProcessors );
    printf( "tick=%lu\n", GetTickCount() );
    return 0;
}

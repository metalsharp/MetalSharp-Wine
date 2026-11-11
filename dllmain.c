/* Linux module host as a DLL, for processes that load Linux code themselves (process.c): it makes up libc.so.6
 * and has Wine send the system calls of code at 0x700000000000 and above to the host. */
#include <windows.h>

#include "glibc.h"
#include "process.h"
#include "syscall.h"

#define LINUX_CODE_START ((void *)0x700000000000)
#define LINUX_CODE_SIZE  0x100000000000

BOOL WINAPI DllMain( HINSTANCE instance, DWORD reason, void *reserved )
{
    unsigned int count;
    const struct libc_export *exports;

    if (reason == DLL_THREAD_ATTACH) linuxhost_thread_init();
    if (reason != DLL_PROCESS_ATTACH) return TRUE;
    exports = linuxhost_libc_exports( &count );
    if (!linuxhost_process_init( exports, count )) return FALSE;
    if (!linuxhost_register_syscall_range( LINUX_CODE_START, LINUX_CODE_SIZE ))
        linuxhost_trace( "this Wine cannot send Linux system calls to the host\n" );
    else
        linuxhost_trace( "Linux system calls from %p go to the host\n", LINUX_CODE_START );
    return TRUE;
}

/* archs: x86_64 */
/* EFlags.DF must be visible in an exception context and survive a continue. */
#define _WIN32_WINNT 0x0A00
#include <windows.h>
#include <stdio.h>

static DWORD seen_flags;
static LONG CALLBACK veh( EXCEPTION_POINTERS *ep )
{
    if (ep->ExceptionRecord->ExceptionCode != EXCEPTION_ACCESS_VIOLATION) return EXCEPTION_CONTINUE_SEARCH;
    seen_flags = ep->ContextRecord->EFlags;
    ep->ContextRecord->Rip += 3;   /* skip mov [rax], ecx ; nop */
    return EXCEPTION_CONTINUE_EXECUTION;
}

int main(void)
{
    ULONG_PTR flags_after;
    AddVectoredExceptionHandler( 1, veh );
    __asm__ volatile( "std\n\t"
                      ".byte 0x89, 0x08, 0x90\n\t"   /* mov [rax], ecx ; nop -> faults */
                      "pushfq\n\tpopq %0\n\tcld" : "=r"(flags_after) : "a"(0), "c"(1) : "memory" );
    printf( "dfflag: DF in exception context %s, DF after continue %s\n",
            (seen_flags & 0x400) ? "set (ok)" : "CLEAR", (flags_after & 0x400) ? "set (ok)" : "CLEAR" );
    return !((seen_flags & 0x400) && (flags_after & 0x400));
}

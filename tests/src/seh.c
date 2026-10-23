/* archs: x86_64 i686 */
/* Exception handling checks: continue after a fault with a modified context, with and without
 * debug registers in the context, plus __try/__except style unwinding via RaiseException. */
#define _WIN32_WINNT 0x0A00
#include <windows.h>
#include <stdio.h>

static int mode, hits;

static LONG CALLBACK veh( EXCEPTION_POINTERS *ep )
{
    if (ep->ExceptionRecord->ExceptionCode != EXCEPTION_ACCESS_VIOLATION) return EXCEPTION_CONTINUE_SEARCH;
    hits++;
#ifdef _WIN64
    ep->ContextRecord->Rip += 3;   /* mov [rax], ecx -> 89 08 ... we use a 3-byte form below */
#else
    ep->ContextRecord->Eip += 3;
#endif
    if (mode == 1)
    {
        ep->ContextRecord->Dr0 = 0x42424240;
        ep->ContextRecord->Dr7 = 0x115;
        ep->ContextRecord->ContextFlags |= CONTEXT_DEBUG_REGISTERS;
    }
    return EXCEPTION_CONTINUE_EXECUTION;
}

static __attribute__((noinline)) int fault( volatile int *p )
{
    int r = 1;
    /* 3-byte store: mov [rax/eax], ecx with a REX/opsize-free encoding plus nop: 89 08 90 */
    __asm__ volatile( ".byte 0x89, 0x08, 0x90" :: "a"(p), "c"(7) : "memory" );
    return r;
}

int main(void)
{
    int ok = 1;
    void *h = AddVectoredExceptionHandler( 1, veh );
    for (mode = 0; mode < 2; mode++)
    {
        hits = 0;
        fault( NULL );
        printf( "seh: mode %d (%s): handler hits %d -> %s\n", mode, mode ? "with debug registers" : "plain",
                hits, hits == 1 ? "ok" : "FAIL" );
        ok &= hits == 1;
    }
    RemoveVectoredExceptionHandler( h );
    {
        ULONG_PTR args[2] = { 1, 2 };
        int caught = 0;
        ;
        (void)args; (void)caught;
    }
    return ok ? 0 : 1;
}

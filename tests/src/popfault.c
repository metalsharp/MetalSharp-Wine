/* archs: x86_64 */
/* Fault in "popq (%rax)": the instruction must not complete, RSP and [RSP] must be preserved,
 * and execution must continue correctly after the handler skips it. */
#define _WIN32_WINNT 0x0A00
#include <windows.h>
#include <stdio.h>

static ULONG64 fault_rsp, stack_value_at_fault;
static LONG CALLBACK veh( EXCEPTION_POINTERS *ep )
{
    if (ep->ExceptionRecord->ExceptionCode != EXCEPTION_ACCESS_VIOLATION) return EXCEPTION_CONTINUE_SEARCH;
    fault_rsp = ep->ContextRecord->Rsp;
    stack_value_at_fault = *(ULONG64 *)fault_rsp;
    fprintf( stderr, "handler: rsp %llx [rsp] %llx [rsp-8] %llx rip %llx\n", fault_rsp, stack_value_at_fault, *(ULONG64 *)(fault_rsp - 8), ep->ContextRecord->Rip ); fflush( stderr );
    if (stack_value_at_fault == 0) ExitProcess( 3 );
    ep->ContextRecord->Rip += 2;   /* skip popq (%rax) */
    return EXCEPTION_CONTINUE_EXECUTION;
}

int main(void)
{
    static const BYTE code[] = { 0x31, 0xc0, 0x8f, 0x00, 0xc3 };  /* xor eax,eax; popq (%rax); ret */
    void *mem = VirtualAlloc( NULL, 0x2000, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE );
    void (*func)(void) = mem;
    memcpy( mem, code, sizeof(code) );
    AddVectoredExceptionHandler( 1, veh );
    func();
    printf( "popfault: survived, rsp at fault %llx, [rsp] at fault %llx (nonzero=%s)\n",
            fault_rsp, stack_value_at_fault, stack_value_at_fault ? "ok" : "CLOBBERED" );
    return 0;
}

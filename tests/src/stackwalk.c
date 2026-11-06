/* archs: x86_64 */
/* stackwalk: a stack trace (RtlCaptureStackBackTrace) taken from a function whose return address is not code but
 * data above the user address space, as Overwatch's stack walks reach. Windows stops the walk there; it must not
 * fault in ntdll (ARM64EC's RtlIsEcCode read past its code bitmap for such an address). Prints "passed" or what
 * went wrong; exits 0 on success. */
#include <windows.h>
#include <stdio.h>

ULONG64 resume_rsp, resume_rip;
void *frames[16];
volatile LONG captured = -1;
static volatile LONG faults;

extern void walk_from_junk(void);

/* walk_from_junk: pushes a junk return address and jumps to inner, which takes the trace and goes back to
 * walk_from_junk's caller through the saved stack pointer (it cannot return to the junk address) */
__asm__(
    ".text\n"
    ".globl walk_from_junk\n"
    ".def walk_from_junk; .scl 2; .type 32; .endef\n"
    "walk_from_junk:\n"
    "  leaq 8(%rsp), %rax\n  movq %rax, resume_rsp(%rip)\n"
    "  movq (%rsp), %rax\n  movq %rax, resume_rip(%rip)\n"
    "  movabsq $0x000080e300a70070, %rax\n"
    "  pushq %rax\n"
    "  jmp junk_inner\n"

    ".def junk_inner; .scl 3; .type 32; .endef\n"
    ".seh_proc junk_inner\n"
    "junk_inner:\n"
    "  subq $40, %rsp\n  .seh_stackalloc 40\n"
    "  .seh_endprologue\n"
    "  xorl %ecx, %ecx\n"
    "  movl $16, %edx\n"
    "  leaq frames(%rip), %r8\n"
    "  xorl %r9d, %r9d\n"
    "  callq *__imp_RtlCaptureStackBackTrace(%rip)\n"
    "  movl %eax, captured(%rip)\n"
    "  movq resume_rsp(%rip), %rsp\n"
    "  jmpq *resume_rip(%rip)\n"
    "  nop\n"
    ".seh_endproc\n");

static LONG CALLBACK count(EXCEPTION_POINTERS *ep) {
  if (ep->ExceptionRecord->ExceptionCode == EXCEPTION_ACCESS_VIOLATION)
    faults++;
  return EXCEPTION_CONTINUE_SEARCH;
}

int main(void) {
  int failures = 0;
  AddVectoredExceptionHandler(1, count);
  walk_from_junk();
  if (captured < 1) {
    printf("wrong: captured %ld frames\n", captured);
    failures++;
  }
  if (faults) {
    printf("wrong: %ld access violations during the walk\n", faults);
    failures++;
  }
  /* the walk ends at the junk return address, which is no frame */
  for (LONG i = 0; i < captured; i++)
    if ((ULONG_PTR)frames[i] > 0x7ffffffeffffull) {
      printf("wrong: frame %ld is %p, past the user address space\n", i, frames[i]);
      failures++;
    }
  if (failures)
    printf("failed: %d wrong results\n", failures);
  else
    printf("passed: 0 wrong results (%ld frames)\n", captured);
  return failures != 0;
}

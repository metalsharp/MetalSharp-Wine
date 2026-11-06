/* archs: x86_64 */
/* unwindregs: an exception handler that unwinds out of the faulting function to a landing point frames above it
 * (RtlUnwindEx with a target IP), as protectors such as Overwatch's loader do with their int3 and int 2c probes.
 *
 * outer() puts known values in the nonvolatile registers (rbx, rsi, rdi, r12-r15, xmm6, xmm7) and calls inner(),
 * which saves them in its prologue (with unwind info), puts other values in them and executes int3 (then int 2c). The
 * vectored handler unwinds to outer's landing point; there, the nonvolatile registers must hold outer's values again,
 * restored by unwinding through the exception dispatcher's frame and inner's prologue, and rax the unwind's return
 * value. Under ARM64EC the dispatcher is native code and the unwind crosses it into x64 frames. Prints "passed" or
 * what differed; exits 0 on success. */
#include <windows.h>
#include <stdio.h>
#include <string.h>

struct regs {
  ULONG64 rbx, rsi, rdi, r12, r13, r14, r15;
  ULONG64 xmm6[2], xmm7[2];
  ULONG64 rax;
};

/* read and written by the assembly below, by name */
struct regs seen;
ULONG64 outer_frame;
static DWORD trap; /* 0: int3, 1: int 2c */
static volatile LONG handled;

extern ULONG64 outer(DWORD kind);
extern void outer_landing(void);

/* outer(kind): sets the nonvolatile registers, calls inner(kind); the handler unwinds to outer_landing, which stores
 * the registers into `seen` and returns. inner never returns normally. */
__asm__(
    ".text\n"
    ".globl outer\n"
    ".def outer; .scl 2; .type 32; .endef\n"
    ".seh_proc outer\n"
    "outer:\n"
    "  pushq %rbx\n  .seh_pushreg %rbx\n"
    "  pushq %rsi\n  .seh_pushreg %rsi\n"
    "  pushq %rdi\n  .seh_pushreg %rdi\n"
    "  pushq %r12\n  .seh_pushreg %r12\n"
    "  pushq %r13\n  .seh_pushreg %r13\n"
    "  pushq %r14\n  .seh_pushreg %r14\n"
    "  pushq %r15\n  .seh_pushreg %r15\n"
    "  subq $72, %rsp\n  .seh_stackalloc 72\n"
    "  movaps %xmm6, 32(%rsp)\n  .seh_savexmm %xmm6, 32\n"
    "  movaps %xmm7, 48(%rsp)\n  .seh_savexmm %xmm7, 48\n"
    "  .seh_endprologue\n"
    "  movq %rsp, outer_frame(%rip)\n"
    "  movabsq $0x1111111111111111, %rbx\n"
    "  movabsq $0x2222222222222222, %rsi\n"
    "  movabsq $0x3333333333333333, %rdi\n"
    "  movabsq $0x4444444444444444, %r12\n"
    "  movabsq $0x5555555555555555, %r13\n"
    "  movabsq $0x6666666666666666, %r14\n"
    "  movabsq $0x7777777777777777, %r15\n"
    "  movabsq $0x0606060606060606, %rax\n  movq %rax, %xmm6\n  pinsrq $1, %rax, %xmm6\n"
    "  movabsq $0x0707070707070707, %rax\n  movq %rax, %xmm7\n  pinsrq $1, %rax, %xmm7\n"
    "  call inner\n"
    "  nop\n"
    ".globl outer_landing\n"
    "outer_landing:\n"
    "  movq %rbx, seen+0(%rip)\n"
    "  movq %rsi, seen+8(%rip)\n"
    "  movq %rdi, seen+16(%rip)\n"
    "  movq %r12, seen+24(%rip)\n"
    "  movq %r13, seen+32(%rip)\n"
    "  movq %r14, seen+40(%rip)\n"
    "  movq %r15, seen+48(%rip)\n"
    "  movdqu %xmm6, seen+56(%rip)\n"
    "  movdqu %xmm7, seen+72(%rip)\n"
    "  movq %rax, seen+88(%rip)\n"
    "  movaps 32(%rsp), %xmm6\n"
    "  movaps 48(%rsp), %xmm7\n"
    "  addq $72, %rsp\n"
    "  popq %r15\n  popq %r14\n  popq %r13\n  popq %r12\n  popq %rdi\n  popq %rsi\n  popq %rbx\n"
    "  ret\n"
    ".seh_endproc\n"

    ".def inner; .scl 3; .type 32; .endef\n"
    ".seh_proc inner\n"
    "inner:\n"
    "  pushq %rbx\n  .seh_pushreg %rbx\n"
    "  pushq %rsi\n  .seh_pushreg %rsi\n"
    "  pushq %rdi\n  .seh_pushreg %rdi\n"
    "  pushq %r12\n  .seh_pushreg %r12\n"
    "  pushq %r13\n  .seh_pushreg %r13\n"
    "  pushq %r14\n  .seh_pushreg %r14\n"
    "  pushq %r15\n  .seh_pushreg %r15\n"
    "  subq $72, %rsp\n  .seh_stackalloc 72\n"
    "  movaps %xmm6, 32(%rsp)\n  .seh_savexmm %xmm6, 32\n"
    "  movaps %xmm7, 48(%rsp)\n  .seh_savexmm %xmm7, 48\n"
    "  .seh_endprologue\n"
    "  movl %ecx, %eax\n"
    "  movabsq $0xdead00000000beef, %rbx\n"
    "  movq %rbx, %rsi\n  movq %rbx, %rdi\n  movq %rbx, %r12\n  movq %rbx, %r13\n  movq %rbx, %r14\n  movq %rbx, %r15\n"
    "  movq %rbx, %xmm6\n  movq %rbx, %xmm7\n"
    "  testl %eax, %eax\n"
    "  jnz 1f\n"
    "  int3\n"
    "  jmp 2f\n"
    "1:\n"
    "  int $0x2c\n"
    "2:\n"
    "  ud2\n"
    "  nop\n"
    ".seh_endproc\n");

static LONG CALLBACK handler(EXCEPTION_POINTERS *ep) {
  DWORD code = ep->ExceptionRecord->ExceptionCode;
  CONTEXT ctx;
  if (code != EXCEPTION_BREAKPOINT && code != STATUS_ASSERTION_FAILURE)
    return EXCEPTION_CONTINUE_SEARCH;
  handled++;
  /* RtlUnwindEx takes the context as scratch: the registers it unwinds to are the target's */
  ctx = *ep->ContextRecord;
  RtlUnwindEx((void *)outer_frame, (void *)outer_landing, ep->ExceptionRecord, (void *)(ULONG_PTR)0xabcdef0123456789ull,
              &ctx, NULL);
  return EXCEPTION_CONTINUE_SEARCH; /* not reached */
}

static int check(const char *what) {
  static const ULONG64 want[] = {0x1111111111111111ull, 0x2222222222222222ull, 0x3333333333333333ull,
                                 0x4444444444444444ull, 0x5555555555555555ull, 0x6666666666666666ull,
                                 0x7777777777777777ull};
  static const char *names[] = {"rbx", "rsi", "rdi", "r12", "r13", "r14", "r15"};
  const ULONG64 *got = &seen.rbx;
  int bad = 0;
  for (int i = 0; i < 7; i++)
    if (got[i] != want[i]) {
      printf("wrong: after %s, %s is %#llx, expected %#llx\n", what, names[i], (unsigned long long)got[i],
             (unsigned long long)want[i]);
      bad++;
    }
  if (seen.xmm6[0] != 0x0606060606060606ull || seen.xmm6[1] != 0x0606060606060606ull) {
    printf("wrong: after %s, xmm6 is %#llx:%#llx\n", what, (unsigned long long)seen.xmm6[1],
           (unsigned long long)seen.xmm6[0]);
    bad++;
  }
  if (seen.xmm7[0] != 0x0707070707070707ull || seen.xmm7[1] != 0x0707070707070707ull) {
    printf("wrong: after %s, xmm7 is %#llx:%#llx\n", what, (unsigned long long)seen.xmm7[1],
           (unsigned long long)seen.xmm7[0]);
    bad++;
  }
  if (seen.rax != 0xabcdef0123456789ull) {
    printf("wrong: after %s, rax (the return value) is %#llx\n", what, (unsigned long long)seen.rax);
    bad++;
  }
  return bad;
}

int main(void) {
  int failures = 0;
  AddVectoredExceptionHandler(1, handler);
  for (trap = 0; trap < 2; trap++) {
    memset(&seen, 0, sizeof(seen));
    outer(trap);
    failures += check(trap ? "int 2c" : "int3");
  }
  if (handled != 2) {
    printf("wrong: %ld exceptions handled, expected 2\n", handled);
    failures++;
  }
  if (failures)
    printf("failed: %d wrong results\n", failures);
  else
    printf("passed: 0 wrong results\n");
  return failures != 0;
}

/* archs: x86_64 */
/* debugregs: x64 hardware breakpoints (Dr0-3, Dr6, Dr7) as protectors such as Overwatch's loader use them.
 *
 * A thread sets an instruction breakpoint on a function in its own context (SetThreadContext from another thread,
 * and NtContinue from an exception handler's context), calls it, and expects EXCEPTION_SINGLE_STEP at the function's
 * first instruction with Dr6's bit for the breakpoint; the handler continues with RF set, so the function runs once
 * and returns. GetThreadContext reads the registers back. Under FEX's ARM64EC emulator this needs Wine to hand the
 * x64 debug registers to the emulator (ntdll) and the emulator to check them (FEXCore). Prints "passed" or what
 * failed; exits 0 on success. */
#include <windows.h>
#include <stdio.h>

static volatile LONG hits, after_hits;
static volatile DWORD64 hit_address, hit_dr6;
static int failures;

__declspec(noinline) static int target(int x) { return x * 3 + 1; }
__declspec(noinline) static int second(int x) { return x - 7; }

static LONG CALLBACK handler(EXCEPTION_POINTERS *ep) {
  if (ep->ExceptionRecord->ExceptionCode != EXCEPTION_SINGLE_STEP)
    return EXCEPTION_CONTINUE_SEARCH;
  hits++;
  hit_address = (DWORD64)ep->ExceptionRecord->ExceptionAddress;
  hit_dr6 = ep->ContextRecord->Dr6;
  /* the second breakpoint is set from here, as a protector's handler does: it is armed by NtContinue */
  if (hits == 1) {
    ep->ContextRecord->Dr1 = (DWORD64)&second;
    ep->ContextRecord->Dr7 |= 1 << 2;
  }
  ep->ContextRecord->EFlags |= 0x10000; /* RF: run the instruction once */
  ep->ContextRecord->Dr6 = 0;
  return EXCEPTION_CONTINUE_EXECUTION;
}

static DWORD WINAPI arm(void *param) {
  HANDLE thread = param;
  CONTEXT ctx = {0};
  SuspendThread(thread);
  ctx.ContextFlags = CONTEXT_DEBUG_REGISTERS;
  ctx.Dr0 = (DWORD64)&target;
  ctx.Dr7 = 1; /* L0: execute breakpoint, length 1 */
  if (!SetThreadContext(thread, &ctx)) {
    printf("wrong: SetThreadContext failed: %lu\n", GetLastError());
    failures++;
  }
  ResumeThread(thread);
  return 0;
}

#define CHECK(c, ...) do { if (!(c)) { printf("wrong: " __VA_ARGS__); printf("\n"); failures++; } } while (0)

int main(void) {
  HANDLE self, armer;
  CONTEXT ctx = {0};
  int r;

  AddVectoredExceptionHandler(1, handler);
  DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &self, 0, FALSE, DUPLICATE_SAME_ACCESS);
  armer = CreateThread(NULL, 0, arm, self, 0, NULL);
  WaitForSingleObject(armer, INFINITE);

  ctx.ContextFlags = CONTEXT_DEBUG_REGISTERS;
  CHECK(GetThreadContext(GetCurrentThread(), &ctx), "GetThreadContext failed");
  CHECK(ctx.Dr0 == (DWORD64)&target, "Dr0 reads back %#llx, expected %#llx", (unsigned long long)ctx.Dr0,
        (unsigned long long)(DWORD64)&target);
  CHECK((ctx.Dr7 & 1) == 1, "Dr7 reads back %#llx", (unsigned long long)ctx.Dr7);

  r = target(4);
  CHECK(r == 13, "target returned %d", r);
  CHECK(hits == 1, "%ld breakpoint exceptions at target, expected 1", hits);
  CHECK(hit_address == (DWORD64)&target, "breakpoint at %#llx, expected %#llx", (unsigned long long)hit_address,
        (unsigned long long)(DWORD64)&target);
  CHECK(hit_dr6 & 1, "Dr6 %#llx lacks B0", (unsigned long long)hit_dr6);

  r = target(5); /* RF only covers the one instruction: the breakpoint fires again */
  CHECK(r == 16 && hits == 2, "second call: %d, %ld hits", r, hits);

  r = second(10); /* armed from the handler through NtContinue */
  CHECK(r == 3, "second returned %d", r);
  CHECK(hits == 3, "%ld breakpoint exceptions after second, expected 3", hits);
  CHECK(hit_address == (DWORD64)&second && (hit_dr6 & 2), "second's hit at %#llx, Dr6 %#llx",
        (unsigned long long)hit_address, (unsigned long long)hit_dr6);

  /* cleared: no more exceptions */
  ctx.ContextFlags = CONTEXT_DEBUG_REGISTERS;
  GetThreadContext(GetCurrentThread(), &ctx);
  ctx.Dr0 = ctx.Dr1 = 0;
  ctx.Dr7 = 0;
  CHECK(SetThreadContext(GetCurrentThread(), &ctx), "SetThreadContext (clear) failed");
  after_hits = hits;
  target(1);
  second(1);
  CHECK(hits == after_hits, "%ld exceptions after clearing the breakpoints", hits - after_hits);

  if (failures)
    printf("failed: %d wrong results\n", failures);
  else
    printf("passed: 0 wrong results\n");
  return failures != 0;
}

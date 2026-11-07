/* archs: x86_64 */
/* execfault: code that runs into a page it may not execute, as decrypt-on-execute protectors (Overwatch's loader)
 * arrange it.
 *
 * An instruction starts at the end of an executable page and continues into a no-access page. Windows raises an
 * execute access violation naming the first byte it couldn't fetch (the second page), and a handler that makes that
 * page executable and continues has the instruction run whole. A fault naming the instruction's own page sends such
 * a handler round forever: it unlocks a page that is already unlocked. Prints "passed" or what failed; exits 0 on
 * success. */
#include <windows.h>
#include <stdio.h>

static BYTE *pages;
static SYSTEM_INFO si;
static volatile LONG faults;
static volatile ULONG_PTR fault_kind, fault_address;

static LONG CALLBACK handler(EXCEPTION_POINTERS *ep) {
  DWORD old;
  if (ep->ExceptionRecord->ExceptionCode != EXCEPTION_ACCESS_VIOLATION)
    return EXCEPTION_CONTINUE_SEARCH;
  if (++faults > 4)
    return EXCEPTION_CONTINUE_SEARCH; /* a handler unlocking the wrong page would loop: give up */
  fault_kind = ep->ExceptionRecord->ExceptionInformation[0];
  fault_address = ep->ExceptionRecord->ExceptionInformation[1];
  /* unlock the page the fault names, as the protector does */
  VirtualProtect((void *)(fault_address & ~(ULONG_PTR)(si.dwPageSize - 1)), si.dwPageSize, PAGE_EXECUTE_READ, &old);
  return EXCEPTION_CONTINUE_EXECUTION;
}

int main(void) {
  DWORD old;
  BYTE *code;
  int (*fn)(void);
  int r, failures = 0;

  GetSystemInfo(&si);
  pages = VirtualAlloc(NULL, 2 * si.dwPageSize, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
  /* mov eax, 0x12345678 (5 bytes) starting 2 bytes before the page boundary, then ret on the second page */
  code = pages + si.dwPageSize - 2;
  code[0] = 0xb8;
  code[1] = 0x78;
  code[2] = 0x56;
  code[3] = 0x34;
  code[4] = 0x12;
  code[5] = 0xc3;
  FlushInstructionCache(GetCurrentProcess(), pages, 2 * si.dwPageSize);
  VirtualProtect(pages, si.dwPageSize, PAGE_EXECUTE_READ, &old);
  VirtualProtect(pages + si.dwPageSize, si.dwPageSize, PAGE_NOACCESS, &old);
  AddVectoredExceptionHandler(1, handler);

  fn = (int (*)(void))code;
  r = fn();

  if (r != 0x12345678) {
    printf("wrong: returned %#x\n", r);
    failures++;
  }
  if (faults != 1) {
    printf("wrong: %ld access violations, expected 1\n", faults);
    failures++;
  }
  if (fault_kind != 8) {
    printf("wrong: access violation kind %lu, expected 8 (execute)\n", (unsigned long)fault_kind);
    failures++;
  }
  if (fault_address != (ULONG_PTR)(pages + si.dwPageSize)) {
    printf("wrong: fault names %p, expected %p (the first byte not executable)\n", (void *)fault_address,
           (void *)(pages + si.dwPageSize));
    failures++;
  }
  if (failures)
    printf("failed: %d wrong results\n", failures);
  else
    printf("passed: 0 wrong results\n");
  return failures != 0;
}

/* archs: x86_64 i686 */
/* rdtscp: RDTSCP and RDPID against GetCurrentProcessorNumber, whether or not CPUID advertises them. Programs such as
 * Blender execute them without checking CPUID, so they must not fault under Wine. Each sample reads the processor
 * number before and after the instruction and counts only when both agree, so a mismatch means a wrong value rather
 * than a migration (bar a rare migration away and back). Prints "rdtscp ok" or "rdtscp FAIL ..." last. */
#include <windows.h>
#include <intrin.h>
#include <stdint.h>
#include <stdio.h>

#define SAMPLES 200000
#define THREADS 8

static volatile LONG faulted;

static LONG CALLBACK skip_ud(EXCEPTION_POINTERS *ep) {
  if (ep->ExceptionRecord->ExceptionCode != EXCEPTION_ILLEGAL_INSTRUCTION) return EXCEPTION_CONTINUE_SEARCH;
  InterlockedExchange(&faulted, 1);
#ifdef _WIN64
  ep->ContextRecord->Rip += 4;  /* both probes below are 4 bytes up to a nop */
#else
  ep->ContextRecord->Eip += 4;
#endif
  return EXCEPTION_CONTINUE_EXECUTION;
}

static unsigned rdpid(void) {
  uintptr_t v = ~(uintptr_t)0;
  __asm__ volatile(".byte 0xf3, 0x0f, 0xc7, 0xf8" : "+a"(v));  /* rdpid eax/rax (4 bytes) */
  return (unsigned)v;
}

struct result { unsigned matched, mismatched, out_of_range, backwards; };

static DWORD WINAPI worker(void *arg) {
  struct result *r = arg;
  unsigned ncpu = GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);
  unsigned long long last = 0;
  for (int i = 0; i < SAMPLES; i++) {
    unsigned aux, before = GetCurrentProcessorNumber(), id;
    unsigned long long tsc = __rdtscp(&aux);
    id = (i & 1) ? rdpid() : aux;
    unsigned after = GetCurrentProcessorNumber();
    if (tsc < last) r->backwards++;
    last = tsc;
    if (before != after) continue;
    r->matched++;
    if (id >= ncpu) r->out_of_range++;
    else if (id != before) r->mismatched++;
  }
  return 0;
}

int main(void) {
  int c[4];
  __cpuid(c, 0x80000001);
  int adv_rdtscp = (c[3] >> 27) & 1;
  __cpuidex(c, 7, 0);
  int adv_rdpid = (c[2] >> 22) & 1;
  printf("cpuid rdtscp=%d rdpid=%d ncpu=%lu\n", adv_rdtscp, adv_rdpid, GetActiveProcessorCount(ALL_PROCESSOR_GROUPS));

  /* RDTSCP is 3 bytes and RDPID 4; probe each alone so a fault can be skipped exactly. */
  PVOID h = AddVectoredExceptionHandler(1, skip_ud);
  unsigned aux;
  faulted = 0;
  __asm__ volatile("rdtscp; nop" : "=c"(aux) : : "eax", "edx");  /* 3 + 1 bytes */
  if (faulted) { printf("rdtscp FAIL: RDTSCP raised #UD\n"); return 1; }
  faulted = 0;
  __asm__ volatile(".byte 0xf3, 0x0f, 0xc7, 0xf8; nop" : : : "eax");
  RemoveVectoredExceptionHandler(h);
  if (faulted) { printf("rdtscp FAIL: RDPID raised #UD\n"); return 1; }

  LARGE_INTEGER f, t0, t1;
  QueryPerformanceFrequency(&f);
  QueryPerformanceCounter(&t0);
  for (int i = 0; i < 100000; i++) __rdtscp(&aux);
  QueryPerformanceCounter(&t1);
  printf("rdtscp %.1f ns each\n", (t1.QuadPart - t0.QuadPart) * 1e9 / f.QuadPart / 100000);

  HANDLE th[THREADS];
  struct result r[THREADS] = {0}, total = {0};
  for (int i = 0; i < THREADS; i++) th[i] = CreateThread(NULL, 0, worker, &r[i], 0, NULL);
  WaitForMultipleObjects(THREADS, th, TRUE, INFINITE);
  for (int i = 0; i < THREADS; i++) {
    total.matched += r[i].matched; total.mismatched += r[i].mismatched;
    total.out_of_range += r[i].out_of_range; total.backwards += r[i].backwards;
  }
  printf("samples %u mismatched %u out_of_range %u backwards %u\n", total.matched, total.mismatched,
         total.out_of_range, total.backwards);
  if (total.out_of_range || total.backwards || !total.matched || total.mismatched * 1000ull > total.matched) {
    printf("rdtscp FAIL\n");
    return 1;
  }
  printf("rdtscp ok\n");
  return 0;
}

/* archs: x86_64 */
/* libs: powrprof */
/* cpuident: what an x64 program can learn about the CPU it runs on, the way protectors and anti-VM checks look
 * (CPUID vendor, brand, family/model, feature and hypervisor bits, the TSC leaves; RDTSC's measured rate; and the
 * processor clock CallNtPowerInformation reports). Prints one "name value" line each, so two runtimes' output (FEX,
 * Rosetta) can be diffed. */
#include <windows.h>
#include <powrprof.h>
#include <intrin.h>
#include <stdio.h>
#include <string.h>

typedef struct {
  ULONG Number, MaxMhz, CurrentMhz, MhzLimit, MaxIdleState, CurrentIdleState;
} PROCESSOR_POWER_INFO;

static LONG CALLBACK continue_beef(EXCEPTION_POINTERS *ep) {
  return ep->ExceptionRecord->ExceptionCode == 0x4000beef ? EXCEPTION_CONTINUE_EXECUTION : EXCEPTION_CONTINUE_SEARCH;
}

static void leaf(unsigned l, unsigned s) {
  int r[4];
  __cpuidex(r, l, s);
  printf("cpuid %08x.%u eax=%08x ebx=%08x ecx=%08x edx=%08x\n", l, s, r[0], r[1], r[2], r[3]);
}

int main(void) {
  int r[4];
  char vendor[13] = {0}, brand[49] = {0};
  unsigned max_leaf, max_ext;

  __cpuid(r, 0);
  max_leaf = r[0];
  memcpy(vendor, &r[1], 4), memcpy(vendor + 4, &r[3], 4), memcpy(vendor + 8, &r[2], 4);
  printf("vendor %s (max leaf %#x)\n", vendor, max_leaf);
  __cpuid(r, 0x80000000);
  max_ext = r[0];
  if (max_ext >= 0x80000004) {
    __cpuid((int *)brand, 0x80000002), __cpuid((int *)(brand + 16), 0x80000003), __cpuid((int *)(brand + 32), 0x80000004);
    printf("brand %s\n", brand);
  }
  __cpuid(r, 1);
  printf("family/model/stepping eax=%08x, hypervisor bit %d, features ecx=%08x edx=%08x\n", r[0], (r[2] >> 31) & 1,
         r[2], r[3]);
  if ((r[2] >> 31) & 1) {
    char hv[13] = {0};
    __cpuid(r, 0x40000000);
    memcpy(hv, &r[1], 4), memcpy(hv + 4, &r[2], 4), memcpy(hv + 8, &r[3], 4);
    printf("hypervisor vendor %s\n", hv);
  }
  for (unsigned l = 0; l <= max_leaf && l <= 0x20; l++)
    if (l == 4 || l == 7 || l == 0xb || l == 0xd)
      leaf(l, 0), leaf(l, 1);
    else
      leaf(l, 0);
  for (unsigned l = 0x80000000; l <= max_ext && l <= 0x80000008; l++)
    leaf(l, 0);

  /* RDTSC against QueryPerformanceCounter over 200 ms */
  LARGE_INTEGER f, a, b;
  QueryPerformanceFrequency(&f);
  QueryPerformanceCounter(&a);
  unsigned long long t0 = __rdtsc();
  Sleep(200);
  unsigned long long t1 = __rdtsc();
  QueryPerformanceCounter(&b);
  double seconds = (double)(b.QuadPart - a.QuadPart) / f.QuadPart;
  printf("qpc frequency %lld\n", f.QuadPart);
  printf("rdtsc rate %.1f MHz\n", (t1 - t0) / seconds / 1e6);
  /* how long RDTSC itself takes, and back-to-back deltas: emulators and debuggers show up here */
  unsigned long long best = ~0ull, sum = 0;
  for (int i = 0; i < 1000; i++) {
    unsigned long long x = __rdtsc(), y = __rdtsc();
    if (y - x < best)
      best = y - x;
    sum += y - x;
  }
  printf("rdtsc back-to-back min %llu mean %.1f ticks\n", best, sum / 1000.0);
  /* an exception's round trip in RDTSC ticks, as anti-debug timing checks measure it */
  void *veh = AddVectoredExceptionHandler(1, continue_beef);
  unsigned long long e0 = __rdtsc();
  RaiseException(0x4000beef, 0, 0, NULL);
  unsigned long long e1 = __rdtsc();
  RemoveVectoredExceptionHandler(veh);
  printf("exception round trip %llu ticks\n", e1 - e0);

  SYSTEM_INFO si;
  GetSystemInfo(&si);
  printf("processors %lu\n", si.dwNumberOfProcessors);
  PROCESSOR_POWER_INFO ppi[64];
  memset(ppi, 0, sizeof(ppi));
  long status = CallNtPowerInformation(ProcessorInformation, NULL, 0, ppi,
                                       sizeof(PROCESSOR_POWER_INFO) * (si.dwNumberOfProcessors < 64 ? si.dwNumberOfProcessors : 64));
  printf("power information status %#lx, cpu0 MaxMhz %lu CurrentMhz %lu MhzLimit %lu\n", (unsigned long)status,
         ppi[0].MaxMhz, ppi[0].CurrentMhz, ppi[0].MhzLimit);
  HKEY key;
  DWORD mhz = 0, size = sizeof(mhz);
  char name[128] = {0};
  DWORD nsize = sizeof(name);
  if (!RegOpenKeyExA(HKEY_LOCAL_MACHINE, "HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0", 0, KEY_READ, &key)) {
    RegQueryValueExA(key, "~MHz", NULL, NULL, (BYTE *)&mhz, &size);
    RegQueryValueExA(key, "ProcessorNameString", NULL, NULL, (BYTE *)name, &nsize);
    RegCloseKey(key);
  }
  printf("registry ~MHz %lu name %s\n", mhz, name);
  return 0;
}

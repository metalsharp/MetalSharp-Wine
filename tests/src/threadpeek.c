/* archs: x86_64 */
/* threadpeek <exe name> [samples] [interval ms]: where another process's threads are, as a debugger's "break all"
 * would show, for finding what a busy program loops on. Each sample suspends every thread of the first process with
 * that exe name, reads its x64 context (GetThreadContext) and resumes it. Prints, per thread, its hottest RIPs as
 * module+offset with hit counts, and the 32 bytes at the hottest one (code a protector decrypted in memory). */
#include <windows.h>
#include <tlhelp32.h>
#include <psapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_THREADS 256
#define MAX_RIPS 64

struct hit { DWORD64 rip; int count; };
struct thread { DWORD tid; int samples; struct hit hits[MAX_RIPS]; int nhits; DWORD64 rsp; };

static HMODULE modules[1024];
static DWORD nmodules;

static void where(HANDLE process, DWORD64 addr, char *out, size_t size) {
  MODULEINFO mi;
  char name[MAX_PATH];
  for (DWORD i = 0; i < nmodules; i++)
    if (GetModuleInformation(process, modules[i], &mi, sizeof(mi)) && addr >= (DWORD64)mi.lpBaseOfDll &&
        addr < (DWORD64)mi.lpBaseOfDll + mi.SizeOfImage) {
      GetModuleBaseNameA(process, modules[i], name, sizeof(name));
      snprintf(out, size, "%s+%#llx", name, (unsigned long long)(addr - (DWORD64)mi.lpBaseOfDll));
      return;
    }
  snprintf(out, size, "%#llx (no module)", (unsigned long long)addr);
}

int main(int argc, char **argv) {
  const char *exe = argc > 1 ? argv[1] : "Overwatch.exe";
  int samples = argc > 2 ? atoi(argv[2]) : 50, interval = argc > 3 ? atoi(argv[3]) : 40;
  static struct thread threads[MAX_THREADS];
  int nthreads = 0;
  DWORD pid = 0;
  PROCESSENTRY32 pe = {sizeof(pe)};
  HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
  for (BOOL ok = Process32First(snap, &pe); ok; ok = Process32Next(snap, &pe))
    if (!_stricmp(pe.szExeFile, exe)) {
      pid = pe.th32ProcessID;
      break;
    }
  CloseHandle(snap);
  if (!pid) {
    printf("no process %s\n", exe);
    return 1;
  }
  HANDLE process = OpenProcess(PROCESS_ALL_ACCESS, FALSE, pid);
  EnumProcessModules(process, modules, sizeof(modules), &nmodules);
  nmodules /= sizeof(HMODULE);
  printf("%s pid %#lx, %lu modules\n", exe, pid, nmodules);

  for (int s = 0; s < samples; s++) {
    THREADENTRY32 te = {sizeof(te)};
    snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te)) {
      if (te.th32OwnerProcessID != pid)
        continue;
      HANDLE th = OpenThread(THREAD_ALL_ACCESS, FALSE, te.th32ThreadID);
      if (!th)
        continue;
      CONTEXT ctx = {0};
      ctx.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
      if (SuspendThread(th) != (DWORD)-1) {
        if (GetThreadContext(th, &ctx)) {
          int t;
          for (t = 0; t < nthreads && threads[t].tid != te.th32ThreadID; t++)
            ;
          if (t == nthreads && nthreads < MAX_THREADS)
            threads[nthreads++].tid = te.th32ThreadID;
          if (t < nthreads) {
            struct thread *tr = &threads[t];
            int h;
            tr->samples++;
            tr->rsp = ctx.Rsp;
            for (h = 0; h < tr->nhits && tr->hits[h].rip != ctx.Rip; h++)
              ;
            if (h == tr->nhits && tr->nhits < MAX_RIPS)
              tr->hits[tr->nhits++].rip = ctx.Rip;
            if (h < tr->nhits)
              tr->hits[h].count++;
          }
        }
        ResumeThread(th);
      }
      CloseHandle(th);
    }
    CloseHandle(snap);
    Sleep(interval);
  }

  for (int t = 0; t < nthreads; t++) {
    struct thread *tr = &threads[t];
    /* the hottest RIPs first */
    for (int a = 0; a < tr->nhits; a++)
      for (int b = a + 1; b < tr->nhits; b++)
        if (tr->hits[b].count > tr->hits[a].count) {
          struct hit x = tr->hits[a];
          tr->hits[a] = tr->hits[b];
          tr->hits[b] = x;
        }
    printf("thread %#lx: %d samples, %d places\n", tr->tid, tr->samples, tr->nhits);
    for (int h = 0; h < tr->nhits && h < 6; h++) {
      char w[300];
      where(process, tr->hits[h].rip, w, sizeof(w));
      printf("  %3d  %s\n", tr->hits[h].count, w);
    }
    /* the stack above RSP: values that point into a module are (mostly) return addresses, newest first */
    if (tr->rsp) {
      ULONG64 stack[1024];
      SIZE_T got = 0;
      int shown = 0;
      if (ReadProcessMemory(process, (void *)tr->rsp, stack, sizeof(stack), &got) || got)
        for (SIZE_T i = 0; i < got / 8 && shown < 14; i++) {
          char w[300];
          if (stack[i] < 0x10000)
            continue;
          where(process, stack[i], w, sizeof(w));
          if (strstr(w, "(no module)"))
            continue;
          printf("    rsp+%#05llx  %s\n", (unsigned long long)(i * 8), w);
          shown++;
        }
    }
    if (tr->nhits) {
      unsigned char code[32];
      SIZE_T got = 0;
      if (ReadProcessMemory(process, (void *)(tr->hits[0].rip), code, sizeof(code), &got) && got) {
        printf("  code:");
        for (SIZE_T i = 0; i < got; i++)
          printf(" %02x", code[i]);
        printf("\n");
      }
    }
  }
  return 0;
}

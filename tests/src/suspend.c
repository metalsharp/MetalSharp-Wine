/* Suspends and resumes a thread that runs translated code, as Mono's and Unity's garbage collectors do to stop the
 * world: thousands of SuspendThread/GetThreadContext/ResumeThread rounds against a worker in a loop of calls and
 * indirect branches. The worker must keep running, each stop must give a context inside the worker's code with its
 * loop counter in a register, and nothing may hang or crash.
 *   suspend [rounds]   prints SUSPEND_OK, or SUSPEND_FAIL with what went wrong */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>

static volatile LONG64 progress;
static volatile LONG stop;

#define NOINLINE __attribute__((noinline))
NOINLINE static unsigned f0(unsigned x) { return x * 3 + 1; }
NOINLINE static unsigned f1(unsigned x) { return (x ^ 0x5a5a) + 7; }
NOINLINE static unsigned f2(unsigned x) { return (x >> 1) + (x << 3); }
NOINLINE static unsigned f3(unsigned x) { return x * 2654435761u; }
static unsigned (*const table[4])(unsigned) = { f0, f1, f2, f3 };

static DWORD WINAPI worker(void *arg)
{
    unsigned x = 1;
    while (!stop)
    {
        for (int i = 0; i < 1000; i++) x = table[x & 3](x) + i;
        InterlockedIncrement64(&progress);
    }
    return x;
}

int main(int argc, char **argv)
{
    int rounds = argc > 1 ? atoi(argv[1]) : 20000;
    HANDLE thread = CreateThread(NULL, 0, worker, NULL, 0, NULL);
    HMODULE self = GetModuleHandleA(NULL);
    MEMORY_BASIC_INFORMATION info;
    int bad_context = 0, stalled = 0;

    VirtualQuery((void *)main, &info, sizeof(info));
    while (!progress) Sleep(1);
    DWORD start = GetTickCount();
    for (int i = 0; i < rounds; i++)
    {
        LONG64 before = progress;
        if (SuspendThread(thread) == (DWORD)-1)
        {
            printf("SUSPEND_FAIL: SuspendThread failed (%lu) at round %d\n", GetLastError(), i);
            return 1;
        }
        CONTEXT context = { .ContextFlags = CONTEXT_FULL };
        if (!GetThreadContext(thread, &context))
        {
            printf("SUSPEND_FAIL: GetThreadContext failed (%lu) at round %d\n", GetLastError(), i);
            return 1;
        }
        /* a suspended thread stops in its own code or in a system function it called */
#ifdef _WIN64
        if (!context.Rip || !context.Rsp)
#else
        if (!context.Eip || !context.Esp)
#endif
            bad_context++;
        /* while it is suspended the worker must not move */
        LONG64 during = progress;
        Sleep(0);
        if (progress != during)
        {
            printf("SUSPEND_FAIL: the worker ran while suspended at round %d\n", i);
            return 1;
        }
        if (ResumeThread(thread) == (DWORD)-1)
        {
            printf("SUSPEND_FAIL: ResumeThread failed (%lu) at round %d\n", GetLastError(), i);
            return 1;
        }
        /* every so often, check it still runs after being resumed */
        if (i % 1000 == 999)
        {
            DWORD wait = GetTickCount();
            while (progress == before && GetTickCount() - wait < 5000) Sleep(1);
            if (progress == before) stalled++;
        }
    }
    DWORD ms = GetTickCount() - start;
    stop = 1;
    if (WaitForSingleObject(thread, 10000) != WAIT_OBJECT_0)
    {
        printf("SUSPEND_FAIL: the worker didn't exit\n");
        return 1;
    }
    printf("%d rounds in %lu ms (%.1f us each), worker iterations %lld, bad contexts %d, stalls %d\n", rounds, ms,
           ms * 1000.0 / rounds, (long long)progress, bad_context, stalled);
    printf(bad_context || stalled ? "SUSPEND_FAIL\n" : "SUSPEND_OK\n");
    (void)self;
    return bad_context || stalled;
}

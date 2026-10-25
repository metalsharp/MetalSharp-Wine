/* Samples where a thread of another process runs, as the program sees it: what macOS's sample shows of a program run
 * by FEX is the translated code, with no symbols. The thread is suspended, its context read and resumed, many times,
 * and the instruction pointers are counted by module and offset (and the return addresses on its stack, one frame up,
 * when the frame pointer chain gives one).
 * 32-bit (WoW64) processes are sampled through their x86 context.
 *   guestsample <pid> <thread name substring | 0xTID | all> [samples] [interval ms]
 * prints "count module+offset" for the most frequent places, then for their callers ("all" samples every thread
 * and prefixes each place with its thread id) */
#include <windows.h>
#include <psapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <tlhelp32.h>
#include <wchar.h>

struct module { char name[64]; DWORD64 base, size; };
static struct module modules[1024];
static int module_count;

static void load_modules(HANDLE process)
{
    HMODULE handles[1024];
    DWORD needed;
    if (!EnumProcessModulesEx(process, handles, sizeof(handles), &needed, LIST_MODULES_ALL)) return;
    for (DWORD i = 0; i < needed / sizeof(HMODULE) && module_count < 1024; i++)
    {
        MODULEINFO info;
        struct module *m = &modules[module_count];
        if (!GetModuleInformation(process, handles[i], &info, sizeof(info))) continue;
        GetModuleBaseNameA(process, handles[i], m->name, sizeof(m->name));
        m->base = (DWORD64)info.lpBaseOfDll;
        m->size = info.SizeOfImage;
        module_count++;
    }
}

static void place(DWORD64 address, char *out, size_t size)
{
    for (int i = 0; i < module_count; i++)
        if (address >= modules[i].base && address < modules[i].base + modules[i].size)
        {
            snprintf(out, size, "%s+0x%llx", modules[i].name, (unsigned long long)(address - modules[i].base));
            return;
        }
    snprintf(out, size, "?0x%llx", (unsigned long long)address);
}

struct count { char where[96 * 9]; int n; };
static struct count counts[2][1024];
static int count_used[2];

static void add(int table, const char *where)
{
    for (int i = 0; i < count_used[table]; i++)
        if (!strcmp(counts[table][i].where, where)) { counts[table][i].n++; return; }
    if (count_used[table] < 1024)
    {
        strcpy(counts[table][count_used[table]].where, where);
        counts[table][count_used[table]++].n = 1;
    }
}

static int by_count(const void *a, const void *b) { return ((const struct count *)b)->n - ((const struct count *)a)->n; }

int main(int argc, char **argv)
{
    if (argc < 3) { printf("usage: guestsample <pid> <thread name> [samples] [interval ms]\n"); return 1; }
    /* a process id, or the name of its executable */
    DWORD pid = strtoul(argv[1], NULL, 0);
    if (!pid)
    {
        HANDLE procs = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        PROCESSENTRY32 pe = { sizeof(pe) };
        for (BOOL ok = Process32First(procs, &pe); ok && !pid; ok = Process32Next(procs, &pe))
            if (!_stricmp(pe.szExeFile, argv[1])) pid = pe.th32ProcessID;
        CloseHandle(procs);
    }
    int samples = argc > 3 ? atoi(argv[3]) : 500, interval = argc > 4 ? atoi(argv[4]) : 5;
    wchar_t want[128];
    MultiByteToWideChar(CP_UTF8, 0, argv[2], -1, want, 128);

    HANDLE process = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, pid);
    if (!process) { printf("OpenProcess failed (%lu)\n", GetLastError()); return 1; }
    load_modules(process);

    static HANDLE threads[512];
    static DWORD tids[512];
    int thread_count = 0, all = !wcscmp(want, L"all");
    DWORD want_tid = !wcsncmp(want, L"0x", 2) ? wcstoul(want, NULL, 16) : 0;
    BOOL wow64 = FALSE;
    IsWow64Process(process, &wow64);
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    THREADENTRY32 te = { sizeof(te) };
    for (BOOL ok = Thread32First(snap, &te); ok && thread_count < 512 && (all || !thread_count); ok = Thread32Next(snap, &te))
    {
        if (te.th32OwnerProcessID != pid) continue;
        HANDLE t = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_LIMITED_INFORMATION, FALSE, te.th32ThreadID);
        PWSTR name = NULL;
        if (t && SUCCEEDED(GetThreadDescription(t, &name)) && name && !wcscmp(want, L"list")) printf("%04lx %ls\n", te.th32ThreadID, name);
        if (t && (all || te.th32ThreadID == want_tid || (name && wcscmp(want, L"list") && !want_tid && wcsstr(name, want))))
        {
            tids[thread_count] = te.th32ThreadID;
            threads[thread_count++] = t;
        }
        else if (t) CloseHandle(t);
        if (name) LocalFree(name);
    }
    if (!thread_count) { printf("no thread like %s in %lu\n", argv[2], pid); return 1; }
    printf("process %lu (%04lx)%s, %d thread(s), first %04lx\n", pid, pid, wow64 ? " WoW64" : "", thread_count, tids[0]);

    for (int s = 0; s < samples; s++)
    {
        for (int k = 0; k < thread_count; k++)
        {
            HANDLE thread = threads[k];
            DWORD64 ip = 0, fp = 0;
            if (SuspendThread(thread) == (DWORD)-1) continue;
            if (wow64)
            {
                WOW64_CONTEXT context = { .ContextFlags = WOW64_CONTEXT_FULL };
                if (Wow64GetThreadContext(thread, &context)) { ip = context.Eip; fp = context.Ebp; }
            }
            else
            {
                CONTEXT context = { .ContextFlags = CONTEXT_FULL };
                if (GetThreadContext(thread, &context)) { ip = context.Rip; fp = context.Rbp; }
            }
            if (ip)
            {
                char where[96], prefixed[112];
                place(ip, where, sizeof(where));
                if (all) { snprintf(prefixed, sizeof(prefixed), "%04lx %s", tids[k], where); add(0, prefixed); }
                else add(0, where);
                /* the frame pointer chain, up to 8 callers, as one line */
                char chain[96 * 9] = "", one[96];
                SIZE_T got;
                for (int depth = 0; depth < 8 && fp; depth++)
                {
                    DWORD64 next, ret;
                    if (wow64)
                    {
                        DWORD frame[2];
                        if (!ReadProcessMemory(process, (void *)fp, frame, sizeof(frame), &got)) break;
                        next = frame[0]; ret = frame[1];
                    }
                    else
                    {
                        DWORD64 frame[2];
                        if (!ReadProcessMemory(process, (void *)fp, frame, sizeof(frame), &got)) break;
                        next = frame[0]; ret = frame[1];
                    }
                    place(ret, one, sizeof(one));
                    if (depth) strcat(chain, " < ");
                    strcat(chain, one);
                    if (next <= fp) break;
                    fp = next;
                }
                if (*chain && !all) add(1, chain);
            }
            ResumeThread(thread);
        }
        Sleep(interval);
    }
    const char *titles[2] = { "where it runs", "callers (frame pointer chain)" };
    for (int table = 0; table < 2; table++)
    {
        qsort(counts[table], count_used[table], sizeof(counts[table][0]), by_count);
        printf("== %s\n", titles[table]);
        for (int i = 0; i < count_used[table] && i < (all ? 400 : 25); i++) printf("%5d %s\n", counts[table][i].n, counts[table][i].where);
    }
    return 0;
}

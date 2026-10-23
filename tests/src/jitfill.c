/* archs: i686 x86_64 */
/* Fill the JIT code buffer with many distinct functions so FEX has to clear and reallocate it
 * mid-run, and check that the guest stack pointer survives every call. */
#include <windows.h>
#include <stdio.h>
#include <string.h>

typedef int (*func_t)(int);

static void *get_sp(void)
{
    return __builtin_frame_address(0);
}

static int run(int count);
static int cb_count, cb_result;

static LRESULT CALLBACK wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == WM_USER) { cb_result = run(cb_count); return 1; }
    return DefWindowProcA(hwnd, msg, wp, lp);
}

int main(int argc, char **argv)
{
    int count = argc > 1 ? atoi(argv[1]) : 400000;

    if (argc > 2 && !strcmp(argv[2], "cb"))
    {
        /* fill the code buffer from inside a user callback (nested JIT entry) */
        WNDCLASSA cls = { 0, wndproc, 0, 0, GetModuleHandleA(NULL), 0, 0, 0, 0, "jitfill" };
        HWND hwnd;
        int i, ret = 0;
        RegisterClassA(&cls);
        hwnd = CreateWindowA("jitfill", "", 0, 0, 0, 10, 10, HWND_MESSAGE, 0, 0, 0);
        for (i = 0; i < 3; i++)
        {
            void *sp = get_sp();
            cb_count = count;
            SendMessageA(hwnd, WM_USER, 0, 0);
            if (get_sp() != sp) { printf("stack pointer changed across callback: %p -> %p\n", sp, get_sp()); ret = 1; }
            ret |= cb_result;
        }
        printf("jitfill cb: %s\n", ret ? "FAILED" : "ok");
        return ret;
    }
    return run(count);
}

static int run(int count)
{
    int i, bad = 0;
    /* push ebp/rbp; mov eax, imm32; add eax, ecx/edi(arg); pop; ret: unique code per function */
    unsigned char *code = VirtualAlloc(NULL, (size_t)count * 16, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    long long sum = 0, expect = 0;

    if (!code) { printf("VirtualAlloc failed\n"); return 1; }
    memset(code, 0xcc, (size_t)count * 16);
    for (i = 0; i < count; i++)
    {
        unsigned char *p = code + (size_t)i * 16;
#ifdef _WIN64
        *p++ = 0x55;                                     /* push rbp */
        *p++ = 0xb8; memcpy( p, &i, 4 ); p += 4;         /* mov eax, i */
        *p++ = 0x01; *p++ = 0xc8;                        /* add eax, ecx */
        *p++ = 0x5d;                                     /* pop rbp */
        *p++ = 0xc3;                                     /* ret */
#else
        *p++ = 0x55;                                     /* push ebp */
        *p++ = 0xb8; memcpy( p, &i, 4 ); p += 4;         /* mov eax, i */
        *p++ = 0x03; *p++ = 0x44; *p++ = 0x24; *p++ = 0x08; /* add eax, [esp+8] */
        *p++ = 0x5d;                                     /* pop ebp */
        *p++ = 0xc3;                                     /* ret */
#endif
    }
    FlushInstructionCache(GetCurrentProcess(), code, (size_t)count * 16);

    for (int pass = 0; pass < 2; pass++)
    {
        for (i = 0; i < count; i++)
        {
            void *sp = get_sp();
            sum += ((func_t)(code + (size_t)i * 16))(1);
            expect += i + 1;
            if (get_sp() != sp && bad++ < 5) printf("stack pointer changed at %d: %p -> %p\n", i, sp, get_sp());
        }
    }
    printf("jitfill: %d functions x2, sum %s, %d stack errors\n", count, sum == expect ? "ok" : "WRONG", bad);
    VirtualFree(code, 0, MEM_RELEASE);
    return sum != expect || bad;
}

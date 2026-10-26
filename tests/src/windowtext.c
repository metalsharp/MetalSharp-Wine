/* Prints the windows of a process and the text of their child controls (a message box hidden behind a full-screen
 * window shows here).   windowtext <exe name> */
#include <windows.h>
#include <stdio.h>
#include <tlhelp32.h>

static DWORD pid;

static BOOL CALLBACK child(HWND hwnd, LPARAM depth)
{
    char cls[64], text[1024];
    GetClassNameA(hwnd, cls, sizeof(cls));
    GetWindowTextA(hwnd, text, sizeof(text));
    printf("    %s: %s\n", cls, text);
    return TRUE;
}

static BOOL CALLBACK top(HWND hwnd, LPARAM unused)
{
    DWORD owner;
    char cls[64], text[1024];
    GetWindowThreadProcessId(hwnd, &owner);
    if (owner != pid) return TRUE;
    GetClassNameA(hwnd, cls, sizeof(cls));
    GetWindowTextA(hwnd, text, sizeof(text));
    printf("window %p %s visible %d: %s\n", hwnd, cls, IsWindowVisible(hwnd), text);
    EnumChildWindows(hwnd, child, 0);
    return TRUE;
}

int main(int argc, char **argv)
{
    HANDLE procs = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    PROCESSENTRY32 pe = { sizeof(pe) };
    for (BOOL ok = Process32First(procs, &pe); ok && !pid; ok = Process32Next(procs, &pe))
        if (!_stricmp(pe.szExeFile, argv[1])) pid = pe.th32ProcessID;
    if (!pid) { printf("no %s\n", argv[1]); return 1; }
    EnumWindows(top, 0);
    return 0;
}

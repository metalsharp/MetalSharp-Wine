/* Dumps a range of another process's memory to a file, relative to one of its modules: code that is only
 * decrypted at run time (packed executables) can then be disassembled.
 *   memdump <pid | exe name> <module> <offset hex> <length hex> <output file> */
#include <windows.h>
#include <psapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <tlhelp32.h>

int main(int argc, char **argv)
{
    if (argc < 6) { printf("usage: memdump <pid|exe> <module> <offset> <length> <file>\n"); return 1; }
    DWORD pid = strtoul(argv[1], NULL, 0);
    if (!pid)
    {
        HANDLE procs = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        PROCESSENTRY32 pe = { sizeof(pe) };
        for (BOOL ok = Process32First(procs, &pe); ok && !pid; ok = Process32Next(procs, &pe))
            if (!_stricmp(pe.szExeFile, argv[1])) pid = pe.th32ProcessID;
        CloseHandle(procs);
    }
    HANDLE process = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, pid);
    if (!process) { printf("OpenProcess failed (%lu)\n", GetLastError()); return 1; }
    HMODULE modules[1024];
    DWORD needed;
    char name[MAX_PATH];
    BYTE *base = NULL;
    if (EnumProcessModulesEx(process, modules, sizeof(modules), &needed, LIST_MODULES_ALL))
        for (DWORD i = 0; i < needed / sizeof(HMODULE) && !base; i++)
            if (GetModuleBaseNameA(process, modules[i], name, sizeof(name)) && !_stricmp(name, argv[2])) base = (BYTE *)modules[i];
    if (!base) { printf("no module %s\n", argv[2]); return 1; }
    SIZE_T offset = strtoull(argv[3], NULL, 16), length = strtoull(argv[4], NULL, 16), got = 0;
    BYTE *buffer = malloc(length);
    if (!ReadProcessMemory(process, base + offset, buffer, length, &got)) { printf("read failed (%lu)\n", GetLastError()); return 1; }
    FILE *out = fopen(argv[5], "wb");
    fwrite(buffer, 1, got, out);
    fclose(out);
    printf("%s base %p: %#Ix bytes at +%#Ix\n", argv[2], base, got, offset);
    return 0;
}

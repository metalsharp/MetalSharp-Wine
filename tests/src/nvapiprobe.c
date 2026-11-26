/* archs: x86_64 */
/* nvapiprobe: loads System32's nvapi64.dll as NVIDIA's Streamline does and calls NvAPI_Initialize and
 * NvAPI_SYS_GetDriverAndBranchVersion through nvapi_QueryInterface */
#include <windows.h>
#include <stdio.h>

typedef void *(__cdecl *query_t)(unsigned int);
typedef int (__cdecl *init_t)(void);
typedef int (__cdecl *driver_t)(unsigned int *, char *);

int main(void)
{
    WCHAR path[MAX_PATH];
    HMODULE module;
    query_t query;

    GetSystemDirectoryW(path, MAX_PATH);
    lstrcatW(path, L"\\nvapi64.dll");
    if (!(module = LoadLibraryW(path)))
    {
        printf("LoadLibrary(%ls) failed, error %lu\n", path, GetLastError());
        return 1;
    }
    printf("loaded %ls\n", path);
    if (!(query = (query_t)GetProcAddress(module, "nvapi_QueryInterface")))
    {
        printf("no nvapi_QueryInterface\n");
        return 1;
    }
    {
        init_t init = (init_t)query(0x0150e828);
        driver_t driver = (driver_t)query(0x2926aaad);
        unsigned int version = 0;
        char branch[64] = {0};
        printf("NvAPI_Initialize %p -> %d\n", init, init ? init() : -999);
        printf("NvAPI_SYS_GetDriverAndBranchVersion %p -> %d, version %u, branch %s\n", driver,
               driver ? driver(&version, branch) : -999, version, branch);
    }
    return 0;
}

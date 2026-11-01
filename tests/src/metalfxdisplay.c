/* metalfxdisplay: prints the display a program sees (screen metrics, the primary monitor, the current, registry
 * and largest modes, DXGI's output, the adapter's LUID), to check that a MetalFX game is shown the display divided by its factor */
#define COBJMACROS
#define INITGUID
#include <windows.h>
#include <dxgi.h>
#include <stdio.h>

int main(void)
{
    DEVMODEW cur = {.dmSize = sizeof(cur)}, reg = {.dmSize = sizeof(reg)}, mode = {.dmSize = sizeof(mode)};
    MONITORINFO info = {.cbSize = sizeof(info)};
    DWORD i, max_w = 0, max_h = 0, count = 0;
    IDXGIFactory1 *factory;
    IDXGIAdapter1 *adapter;
    IDXGIOutput *output;
    HWND hwnd;
    RECT rect;

    printf("metrics %dx%d, virtual %dx%d\n", GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN),
           GetSystemMetrics(SM_CXVIRTUALSCREEN), GetSystemMetrics(SM_CYVIRTUALSCREEN));
    GetMonitorInfoW(MonitorFromPoint((POINT){0, 0}, MONITOR_DEFAULTTOPRIMARY), &info);
    printf("monitor %ldx%ld\n", info.rcMonitor.right - info.rcMonitor.left, info.rcMonitor.bottom - info.rcMonitor.top);
    EnumDisplaySettingsW(NULL, ENUM_CURRENT_SETTINGS, &cur);
    EnumDisplaySettingsW(NULL, ENUM_REGISTRY_SETTINGS, &reg);
    for (i = 0; EnumDisplaySettingsW(NULL, i, &mode); i++, count++)
        if (mode.dmPelsWidth * mode.dmPelsHeight > max_w * max_h) max_w = mode.dmPelsWidth, max_h = mode.dmPelsHeight;
    printf("current %lux%lu, registry %lux%lu, largest of %lu modes %lux%lu\n", cur.dmPelsWidth, cur.dmPelsHeight,
           reg.dmPelsWidth, reg.dmPelsHeight, count, max_w, max_h);
    if (SUCCEEDED(CreateDXGIFactory1(&IID_IDXGIFactory1, (void **)&factory)) &&
        SUCCEEDED(IDXGIFactory1_EnumAdapters1(factory, 0, &adapter)) &&
        SUCCEEDED(IDXGIAdapter1_EnumOutputs(adapter, 0, &output)))
    {
        DXGI_OUTPUT_DESC desc;
        IDXGIOutput_GetDesc(output, &desc);
        printf("dxgi output %ldx%ld\n", desc.DesktopCoordinates.right - desc.DesktopCoordinates.left,
               desc.DesktopCoordinates.bottom - desc.DesktopCoordinates.top);
    }
    {
        struct { WCHAR DeviceName[32]; UINT hAdapter; LUID AdapterLuid; UINT VidPnSourceId; } open = {0};
        LONG (WINAPI *open_adapter)(void *) = (void *)GetProcAddress(GetModuleHandleA("gdi32.dll"),
                                                                     "D3DKMTOpenAdapterFromGdiDisplayName");
        MONITORINFOEXW mi = {.cbSize = sizeof(mi)};
        GetMonitorInfoW(MonitorFromPoint((POINT){0, 0}, MONITOR_DEFAULTTOPRIMARY), (MONITORINFO *)&mi);
        lstrcpyW(open.DeviceName, mi.szDevice);
        if (open_adapter && !open_adapter(&open))
            printf("adapter luid %#lx:%#lx\n", open.AdapterLuid.HighPart, open.AdapterLuid.LowPart);
    }
    hwnd = CreateWindowExW(0, L"static", L"metalfx", WS_POPUP | WS_VISIBLE, 0, 0, info.rcMonitor.right,
                           info.rcMonitor.bottom, NULL, NULL, NULL, NULL);
    GetClientRect(hwnd, &rect);
    printf("fullscreen window client %ldx%ld\n", rect.right, rect.bottom);
    DestroyWindow(hwnd);
    return 0;
}

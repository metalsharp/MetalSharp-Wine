/* D3D11 smoke test: device + swap chain on a window, clear and present a few frames, read
 * back the adapter and feature level. Run with DXMT (WINEDLLPATH) or Wine's wined3d. */
#define COBJMACROS
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <stdio.h>

int main(void)
{
    static const D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0 };
    DXGI_SWAP_CHAIN_DESC sd = { 0 };
    ID3D11Device *device = NULL;
    ID3D11DeviceContext *ctx = NULL;
    IDXGISwapChain *swap = NULL;
    ID3D11Texture2D *back = NULL;
    ID3D11RenderTargetView *rtv = NULL;
    IDXGIDevice *dxgi_dev = NULL;
    IDXGIAdapter *adapter = NULL;
    DXGI_ADAPTER_DESC desc;
    D3D_FEATURE_LEVEL level;
    HMODULE mod;
    WCHAR path[MAX_PATH];
    HWND hwnd;
    HRESULT hr;
    int i;

    hwnd = CreateWindowA( "static", "d3d11info", WS_OVERLAPPEDWINDOW | WS_VISIBLE, 100, 100, 320, 240, 0, 0, 0, 0 );
    sd.BufferCount = 2;
    sd.BufferDesc.Width = 320;
    sd.BufferDesc.Height = 240;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hwnd;
    sd.SampleDesc.Count = 1;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;

    hr = D3D11CreateDeviceAndSwapChain( NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 0, levels, ARRAYSIZE(levels),
                                        D3D11_SDK_VERSION, &sd, &swap, &device, &level, &ctx );
    if ((mod = GetModuleHandleA( "d3d11.dll" )) && GetModuleFileNameW( mod, path, MAX_PATH ))
        printf( "d3d11.dll: %ls\n", path );
    if (FAILED(hr)) { printf( "D3D11CreateDeviceAndSwapChain failed: %#lx\n", hr ); return 1; }

    if (SUCCEEDED(ID3D11Device_QueryInterface( device, &IID_IDXGIDevice, (void **)&dxgi_dev )) &&
        SUCCEEDED(IDXGIDevice_GetAdapter( dxgi_dev, &adapter )) && SUCCEEDED(IDXGIAdapter_GetDesc( adapter, &desc )))
        printf( "adapter: %ls (vendor %04x, %u MB)\n", desc.Description, desc.VendorId, (unsigned)(desc.DedicatedVideoMemory >> 20) );
    printf( "feature level: %x.%x\n", level >> 12, (level >> 8) & 0xf );

    hr = IDXGISwapChain_GetBuffer( swap, 0, &IID_ID3D11Texture2D, (void **)&back );
    if (SUCCEEDED(hr)) hr = ID3D11Device_CreateRenderTargetView( device, (ID3D11Resource *)back, NULL, &rtv );
    for (i = 0; SUCCEEDED(hr) && i < 30; i++)
    {
        const float color[4] = { 0.1f, 0.4f * (i % 2), 0.8f, 1.0f };
        ID3D11DeviceContext_OMSetRenderTargets( ctx, 1, &rtv, NULL );
        ID3D11DeviceContext_ClearRenderTargetView( ctx, rtv, color );
        hr = IDXGISwapChain_Present( swap, 1, 0 );
    }
    printf( "30 frames presented: %s (%#lx)\n", SUCCEEDED(hr) ? "ok" : "FAILED", hr );
    return FAILED(hr);
}

#define COBJMACROS
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <stdio.h>

int main(void)
{
    D3D11_TEXTURE2D_DESC desc = {0};
    D3D_FEATURE_LEVEL level;
    ID3D11Texture2D *texture = NULL, *roundtrip = NULL;
    ID3D11DeviceContext *context = NULL;
    IDXGISurface1 *surface = NULL;
    IUnknown *texture_identity = NULL, *surface_identity = NULL;
    ID3D11Device *device = NULL;
    DXGI_SURFACE_DESC surface_desc;
    HDC dc;
    HRESULT hr;
    int failures = 0;

    hr = D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL,
            D3D11_CREATE_DEVICE_BGRA_SUPPORT, NULL, 0, D3D11_SDK_VERSION,
            &device, &level, &context);
    if (FAILED(hr))
    {
        printf("SKIP D3D11CreateDevice %#lx\n", (unsigned long)hr);
        return 77;
    }
    desc.Width = 8;
    desc.Height = 8;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_RENDER_TARGET;
    desc.MiscFlags = D3D11_RESOURCE_MISC_GDI_COMPATIBLE;
    hr = ID3D11Device_CreateTexture2D(device, &desc, NULL, &texture);
    if (FAILED(hr)) ++failures;
    if (!failures && FAILED(ID3D11Texture2D_QueryInterface(texture,
            &IID_IDXGISurface1, (void **)&surface))) ++failures;
    if (!failures && FAILED(IDXGISurface1_GetDesc(surface, &surface_desc))) ++failures;
    if (!failures && (surface_desc.Width != 8 || surface_desc.Height != 8)) ++failures;
    if (!failures && FAILED(ID3D11Texture2D_QueryInterface(texture,
            &IID_IUnknown, (void **)&texture_identity))) ++failures;
    if (!failures && FAILED(IDXGISurface1_QueryInterface(surface,
            &IID_IUnknown, (void **)&surface_identity))) ++failures;
    if (!failures && texture_identity != surface_identity) ++failures;
    if (!failures && FAILED(IDXGISurface1_QueryInterface(surface,
            &IID_ID3D11Texture2D, (void **)&roundtrip))) ++failures;
    if (roundtrip) ID3D11Texture2D_Release(roundtrip);
    if (!failures && FAILED(IDXGISurface1_GetDC(surface, TRUE, &dc))) ++failures;
    if (!failures)
    {
        SetPixel(dc, 2, 3, RGB(0x11, 0x22, 0x33));
        if (FAILED(IDXGISurface1_ReleaseDC(surface, NULL))) ++failures;
    }
    if (!failures && FAILED(IDXGISurface1_GetDC(surface, FALSE, &dc))) ++failures;
    if (!failures && GetPixel(dc, 2, 3) != RGB(0x11, 0x22, 0x33)) ++failures;
    if (!failures && FAILED(IDXGISurface1_ReleaseDC(surface, NULL))) ++failures;
    if (surface_identity) IUnknown_Release(surface_identity);
    if (texture_identity) IUnknown_Release(texture_identity);
    if (texture) ID3D11Texture2D_Release(texture);
    if (surface) IDXGISurface1_Release(surface);
    if (context) ID3D11DeviceContext_Release(context);
    if (device) ID3D11Device_Release(device);
    printf("%s d3d11_gdi_surface_unit\n", failures ? "FAIL" : "PASS");
    return failures != 0;
}

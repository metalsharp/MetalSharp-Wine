/* D3D9 smoke test (wined3d over OpenGL): HAL device on a window, clear and present 30 frames,
 * read a pixel of the back buffer back. */
#define COBJMACROS
#include <windows.h>
#include <d3d9.h>
#include <stdio.h>

int main(void)
{
    D3DPRESENT_PARAMETERS pp = { 0 };
    D3DADAPTER_IDENTIFIER9 id;
    IDirect3D9 *d3d = Direct3DCreate9( D3D_SDK_VERSION );
    IDirect3DDevice9 *dev = NULL;
    IDirect3DSurface9 *back = NULL, *sys = NULL;
    D3DLOCKED_RECT lr;
    D3DCAPS9 caps;
    HWND hwnd;
    HRESULT hr;
    DWORD pixel = 0;
    int i;

    if (!d3d) { printf( "Direct3DCreate9 failed\n" ); return 1; }
    IDirect3D9_GetAdapterIdentifier( d3d, 0, 0, &id );
    printf( "adapter: %s (%s)\n", id.Description, id.Driver );
    hwnd = CreateWindowA( "static", "d3d9info", WS_OVERLAPPEDWINDOW | WS_VISIBLE, 100, 100, 320, 240, 0, 0, 0, 0 );
    pp.Windowed = TRUE;
    pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
    pp.BackBufferFormat = D3DFMT_X8R8G8B8;
    pp.BackBufferWidth = 320;
    pp.BackBufferHeight = 240;
    hr = IDirect3D9_CreateDevice( d3d, 0, D3DDEVTYPE_HAL, hwnd, D3DCREATE_HARDWARE_VERTEXPROCESSING, &pp, &dev );
    if (FAILED(hr)) { printf( "CreateDevice failed: %#lx\n", hr ); return 1; }
    IDirect3DDevice9_GetDeviceCaps( dev, &caps );
    printf( "shader model: vs %lu.%lu ps %lu.%lu\n", (caps.VertexShaderVersion >> 8) & 0xff, caps.VertexShaderVersion & 0xff,
            (caps.PixelShaderVersion >> 8) & 0xff, caps.PixelShaderVersion & 0xff );
    for (i = 0; i < 30 && SUCCEEDED(hr); i++)
    {
        hr = IDirect3DDevice9_Clear( dev, 0, NULL, D3DCLEAR_TARGET, D3DCOLOR_XRGB( 51, 153, 51 ), 1.0f, 0 );
        if (i == 29 && SUCCEEDED(IDirect3DDevice9_GetBackBuffer( dev, 0, 0, D3DBACKBUFFER_TYPE_MONO, &back )) &&
            SUCCEEDED(IDirect3DDevice9_CreateOffscreenPlainSurface( dev, 320, 240, D3DFMT_X8R8G8B8, D3DPOOL_SYSTEMMEM, &sys, NULL )) &&
            SUCCEEDED(IDirect3DDevice9_GetRenderTargetData( dev, back, sys )) &&
            SUCCEEDED(IDirect3DSurface9_LockRect( sys, &lr, NULL, D3DLOCK_READONLY )))
        {
            pixel = ((DWORD *)lr.pBits)[10 * lr.Pitch / 4 + 10] & 0xffffff;
            IDirect3DSurface9_UnlockRect( sys );
        }
        if (SUCCEEDED(hr)) hr = IDirect3DDevice9_Present( dev, NULL, NULL, NULL, NULL );
    }
    printf( "30 frames presented: %s (%#lx), pixel %06lx (expect 339933)\n", SUCCEEDED(hr) ? "ok" : "FAILED", hr, pixel );
    return FAILED(hr) || pixel != 0x339933;
}

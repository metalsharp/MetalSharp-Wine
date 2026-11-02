/* ngxtest11: drives the NGX core (nvngx.dll) as a D3D11 game does: DLSS Super Resolution on cleared inputs, and
 * checks that the output is the input's color, upscaled (MetalFX through DXVK's ID3D11VkMetalFXContext). Run on the
 * DXVK lane (GRAPHICS_BACKEND=dxvk) with DXVK_CONFIG showing an NVIDIA GPU (dxgi.customVendorId=10de).
 * Built with -I <VKD3D-Proton-MacOS>/ngx/nvngx for ngx.hpp. */
#include <windows.h>
#include <d3d11.h>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include "ngx.hpp"

typedef NVNGX_RESULT (*init_t)(unsigned long long, const wchar_t *, ID3D11Device *, const void *, unsigned int);
typedef NVNGX_RESULT (*alloc_t)(NVNGXParameter **);
typedef NVNGX_RESULT (*create_t)(ID3D11DeviceContext *, unsigned int, NVNGXParameter *, unsigned int **);
typedef NVNGX_RESULT (*eval_t)(ID3D11DeviceContext *, const unsigned int *, NVNGXParameter *, void *);
typedef NVNGX_RESULT (*release_t)(unsigned int *);

static ID3D11Device *device;
static ID3D11DeviceContext *context;

static ID3D11Texture2D *texture(UINT width, UINT height, DXGI_FORMAT format, UINT bind)
{
    D3D11_TEXTURE2D_DESC desc = {width, height, 1, 1, format, {1, 0}, D3D11_USAGE_DEFAULT, bind, 0, 0};
    ID3D11Texture2D *tex = nullptr;
    if (FAILED(device->CreateTexture2D(&desc, nullptr, &tex)))
        printf("CreateTexture2D %ux%u format %u failed\n", width, height, format);
    return tex;
}

static void clear(ID3D11Texture2D *tex, float r, float g, float b, float a)
{
    ID3D11RenderTargetView *rtv = nullptr;
    const float color[4] = {r, g, b, a};
    device->CreateRenderTargetView(tex, nullptr, &rtv);
    context->ClearRenderTargetView(rtv, color);
    rtv->Release();
}

static bool read_center(ID3D11Texture2D *tex, UINT width, UINT height, float rgba[4])
{
    D3D11_TEXTURE2D_DESC desc;
    tex->GetDesc(&desc);
    desc.Usage = D3D11_USAGE_STAGING;
    desc.BindFlags = 0;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ID3D11Texture2D *staging = nullptr;
    D3D11_MAPPED_SUBRESOURCE map;
    if (FAILED(device->CreateTexture2D(&desc, nullptr, &staging)))
        return false;
    context->CopyResource(staging, tex);
    if (FAILED(context->Map(staging, 0, D3D11_MAP_READ, 0, &map)))
        return false;
    const uint16_t *pixel = (const uint16_t *)((const char *)map.pData + map.RowPitch * (height / 2)) + 4 * (width / 2);
    for (int i = 0; i < 4; i++)
    {
        uint16_t h = pixel[i];
        int exponent = (h >> 10) & 0x1f, mantissa = h & 0x3ff;
        rgba[i] = exponent ? std::ldexp(1.0f + mantissa / 1024.0f, exponent - 15) : std::ldexp(mantissa / 1024.0f, -14);
    }
    context->Unmap(staging, 0);
    staging->Release();
    return true;
}

int main()
{
    const UINT in_w = 960, in_h = 540, out_w = 1920, out_h = 1080;
    if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION,
                                 &device, nullptr, &context)))
    {
        printf("no D3D11 device\n");
        return 1;
    }
    HMODULE ngx = LoadLibraryW(L"nvngx.dll");
    if (!ngx)
    {
        printf("no nvngx.dll (error %lu)\n", GetLastError());
        return 1;
    }
    auto init = (init_t)GetProcAddress(ngx, "NVSDK_NGX_D3D11_Init");
    auto allocate = (alloc_t)GetProcAddress(ngx, "NVSDK_NGX_D3D11_AllocateParameters");
    auto create = (create_t)GetProcAddress(ngx, "NVSDK_NGX_D3D11_CreateFeature");
    auto evaluate = (eval_t)GetProcAddress(ngx, "NVSDK_NGX_D3D11_EvaluateFeature");
    auto release = (release_t)GetProcAddress(ngx, "NVSDK_NGX_D3D11_ReleaseFeature");
    if (!init || !allocate || !create || !evaluate || !release)
    {
        printf("NGX D3D11 entry points missing\n");
        return 1;
    }
    printf("NVSDK_NGX_D3D11_Init -> %#x\n", init(1, L".", device, nullptr, 0x15));

    auto color = texture(in_w, in_h, DXGI_FORMAT_R16G16B16A16_FLOAT, D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE);
    auto motion = texture(in_w, in_h, DXGI_FORMAT_R16G16_FLOAT, D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE);
    auto depth = texture(in_w, in_h, DXGI_FORMAT_R32_TYPELESS, D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE);
    auto output = texture(out_w, out_h, DXGI_FORMAT_R16G16B16A16_FLOAT, D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE);
    if (!color || !motion || !depth || !output)
        return 1;
    D3D11_DEPTH_STENCIL_VIEW_DESC dsv_desc = {DXGI_FORMAT_D32_FLOAT, D3D11_DSV_DIMENSION_TEXTURE2D};
    ID3D11DepthStencilView *dsv = nullptr;
    device->CreateDepthStencilView(depth, &dsv_desc, &dsv);
    clear(color, 0.25f, 0.5f, 0.75f, 1.0f);
    clear(motion, 0, 0, 0, 0);
    context->ClearDepthStencilView(dsv, D3D11_CLEAR_DEPTH, 0.5f, 0);

    NVNGXParameter *params;
    allocate(&params);
    params->Set("Width", in_w);
    params->Set("Height", in_h);
    params->Set("OutWidth", out_w);
    params->Set("OutHeight", out_h);
    params->Set("PerfQualityValue", 0);
    params->Set("DLSS.Feature.Create.Flags", int(NVNGX_DLSS_FLAG_MV_LOWRES));
    unsigned int *handle = nullptr;
    NVNGX_RESULT result = create(context, 1, params, &handle);
    printf("Super Resolution: CreateFeature -> %#x\n", result);
    if (NVNGX_FAILED(result))
        return 1;
    params->Set("Color", (ID3D11Resource *)color);
    params->Set("Output", (ID3D11Resource *)output);
    params->Set("Depth", (ID3D11Resource *)depth);
    params->Set("MotionVectors", (ID3D11Resource *)motion);
    params->Set("Reset", 1);
    params->Set("DLSS.Render.Subrect.Dimensions.Width", in_w);
    params->Set("DLSS.Render.Subrect.Dimensions.Height", in_h);
    for (int frame = 0; frame < 3; frame++)
    {
        result = evaluate(context, handle, params, nullptr);
        params->Set("Reset", 0);
    }
    printf("Super Resolution: EvaluateFeature -> %#x\n", result);
    float rgba[4] = {};
    bool ok = read_center(output, out_w, out_h, rgba) && !(NVNGX_FAILED(result)) && std::fabs(rgba[0] - 0.25f) < 0.1f &&
              std::fabs(rgba[1] - 0.5f) < 0.1f && std::fabs(rgba[2] - 0.75f) < 0.1f;
    printf("Super Resolution: output center %.3f %.3f %.3f %.3f (expect about 0.25 0.5 0.75): %s\n", rgba[0], rgba[1],
           rgba[2], rgba[3], ok ? "ok" : "FAILED");
    release(handle);
    printf("%d failed\n", !ok);
    return !ok;
}

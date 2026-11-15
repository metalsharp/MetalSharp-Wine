/*
 * MetalSharp D3D11 frontend for D3DMetal: GenerateMips with the D3D11 runtime's checks.
 *
 * The D3D11 runtime only generates mips for a view of a resource created with
 * D3D11_RESOURCE_MISC_GENERATE_MIPS and D3D11_BIND_RENDER_TARGET, and ignores the call
 * otherwise. D3DMetal runs Metal's mipmap blit whatever the resource, which fails for
 * block-compressed textures and leaves their mip levels undefined. Assetto Corsa's Custom
 * Shaders Patch calls GenerateMips on BC1/BC2/BC3 track textures, and they rendered with
 * black and mirror-like squares. This frontend loads D3DMetal's d3d11 under a private alias
 * and puts the runtime's check in front of GenerateMips on the contexts it creates.
 *
 * This library is free software; you can redistribute it and/or modify it under the terms of
 * the GNU Lesser General Public License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#define COBJMACROS
#include <windows.h>
#include <d3d11_4.h>
#include <stddef.h>
#include <stdio.h>

#define SLOT(type, member) (offsetof(type, member) / sizeof(void *))

typedef void (STDMETHODCALLTYPE *generate_mips_fn)(ID3D11DeviceContext *, ID3D11ShaderResourceView *);
typedef HRESULT (STDMETHODCALLTYPE *create_deferred_fn)(ID3D11Device *, UINT, ID3D11DeviceContext **);

struct hook { void **vtable; void *original; };

static SRWLOCK lock = SRWLOCK_INIT;
static struct hook tex1d_hooks[16], tex2d_hooks[16], tex3d_hooks[16], tex2d1_hooks[16], tex3d1_hooks[16];
static struct hook mips_hooks[16], deferred_hooks[16], deferred1_hooks[16], deferred2_hooks[16], deferred3_hooks[16], ctxqi_hooks[16], devqi_hooks[16];
static LONG rejected;
static HMODULE backend;
static PFN_D3D11_CREATE_DEVICE backend_create_device;
static PFN_D3D11_CREATE_DEVICE_AND_SWAP_CHAIN backend_create_device_and_swap_chain;
static LONG skipped;

static void *find_original(struct hook *hooks, void *object)
{
    void **vtable = *(void ***)object, *original = NULL;
    unsigned int i;

    AcquireSRWLockShared(&lock);
    for (i = 0; i < 16 && hooks[i].vtable; ++i)
        if (hooks[i].vtable == vtable) { original = hooks[i].original; break; }
    ReleaseSRWLockShared(&lock);
    return original;
}

static void patch(struct hook *hooks, void *object, size_t slot, void *replacement)
{
    void **vtable = *(void ***)object;
    DWORD old;
    unsigned int i;

    AcquireSRWLockExclusive(&lock);
    for (i = 0; i < 16 && hooks[i].vtable; ++i)
        if (hooks[i].vtable == vtable) goto done;
    if (i == 16 || vtable[slot] == replacement) goto done;
    if (!VirtualProtect(&vtable[slot], sizeof(void *), PAGE_READWRITE, &old)) goto done;
    hooks[i].vtable = vtable;
    hooks[i].original = vtable[slot];
    vtable[slot] = replacement;
    VirtualProtect(&vtable[slot], sizeof(void *), old, &old);
done:
    ReleaseSRWLockExclusive(&lock);
}

static BOOL generate_mips_allowed(ID3D11ShaderResourceView *view)
{
    D3D11_RESOURCE_DIMENSION dimension;
    UINT bind = 0, misc = 0;
    ID3D11Resource *resource;

    if (!view) return FALSE;
    ID3D11ShaderResourceView_GetResource(view, &resource);
    if (!resource) return FALSE;
    ID3D11Resource_GetType(resource, &dimension);
    switch (dimension)
    {
        case D3D11_RESOURCE_DIMENSION_TEXTURE1D:
        {
            D3D11_TEXTURE1D_DESC desc;
            ID3D11Texture1D_GetDesc((ID3D11Texture1D *)resource, &desc);
            bind = desc.BindFlags; misc = desc.MiscFlags;
            break;
        }
        case D3D11_RESOURCE_DIMENSION_TEXTURE2D:
        {
            D3D11_TEXTURE2D_DESC desc;
            ID3D11Texture2D_GetDesc((ID3D11Texture2D *)resource, &desc);
            bind = desc.BindFlags; misc = desc.MiscFlags;
            break;
        }
        case D3D11_RESOURCE_DIMENSION_TEXTURE3D:
        {
            D3D11_TEXTURE3D_DESC desc;
            ID3D11Texture3D_GetDesc((ID3D11Texture3D *)resource, &desc);
            bind = desc.BindFlags; misc = desc.MiscFlags;
            break;
        }
        default:
            break;
    }
    ID3D11Resource_Release(resource);
    return (misc & D3D11_RESOURCE_MISC_GENERATE_MIPS) && (bind & D3D11_BIND_RENDER_TARGET)
            && (bind & D3D11_BIND_SHADER_RESOURCE);
}

static void STDMETHODCALLTYPE guarded_generate_mips(ID3D11DeviceContext *context, ID3D11ShaderResourceView *view)
{
    generate_mips_fn original = find_original(mips_hooks, context);

    if (!generate_mips_allowed(view))
    {
        if (InterlockedIncrement(&skipped) == 1)
            OutputDebugStringA("d3d11 frontend: ignoring GenerateMips on a resource without GENERATE_MIPS\n");
        return;
    }
    if (original) original(context, view);
}

static HRESULT STDMETHODCALLTYPE guarded_context_qi(IUnknown *iface, REFIID iid, void **out);

static void wrap_context(ID3D11DeviceContext *context)
{
    if (!context) return;
    patch(mips_hooks, context, SLOT(ID3D11DeviceContextVtbl, GenerateMips), guarded_generate_mips);
    patch(ctxqi_hooks, context, SLOT(ID3D11DeviceContextVtbl, QueryInterface), guarded_context_qi);
}

static HRESULT STDMETHODCALLTYPE guarded_create_deferred_context(ID3D11Device *device, UINT flags,
        ID3D11DeviceContext **context)
{
    create_deferred_fn original = find_original(deferred_hooks, device);
    HRESULT hr;

    if (!original) return E_FAIL;
    hr = original(device, flags, context);
    if (SUCCEEDED(hr) && context) wrap_context(*context);
    return hr;
}

typedef HRESULT (STDMETHODCALLTYPE *qi_fn)(IUnknown *, REFIID, void **);
typedef HRESULT (STDMETHODCALLTYPE *create_deferred1_fn)(void *, UINT, void **);

static BOOL is_context_iid(REFIID iid)
{
    return IsEqualGUID(iid, &IID_ID3D11DeviceContext) || IsEqualGUID(iid, &IID_ID3D11DeviceContext1) ||
           IsEqualGUID(iid, &IID_ID3D11DeviceContext2) || IsEqualGUID(iid, &IID_ID3D11DeviceContext3) ||
           IsEqualGUID(iid, &IID_ID3D11DeviceContext4);
}

static void wrap_context(ID3D11DeviceContext *context);
static void wrap_device(ID3D11Device *device, ID3D11DeviceContext *immediate);

static HRESULT STDMETHODCALLTYPE guarded_context_qi(IUnknown *iface, REFIID iid, void **out)
{
    qi_fn original = find_original(ctxqi_hooks, iface);
    HRESULT hr = original ? original(iface, iid, out) : E_NOINTERFACE;

    if (SUCCEEDED(hr) && out && *out && is_context_iid(iid) && !find_original(mips_hooks, *out)) wrap_context(*out);
    return hr;
}

static HRESULT STDMETHODCALLTYPE guarded_device_qi(IUnknown *iface, REFIID iid, void **out)
{
    qi_fn original = find_original(devqi_hooks, iface);
    HRESULT hr = original ? original(iface, iid, out) : E_NOINTERFACE;

    if (SUCCEEDED(hr) && out && *out &&
        (IsEqualGUID(iid, &IID_ID3D11Device) || IsEqualGUID(iid, &IID_ID3D11Device1) ||
         IsEqualGUID(iid, &IID_ID3D11Device2) || IsEqualGUID(iid, &IID_ID3D11Device3) ||
         IsEqualGUID(iid, &IID_ID3D11Device4) || IsEqualGUID(iid, &IID_ID3D11Device5)) &&
        !find_original(devqi_hooks, *out))
        wrap_device(*out, NULL);
    return hr;
}

#define DEFERRED_N(n, hooks) \
static HRESULT STDMETHODCALLTYPE guarded_create_deferred_context##n(void *device, UINT flags, void **context) \
{ \
    create_deferred1_fn original = find_original(hooks, device); \
    HRESULT hr = original ? original(device, flags, context) : E_FAIL; \
    if (SUCCEEDED(hr) && context && *context) wrap_context(*context); \
    return hr; \
}
DEFERRED_N(1, deferred1_hooks)
DEFERRED_N(2, deferred2_hooks)
DEFERRED_N(3, deferred3_hooks)

/* The D3D11 runtime refuses block-compressed resources that would be rendered to, written by
 * compute or given generated mips; D3DMetal creates them, so applications that try such a
 * texture first and fall back otherwise (Custom Shaders Patch) never fall back. */
static BOOL is_block_compressed(DXGI_FORMAT format)
{
    return (format >= DXGI_FORMAT_BC1_TYPELESS && format <= DXGI_FORMAT_BC5_SNORM) ||
           (format >= DXGI_FORMAT_BC6H_TYPELESS && format <= DXGI_FORMAT_BC7_UNORM_SRGB);
}

static BOOL invalid_bc_usage(DXGI_FORMAT format, UINT bind, UINT misc)
{
    if (!is_block_compressed(format)) return FALSE;
    if (!(bind & (D3D11_BIND_RENDER_TARGET | D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_UNORDERED_ACCESS))
            && !(misc & D3D11_RESOURCE_MISC_GENERATE_MIPS)) return FALSE;
    if (InterlockedIncrement(&rejected) == 1)
        OutputDebugStringA("d3d11 frontend: refusing a block-compressed render target, as D3D11 does\n");
    return TRUE;
}

typedef HRESULT (STDMETHODCALLTYPE *create_tex_fn)(void *, const void *, const D3D11_SUBRESOURCE_DATA *, void **);

static HRESULT STDMETHODCALLTYPE guarded_create_texture1d(void *device, const D3D11_TEXTURE1D_DESC *desc,
        const D3D11_SUBRESOURCE_DATA *data, void **texture)
{
    create_tex_fn original = find_original(tex1d_hooks, device);
    if (desc && invalid_bc_usage(desc->Format, desc->BindFlags, desc->MiscFlags)) return E_INVALIDARG;
    return original ? original(device, desc, data, texture) : E_FAIL;
}

static HRESULT STDMETHODCALLTYPE guarded_create_texture2d(void *device, const D3D11_TEXTURE2D_DESC *desc,
        const D3D11_SUBRESOURCE_DATA *data, void **texture)
{
    create_tex_fn original = find_original(tex2d_hooks, device);
    if (desc && invalid_bc_usage(desc->Format, desc->BindFlags, desc->MiscFlags)) return E_INVALIDARG;
    return original ? original(device, desc, data, texture) : E_FAIL;
}

static HRESULT STDMETHODCALLTYPE guarded_create_texture3d(void *device, const D3D11_TEXTURE3D_DESC *desc,
        const D3D11_SUBRESOURCE_DATA *data, void **texture)
{
    create_tex_fn original = find_original(tex3d_hooks, device);
    if (desc && invalid_bc_usage(desc->Format, desc->BindFlags, desc->MiscFlags)) return E_INVALIDARG;
    return original ? original(device, desc, data, texture) : E_FAIL;
}

static HRESULT STDMETHODCALLTYPE guarded_create_texture2d1(void *device, const D3D11_TEXTURE2D_DESC1 *desc,
        const D3D11_SUBRESOURCE_DATA *data, void **texture)
{
    create_tex_fn original = find_original(tex2d1_hooks, device);
    if (desc && invalid_bc_usage(desc->Format, desc->BindFlags, desc->MiscFlags)) return E_INVALIDARG;
    return original ? original(device, desc, data, texture) : E_FAIL;
}

static HRESULT STDMETHODCALLTYPE guarded_create_texture3d1(void *device, const D3D11_TEXTURE3D_DESC1 *desc,
        const D3D11_SUBRESOURCE_DATA *data, void **texture)
{
    create_tex_fn original = find_original(tex3d1_hooks, device);
    if (desc && invalid_bc_usage(desc->Format, desc->BindFlags, desc->MiscFlags)) return E_INVALIDARG;
    return original ? original(device, desc, data, texture) : E_FAIL;
}

static void wrap_device(ID3D11Device *device, ID3D11DeviceContext *immediate)
{
    ID3D11DeviceContext *context = immediate;

    if (!device) return;
    patch(deferred_hooks, device, SLOT(ID3D11DeviceVtbl, CreateDeferredContext), guarded_create_deferred_context);
    patch(tex1d_hooks, device, SLOT(ID3D11DeviceVtbl, CreateTexture1D), guarded_create_texture1d);
    patch(tex2d_hooks, device, SLOT(ID3D11DeviceVtbl, CreateTexture2D), guarded_create_texture2d);
    patch(tex3d_hooks, device, SLOT(ID3D11DeviceVtbl, CreateTexture3D), guarded_create_texture3d);
    {
        ID3D11Device3 *device3;
        if (SUCCEEDED(ID3D11Device_QueryInterface(device, &IID_ID3D11Device3, (void **)&device3)))
        {
            patch(deferred1_hooks, device3, SLOT(ID3D11Device3Vtbl, CreateDeferredContext1), guarded_create_deferred_context1);
            patch(deferred2_hooks, device3, SLOT(ID3D11Device3Vtbl, CreateDeferredContext2), guarded_create_deferred_context2);
            patch(deferred3_hooks, device3, SLOT(ID3D11Device3Vtbl, CreateDeferredContext3), guarded_create_deferred_context3);
            patch(tex2d_hooks, device3, SLOT(ID3D11Device3Vtbl, CreateTexture2D), guarded_create_texture2d);
            patch(tex2d1_hooks, device3, SLOT(ID3D11Device3Vtbl, CreateTexture2D1), guarded_create_texture2d1);
            patch(tex3d1_hooks, device3, SLOT(ID3D11Device3Vtbl, CreateTexture3D1), guarded_create_texture3d1);
            ID3D11Device3_Release(device3);
        }
    }
    patch(devqi_hooks, device, SLOT(ID3D11DeviceVtbl, QueryInterface), guarded_device_qi);
    if (!context) ID3D11Device_GetImmediateContext(device, &context);
    wrap_context(context);
    if (context && !immediate) ID3D11DeviceContext_Release(context);
}

static BOOL load_backend(void)
{
    if (backend) return TRUE;
    if (!(backend = LoadLibraryA("wfdxbackend-d3d11.dll"))) return FALSE;
    backend_create_device = (PFN_D3D11_CREATE_DEVICE)(void *)GetProcAddress(backend, "D3D11CreateDevice");
    backend_create_device_and_swap_chain =
            (PFN_D3D11_CREATE_DEVICE_AND_SWAP_CHAIN)(void *)GetProcAddress(backend, "D3D11CreateDeviceAndSwapChain");
    return backend_create_device && backend_create_device_and_swap_chain;
}

HRESULT WINAPI D3D11CreateDevice(IDXGIAdapter *adapter, D3D_DRIVER_TYPE driver_type, HMODULE swrast,
        UINT flags, const D3D_FEATURE_LEVEL *feature_levels, UINT levels, UINT sdk_version,
        ID3D11Device **device, D3D_FEATURE_LEVEL *feature_level, ID3D11DeviceContext **context)
{
    HRESULT hr;

    if (!load_backend()) return E_FAIL;
    hr = backend_create_device(adapter, driver_type, swrast, flags, feature_levels, levels, sdk_version,
            device, feature_level, context);
    if (SUCCEEDED(hr) && device) wrap_device(*device, context ? *context : NULL);
    return hr;
}

HRESULT WINAPI D3D11CreateDeviceAndSwapChain(IDXGIAdapter *adapter, D3D_DRIVER_TYPE driver_type,
        HMODULE swrast, UINT flags, const D3D_FEATURE_LEVEL *feature_levels, UINT levels,
        UINT sdk_version, const DXGI_SWAP_CHAIN_DESC *swapchain_desc, IDXGISwapChain **swapchain,
        ID3D11Device **device, D3D_FEATURE_LEVEL *feature_level, ID3D11DeviceContext **context)
{
    HRESULT hr;

    if (!load_backend()) return E_FAIL;
    hr = backend_create_device_and_swap_chain(adapter, driver_type, swrast, flags, feature_levels, levels,
            sdk_version, swapchain_desc, swapchain, device, feature_level, context);
    if (SUCCEEDED(hr) && device) wrap_device(*device, context ? *context : NULL);
    return hr;
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, void *reserved)
{
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) DisableThreadLibraryCalls(instance);
    return TRUE;
}

/*
 * MetalSharp D3D11 frontend for D3DMetal: GenerateMips with the D3D11 runtime's checks, and shared
 * handles for resources shared within the process (see emulated_get_shared_handle).
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
#include <string.h>

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

/* D3DMetal returns E_NOTIMPL from IDXGIResource::GetSharedHandle, so resources created with
 * D3D11_RESOURCE_MISC_SHARED can't be shared at all. Custom Shaders Patch's shared textures, and the
 * No Hesi server script's car paint built on them, then fail; so does CSP's loader for encrypted
 * (KN5ENC) cars, which makes their textures on a device of its own and opens them on AC's, leaving
 * other players' cars in the KN5's blue placeholder textures. Within one process a shared handle only
 * has to name the resource: GetSharedHandle hands out a handle for it, and OpenSharedResource returns
 * the resource itself, on any device, since D3DMetal's devices share one Metal device and a resource
 * works on each (the application synchronizes them, as D3D11 requires). Another process can't open
 * these handles. */
static const GUID shared_tracker_guid =
    {0x6b3e1d42, 0x9a57, 0x4c1e, {0x8f, 0x2d, 0x31, 0x7a, 0x05, 0xc4, 0x9e, 0x61}};

struct shared_entry
{
    HANDLE handle;
    ID3D11Resource *resource;  /* weak: the tracker removes the entry when the resource goes */
};

static SRWLOCK shared_lock = SRWLOCK_INIT;
static struct shared_entry *shared_entries;
static unsigned int shared_count, shared_capacity;
static ULONG_PTR next_shared_handle = 0x40000000;
static struct hook shared_handle_hooks[16], open_shared_hooks[16];
static LONG shared_logged;

struct shared_tracker
{
    IUnknown IUnknown_iface;
    LONG refcount;
    HANDLE handle;
};

static void remove_shared_entry(HANDLE handle)
{
    unsigned int i;

    AcquireSRWLockExclusive(&shared_lock);
    for (i = 0; i < shared_count; ++i)
    {
        if (shared_entries[i].handle != handle) continue;
        shared_entries[i] = shared_entries[--shared_count];
        break;
    }
    ReleaseSRWLockExclusive(&shared_lock);
}

static HRESULT STDMETHODCALLTYPE tracker_qi(IUnknown *iface, REFIID iid, void **out)
{
    if (IsEqualGUID(iid, &IID_IUnknown))
    {
        IUnknown_AddRef(iface);
        *out = iface;
        return S_OK;
    }
    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG STDMETHODCALLTYPE tracker_addref(IUnknown *iface)
{
    struct shared_tracker *tracker = CONTAINING_RECORD(iface, struct shared_tracker, IUnknown_iface);
    return InterlockedIncrement(&tracker->refcount);
}

static ULONG STDMETHODCALLTYPE tracker_release(IUnknown *iface)
{
    struct shared_tracker *tracker = CONTAINING_RECORD(iface, struct shared_tracker, IUnknown_iface);
    ULONG refcount = InterlockedDecrement(&tracker->refcount);

    if (!refcount)
    {
        remove_shared_entry(tracker->handle);
        HeapFree(GetProcessHeap(), 0, tracker);
    }
    return refcount;
}

static IUnknownVtbl tracker_vtbl = {tracker_qi, tracker_addref, tracker_release};

static UINT resource_misc_flags(ID3D11Resource *resource)
{
    D3D11_RESOURCE_DIMENSION dimension;

    ID3D11Resource_GetType(resource, &dimension);
    switch (dimension)
    {
        case D3D11_RESOURCE_DIMENSION_BUFFER:
        {
            D3D11_BUFFER_DESC desc;
            ID3D11Buffer_GetDesc((ID3D11Buffer *)resource, &desc);
            return desc.MiscFlags;
        }
        case D3D11_RESOURCE_DIMENSION_TEXTURE1D:
        {
            D3D11_TEXTURE1D_DESC desc;
            ID3D11Texture1D_GetDesc((ID3D11Texture1D *)resource, &desc);
            return desc.MiscFlags;
        }
        case D3D11_RESOURCE_DIMENSION_TEXTURE2D:
        {
            D3D11_TEXTURE2D_DESC desc;
            ID3D11Texture2D_GetDesc((ID3D11Texture2D *)resource, &desc);
            return desc.MiscFlags;
        }
        case D3D11_RESOURCE_DIMENSION_TEXTURE3D:
        {
            D3D11_TEXTURE3D_DESC desc;
            ID3D11Texture3D_GetDesc((ID3D11Texture3D *)resource, &desc);
            return desc.MiscFlags;
        }
        default:
            return 0;
    }
}

typedef HRESULT (STDMETHODCALLTYPE *get_shared_handle_fn)(IDXGIResource *, HANDLE *);

static ULONG refcount_of(ID3D11Resource *resource)
{
    ID3D11Resource_AddRef(resource);
    return ID3D11Resource_Release(resource);
}

static ULONG dxgi_refcount_of(IDXGIResource *iface)
{
    IDXGIResource_AddRef(iface);
    return IDXGIResource_Release(iface);
}

/* D3DMetal's IDXGIResource is a wrapper, a new one for each query, that holds a reference to the
 * resource. Querying it for ID3D11Resource returns the resource but adds the reference to the
 * wrapper, so releasing what the query returned releases the resource one time too many. Query
 * twice and see which count the query moves; undo that and own exactly one resource reference, as
 * the D3D11 runtime would give. */
static ID3D11Resource *resource_from_dxgi(IDXGIResource *iface)
{
    ID3D11Resource *resource, *again;
    ULONG resource_before, dxgi_before;

    if (FAILED(IDXGIResource_QueryInterface(iface, &IID_ID3D11Resource, (void **)&resource)) || !resource)
        return NULL;
    resource_before = refcount_of(resource);
    dxgi_before = dxgi_refcount_of(iface);
    if (FAILED(IDXGIResource_QueryInterface(iface, &IID_ID3D11Resource, (void **)&again)) || again != resource)
        return NULL;  /* can't tell what the queries hold, so don't share */
    if (refcount_of(resource) != resource_before)
    {
        ID3D11Resource_Release(again);  /* COM's rule: the first query's reference is ours */
        return resource;
    }
    if (dxgi_refcount_of(iface) != dxgi_before)
    {
        /* each query referenced the wrapper: drop both and take a resource reference instead */
        IDXGIResource_Release(iface);
        IDXGIResource_Release(iface);
    }
    ID3D11Resource_AddRef(resource);
    return resource;
}

static HRESULT STDMETHODCALLTYPE emulated_get_shared_handle(IDXGIResource *iface, HANDLE *handle)
{
    get_shared_handle_fn original = find_original(shared_handle_hooks, iface);
    struct shared_tracker *tracker;
    struct shared_entry *entries;
    ID3D11Resource *resource;
    unsigned int i;
    HRESULT hr;

    if (!handle) return E_INVALIDARG;
    if (!(resource = resource_from_dxgi(iface)))
        return original ? original(iface, handle) : E_NOTIMPL;
    if (!(resource_misc_flags(resource) & (D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX)))
    {
        ID3D11Resource_Release(resource);
        return original ? original(iface, handle) : E_INVALIDARG;
    }

    AcquireSRWLockShared(&shared_lock);
    for (i = 0; i < shared_count; ++i)
        if (shared_entries[i].resource == resource) { *handle = shared_entries[i].handle; break; }
    ReleaseSRWLockShared(&shared_lock);
    if (i < shared_count)
    {
        ID3D11Resource_Release(resource);
        return S_OK;
    }

    if (!(tracker = HeapAlloc(GetProcessHeap(), 0, sizeof(*tracker))))
    {
        ID3D11Resource_Release(resource);
        return E_OUTOFMEMORY;
    }
    tracker->IUnknown_iface.lpVtbl = &tracker_vtbl;
    tracker->refcount = 1;

    AcquireSRWLockExclusive(&shared_lock);
    if (shared_count == shared_capacity)
    {
        unsigned int capacity = shared_capacity ? shared_capacity * 2 : 16;
        entries = shared_entries
                ? HeapReAlloc(GetProcessHeap(), 0, shared_entries, capacity * sizeof(*entries))
                : HeapAlloc(GetProcessHeap(), 0, capacity * sizeof(*entries));
        if (!entries)
        {
            ReleaseSRWLockExclusive(&shared_lock);
            HeapFree(GetProcessHeap(), 0, tracker);
            ID3D11Resource_Release(resource);
            return E_OUTOFMEMORY;
        }
        shared_entries = entries;
        shared_capacity = capacity;
    }
    tracker->handle = (HANDLE)next_shared_handle;
    next_shared_handle += 4;
    shared_entries[shared_count].handle = tracker->handle;
    shared_entries[shared_count].resource = resource;
    ++shared_count;
    ReleaseSRWLockExclusive(&shared_lock);

    /* the resource holds the tracker and releases it when it is destroyed, removing the entry; if it
     * doesn't take it, our release below frees the tracker and removes the entry right away, as an
     * entry the resource could outlive must not stay */
    hr = ID3D11Resource_SetPrivateDataInterface(resource, &shared_tracker_guid, &tracker->IUnknown_iface);
    *handle = tracker->handle;
    IUnknown_Release(&tracker->IUnknown_iface);
    ID3D11Resource_Release(resource);
    if (FAILED(hr))
    {
        *handle = NULL;
        return original ? original(iface, handle) : hr;
    }
    if (InterlockedIncrement(&shared_logged) == 1)
        OutputDebugStringA("d3d11 frontend: emulating shared handles within the process\n");
    return S_OK;
}

typedef HRESULT (STDMETHODCALLTYPE *open_shared_fn)(void *, HANDLE, REFIID, void **);

static HRESULT STDMETHODCALLTYPE emulated_open_shared_resource(void *device, HANDLE handle, REFIID iid, void **out)
{
    open_shared_fn original = find_original(open_shared_hooks, device);
    unsigned int i;
    HRESULT hr = E_INVALIDARG;
    BOOL found = FALSE;

    if (!out) return E_INVALIDARG;
    *out = NULL;

    /* the query runs under the lock so the resource can't be destroyed in between */
    AcquireSRWLockShared(&shared_lock);
    for (i = 0; i < shared_count; ++i)
    {
        if (shared_entries[i].handle != handle) continue;
        found = TRUE;
        hr = ID3D11Resource_QueryInterface(shared_entries[i].resource, iid, out);
        break;
    }
    ReleaseSRWLockShared(&shared_lock);
    if (found) return hr;
    return original ? original(device, handle, iid, out) : E_INVALIDARG;
}

static void wrap_shared_resource(void *texture, UINT misc)
{
    IDXGIResource *resource;

    if (!texture || !(misc & (D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX))) return;
    if (FAILED(IUnknown_QueryInterface((IUnknown *)texture, &IID_IDXGIResource, (void **)&resource))) return;
    patch(shared_handle_hooks, resource, SLOT(IDXGIResourceVtbl, GetSharedHandle), emulated_get_shared_handle);
    IDXGIResource_Release(resource);
}

typedef HRESULT (STDMETHODCALLTYPE *create_tex_fn)(void *, const void *, const D3D11_SUBRESOURCE_DATA *, void **);

static HRESULT STDMETHODCALLTYPE guarded_create_texture1d(void *device, const D3D11_TEXTURE1D_DESC *desc,
        const D3D11_SUBRESOURCE_DATA *data, void **texture)
{
    create_tex_fn original = find_original(tex1d_hooks, device);
    HRESULT hr;
    if (desc && invalid_bc_usage(desc->Format, desc->BindFlags, desc->MiscFlags)) return E_INVALIDARG;
    hr = original ? original(device, desc, data, texture) : E_FAIL;
    if (hr == S_OK && desc && texture) wrap_shared_resource(*texture, desc->MiscFlags);
    return hr;
}

static HRESULT STDMETHODCALLTYPE guarded_create_texture2d(void *device, const D3D11_TEXTURE2D_DESC *desc,
        const D3D11_SUBRESOURCE_DATA *data, void **texture)
{
    create_tex_fn original = find_original(tex2d_hooks, device);
    HRESULT hr;
    if (desc && invalid_bc_usage(desc->Format, desc->BindFlags, desc->MiscFlags)) return E_INVALIDARG;
    hr = original ? original(device, desc, data, texture) : E_FAIL;
    if (hr == S_OK && desc && texture) wrap_shared_resource(*texture, desc->MiscFlags);
    return hr;
}

static HRESULT STDMETHODCALLTYPE guarded_create_texture3d(void *device, const D3D11_TEXTURE3D_DESC *desc,
        const D3D11_SUBRESOURCE_DATA *data, void **texture)
{
    create_tex_fn original = find_original(tex3d_hooks, device);
    HRESULT hr;
    if (desc && invalid_bc_usage(desc->Format, desc->BindFlags, desc->MiscFlags)) return E_INVALIDARG;
    hr = original ? original(device, desc, data, texture) : E_FAIL;
    if (hr == S_OK && desc && texture) wrap_shared_resource(*texture, desc->MiscFlags);
    return hr;
}

static HRESULT STDMETHODCALLTYPE guarded_create_texture2d1(void *device, const D3D11_TEXTURE2D_DESC1 *desc,
        const D3D11_SUBRESOURCE_DATA *data, void **texture)
{
    create_tex_fn original = find_original(tex2d1_hooks, device);
    HRESULT hr;
    if (desc && invalid_bc_usage(desc->Format, desc->BindFlags, desc->MiscFlags)) return E_INVALIDARG;
    hr = original ? original(device, desc, data, texture) : E_FAIL;
    if (hr == S_OK && desc && texture) wrap_shared_resource(*texture, desc->MiscFlags);
    return hr;
}

static HRESULT STDMETHODCALLTYPE guarded_create_texture3d1(void *device, const D3D11_TEXTURE3D_DESC1 *desc,
        const D3D11_SUBRESOURCE_DATA *data, void **texture)
{
    create_tex_fn original = find_original(tex3d1_hooks, device);
    HRESULT hr;
    if (desc && invalid_bc_usage(desc->Format, desc->BindFlags, desc->MiscFlags)) return E_INVALIDARG;
    hr = original ? original(device, desc, data, texture) : E_FAIL;
    if (hr == S_OK && desc && texture) wrap_shared_resource(*texture, desc->MiscFlags);
    return hr;
}

static void wrap_device(ID3D11Device *device, ID3D11DeviceContext *immediate)
{
    ID3D11DeviceContext *context = immediate;

    if (!device) return;
    patch(deferred_hooks, device, SLOT(ID3D11DeviceVtbl, CreateDeferredContext), guarded_create_deferred_context);
    patch(tex1d_hooks, device, SLOT(ID3D11DeviceVtbl, CreateTexture1D), guarded_create_texture1d);
    patch(tex2d_hooks, device, SLOT(ID3D11DeviceVtbl, CreateTexture2D), guarded_create_texture2d);
    patch(tex3d_hooks, device, SLOT(ID3D11DeviceVtbl, CreateTexture3D), guarded_create_texture3d);
    patch(open_shared_hooks, device, SLOT(ID3D11DeviceVtbl, OpenSharedResource), emulated_open_shared_resource);
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
            patch(open_shared_hooks, device3, SLOT(ID3D11Device3Vtbl, OpenSharedResource), emulated_open_shared_resource);
            ID3D11Device3_Release(device3);
        }
    }
    patch(devqi_hooks, device, SLOT(ID3D11DeviceVtbl, QueryInterface), guarded_device_qi);
    if (!context) ID3D11Device_GetImmediateContext(device, &context);
    wrap_context(context);
    if (context && !immediate) ID3D11DeviceContext_Release(context);
}

/* The backend is D3DMetal's d3d11 under a private name, which Wine's loader maps when it loads a
 * Wine placeholder with that name. The placeholder ships next to this frontend, so prefixes need
 * no copy of it; one in system32 is used first. */
static HMODULE load_backend_module(void)
{
    char unix_dir[MAX_PATH], *slash;
    WCHAR *dos_dir, path[MAX_PATH];
    WCHAR *(CDECL *get_dos_file_name)(const char *);
    HMODULE module;
    DWORD len;

    if ((module = LoadLibraryA("wfdxbackend-d3d11.dll"))) return module;

    len = GetEnvironmentVariableA("WFDXCOMPAT_RUNTIME_DIR", unix_dir, sizeof(unix_dir));
    if (!len || len >= sizeof(unix_dir))
    {
        /* <runtime>/d3dmetal-*  ->  <runtime>/wfdxcompat, as ntdll derives it */
        len = GetEnvironmentVariableA("D3DMETAL_RUNTIME_DIR", unix_dir, sizeof(unix_dir));
        if (!len || len >= sizeof(unix_dir) - 16) return NULL;
        while (len > 1 && unix_dir[len - 1] == '/') unix_dir[--len] = 0;
        if (!(slash = strrchr(unix_dir, '/')) || slash == unix_dir) return NULL;
        strcpy(slash, "/wfdxcompat");
    }
    get_dos_file_name = (void *)GetProcAddress(GetModuleHandleA("kernel32.dll"), "wine_get_dos_file_name");
    if (!get_dos_file_name || !(dos_dir = get_dos_file_name(unix_dir))) return NULL;
    _snwprintf(path, ARRAYSIZE(path), L"%s\\x86_64-windows\\wfdxbackend-d3d11.dll", dos_dir);
    path[ARRAYSIZE(path) - 1] = 0;
    HeapFree(GetProcessHeap(), 0, dos_dir);
    return LoadLibraryW(path);
}

static BOOL load_backend(void)
{
    if (backend) return TRUE;
    if (!(backend = load_backend_module())) return FALSE;
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

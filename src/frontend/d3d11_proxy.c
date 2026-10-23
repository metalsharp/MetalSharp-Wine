#define COBJMACROS
#include <windows.h>
#include <d3d11_4.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include "wfdxcompat/launchers_abi.h"
#include "wfdxcompat/d3d11_context_state.h"
#include "wfdxcompat/d3d11_gdi_surface.h"
#include "wfdxcompat/d3d10_bridge.h"

#define SLOT(type, member) (offsetof(type, member) / sizeof(void *))
#define VTABLE_COUNT(type) (sizeof(type) / sizeof(void *))

struct overlay
{
    struct overlay *next;
    void *object;
    void **original;
    void **patched;
    size_t count;
};

static SRWLOCK overlay_lock = SRWLOCK_INIT;
static struct overlay *device_overlays;
static UINT launcher_flags;

static void **find_original(void *object)
{
    struct overlay *entry;
    void **result = NULL;

    AcquireSRWLockShared(&overlay_lock);
    for (entry = device_overlays; entry; entry = entry->next)
        if (entry->object == object && entry->patched == *(void ***)object)
        {
            result = entry->original;
            break;
        }
    ReleaseSRWLockShared(&overlay_lock);
    return result;
}

static HRESULT wrap_device(ID3D11Device *device);

static HRESULT STDMETHODCALLTYPE proxy_device_qi(ID3D11Device5 *iface, REFIID iid, void **out)
{
    ID3D11Device5Vtbl *original = (ID3D11Device5Vtbl *)find_original(iface);
    HRESULT hr;

    if (!original) return E_FAIL;
    if (!(launcher_flags & WFDX_LAUNCHER_FLAG_NATIVE_D3D10) &&
        (IsEqualGUID(iid, &IID_ID3D10Device) ||
         IsEqualGUID(iid, &IID_ID3D10Device1)) &&
        SUCCEEDED(hr = wfdx_d3d10_bridge_query((ID3D11Device *)iface, iid, out)))
        return hr;
    hr = original->QueryInterface(iface, iid, out);
    if (SUCCEEDED(hr) && out && *out &&
        (IsEqualGUID(iid, &IID_ID3D11Device) || IsEqualGUID(iid, &IID_ID3D11Device1) ||
         IsEqualGUID(iid, &IID_ID3D11Device2) || IsEqualGUID(iid, &IID_ID3D11Device3) ||
         IsEqualGUID(iid, &IID_ID3D11Device4) || IsEqualGUID(iid, &IID_ID3D11Device5)))
        wrap_device((ID3D11Device *)*out);
    return hr;
}

static HRESULT STDMETHODCALLTYPE proxy_create_texture2d(ID3D11Device5 *iface,
        const D3D11_TEXTURE2D_DESC *desc, const D3D11_SUBRESOURCE_DATA *data,
        ID3D11Texture2D **texture)
{
    ID3D11Device5Vtbl *original = (ID3D11Device5Vtbl *)find_original(iface);
    HRESULT hr;

    if (!original) return E_FAIL;
    hr = original->CreateTexture2D(iface, desc, data, texture);
    if (SUCCEEDED(hr) && texture && *texture)
        wfdx_gdi_wrap_texture(*texture);
    return hr;
}

static void STDMETHODCALLTYPE proxy_get_immediate_context(ID3D11Device5 *iface,
        ID3D11DeviceContext **context)
{
    ID3D11Device5Vtbl *original = (ID3D11Device5Vtbl *)find_original(iface);
    ID3D11DeviceContext1 *context1;

    if (!original || !context) return;
    original->GetImmediateContext(iface, context);
    if (*context && SUCCEEDED(ID3D11DeviceContext_QueryInterface(*context,
            &IID_ID3D11DeviceContext1, (void **)&context1)))
    {
        wfdx_context_state_wrap_context(context1, (ID3D11Device *)iface);
        ID3D11DeviceContext1_Release(context1);
    }
}

static HRESULT STDMETHODCALLTYPE proxy_create_deferred_context(ID3D11Device5 *iface,
        UINT flags, ID3D11DeviceContext **context)
{
    ID3D11Device5Vtbl *original = (ID3D11Device5Vtbl *)find_original(iface);
    ID3D11DeviceContext1 *context1;
    HRESULT hr;

    if (!original) return E_FAIL;
    hr = original->CreateDeferredContext(iface, flags, context);
    if (SUCCEEDED(hr) && context && *context &&
        SUCCEEDED(ID3D11DeviceContext_QueryInterface(*context,
                &IID_ID3D11DeviceContext1, (void **)&context1)))
    {
        wfdx_context_state_wrap_context(context1, (ID3D11Device *)iface);
        ID3D11DeviceContext1_Release(context1);
    }
    return hr;
}

static void STDMETHODCALLTYPE proxy_get_immediate_context1(ID3D11Device5 *iface,
        ID3D11DeviceContext1 **context)
{
    ID3D11Device5Vtbl *original = (ID3D11Device5Vtbl *)find_original(iface);
    if (!original || !context) return;
    original->GetImmediateContext1(iface, context);
    if (*context) wfdx_context_state_wrap_context(*context, (ID3D11Device *)iface);
}

static HRESULT STDMETHODCALLTYPE proxy_create_deferred_context1(ID3D11Device5 *iface,
        UINT flags, ID3D11DeviceContext1 **context)
{
    ID3D11Device5Vtbl *original = (ID3D11Device5Vtbl *)find_original(iface);
    HRESULT hr;
    if (!original) return E_FAIL;
    hr = original->CreateDeferredContext1(iface, flags, context);
    if (SUCCEEDED(hr) && context && *context)
        wfdx_context_state_wrap_context(*context, (ID3D11Device *)iface);
    return hr;
}

static HRESULT STDMETHODCALLTYPE proxy_create_context_state(ID3D11Device5 *iface,
        UINT flags, const D3D_FEATURE_LEVEL *levels, UINT level_count, UINT sdk,
        REFIID emulated, D3D_FEATURE_LEVEL *chosen, ID3DDeviceContextState **state)
{
    return wfdx_context_state_create((ID3D11Device1 *)iface, flags, levels,
            level_count, sdk, emulated, chosen, state);
}

static HRESULT wrap_device(ID3D11Device *device)
{
    struct overlay *entry, *it;
    void **original, **table;
    size_t count = VTABLE_COUNT(ID3D11Device5Vtbl);

    if (!device) return E_INVALIDARG;
    AcquireSRWLockExclusive(&overlay_lock);
    for (it = device_overlays; it; it = it->next)
        if (it->object == device)
        {
            ReleaseSRWLockExclusive(&overlay_lock);
            return S_OK;
        }
    original = *(void ***)device;
    entry = calloc(1, sizeof(*entry));
    table = malloc(count * sizeof(*table));
    if (!entry || !table)
    {
        free(table);
        free(entry);
        ReleaseSRWLockExclusive(&overlay_lock);
        return E_OUTOFMEMORY;
    }
    memcpy(table, original, count * sizeof(*table));
    table[SLOT(ID3D11Device5Vtbl, QueryInterface)] = proxy_device_qi;
    table[SLOT(ID3D11Device5Vtbl, CreateTexture2D)] = proxy_create_texture2d;
    table[SLOT(ID3D11Device5Vtbl, GetImmediateContext)] = proxy_get_immediate_context;
    table[SLOT(ID3D11Device5Vtbl, CreateDeferredContext)] = proxy_create_deferred_context;
    table[SLOT(ID3D11Device5Vtbl, GetImmediateContext1)] = proxy_get_immediate_context1;
    table[SLOT(ID3D11Device5Vtbl, CreateDeferredContext1)] = proxy_create_deferred_context1;
    table[SLOT(ID3D11Device5Vtbl, CreateDeviceContextState)] = proxy_create_context_state;
    entry->object = device;
    entry->original = original;
    entry->patched = table;
    entry->count = count;
    entry->next = device_overlays;
    device_overlays = entry;
    InterlockedExchangePointer((void *volatile *)device, table);
    ReleaseSRWLockExclusive(&overlay_lock);
    return S_OK;
}

typedef HRESULT (WINAPI *pfn_D3D11CreateDevice)(IDXGIAdapter *, D3D_DRIVER_TYPE,
        HMODULE, UINT, const D3D_FEATURE_LEVEL *, UINT, UINT, ID3D11Device **,
        D3D_FEATURE_LEVEL *, ID3D11DeviceContext **);
typedef HRESULT (WINAPI *pfn_D3D11CreateDeviceAndSwapChain)(IDXGIAdapter *,
        D3D_DRIVER_TYPE, HMODULE, UINT, const D3D_FEATURE_LEVEL *, UINT, UINT,
        const DXGI_SWAP_CHAIN_DESC *, IDXGISwapChain **, ID3D11Device **,
        D3D_FEATURE_LEVEL *, ID3D11DeviceContext **);

static pfn_D3D11CreateDevice backend_create_device;
static pfn_D3D11CreateDeviceAndSwapChain backend_create_device_and_swapchain;
static LONG init_ready;
static LONG init_calls;
static LONG wrapper_calls;
static LONG backend_calls;
static LONG backend_success;
static LONG overlay_attempts;
static LONG overlay_success;
static LONG overlay_failure;

BOOL WINAPI wfdx_launcher_init(const struct wfdx_launcher_init *init)
{
    InterlockedIncrement(&init_calls);
    if (!init || init->size < sizeof(*init) ||
        init->abi_version != WFDX_LAUNCHERS_ABI_VERSION ||
        !(init->flags & WFDX_LAUNCHER_FLAG_INTERPOSE) ||
        !init->create_device || !init->create_device_and_swapchain)
        return FALSE;

    if (InterlockedCompareExchange(&init_ready, -1, 0))
        return InterlockedCompareExchange(&init_ready, 0, 0) == 1 &&
               backend_create_device == (pfn_D3D11CreateDevice)init->create_device &&
               backend_create_device_and_swapchain ==
               (pfn_D3D11CreateDeviceAndSwapChain)init->create_device_and_swapchain &&
               launcher_flags == init->flags;

    backend_create_device_and_swapchain =
            (pfn_D3D11CreateDeviceAndSwapChain)init->create_device_and_swapchain;
    backend_create_device = (pfn_D3D11CreateDevice)init->create_device;
    launcher_flags = init->flags;
    InterlockedExchange(&init_ready, 1);
    return TRUE;
}

void WINAPI wfdx_launcher_get_counters(struct wfdx_launcher_counters *counters)
{
    if (!counters) return;
    counters->size = sizeof(*counters);
    counters->abi_version = WFDX_LAUNCHERS_ABI_VERSION;
    counters->flags = launcher_flags;
    counters->reserved = 0;
    counters->init_calls = InterlockedCompareExchange(&init_calls, 0, 0);
    counters->wrapper_calls = wrapper_calls;
    counters->backend_calls = backend_calls;
    counters->backend_success = InterlockedCompareExchange(&backend_success, 0, 0);
    counters->overlay_attempts = InterlockedCompareExchange(&overlay_attempts, 0, 0);
    counters->overlay_success = InterlockedCompareExchange(&overlay_success, 0, 0);
    counters->overlay_failure = InterlockedCompareExchange(&overlay_failure, 0, 0);
}

static HRESULT finish_create(HRESULT hr, ID3D11Device **device,
        ID3D11DeviceContext **context)
{
    ID3D11DeviceContext1 *context1;
    HRESULT wrap_hr;

    if (FAILED(hr) || !device || !*device) return hr;
    InterlockedIncrement(&backend_success);
    if (!(launcher_flags & (WFDX_LAUNCHER_FLAG_CONTEXT_STATE |
                            WFDX_LAUNCHER_FLAG_GDI_SURFACE)))
        return hr;
    InterlockedIncrement(&overlay_attempts);
    if (FAILED(wrap_hr = wrap_device(*device)))
    {
        InterlockedIncrement(&overlay_failure);
        return hr;
    }
    if (context && *context && SUCCEEDED(ID3D11DeviceContext_QueryInterface(*context,
            &IID_ID3D11DeviceContext1, (void **)&context1)))
    {
        wfdx_context_state_wrap_context(context1, *device);
        ID3D11DeviceContext1_Release(context1);
    }
    InterlockedIncrement(&overlay_success);
    return hr;
}

HRESULT WINAPI wfdx_launcher_D3D11CreateDevice(IDXGIAdapter *adapter,
        D3D_DRIVER_TYPE type,
        HMODULE software, UINT flags, const D3D_FEATURE_LEVEL *levels,
        UINT level_count, UINT sdk, ID3D11Device **device,
        D3D_FEATURE_LEVEL *chosen, ID3D11DeviceContext **context)
{
    HRESULT hr;
    InterlockedIncrement(&wrapper_calls);
    if (InterlockedCompareExchange(&init_ready, 0, 0) != 1 ||
        !backend_create_device) return E_UNEXPECTED;
    InterlockedIncrement(&backend_calls);
    hr = backend_create_device(adapter, type, software,
            flags, levels, level_count,
            sdk, device, chosen, context);
    return finish_create(hr, device, context);
}

HRESULT WINAPI wfdx_launcher_D3D11CreateDeviceAndSwapChain(IDXGIAdapter *adapter,
        D3D_DRIVER_TYPE type, HMODULE software, UINT flags,
        const D3D_FEATURE_LEVEL *levels, UINT level_count, UINT sdk,
        const DXGI_SWAP_CHAIN_DESC *desc, IDXGISwapChain **swapchain,
        ID3D11Device **device, D3D_FEATURE_LEVEL *chosen,
        ID3D11DeviceContext **context)
{
    HRESULT hr;
    InterlockedIncrement(&wrapper_calls);
    if (InterlockedCompareExchange(&init_ready, 0, 0) != 1 ||
        !backend_create_device_and_swapchain) return E_UNEXPECTED;
    InterlockedIncrement(&backend_calls);
    hr = backend_create_device_and_swapchain(adapter, type,
            software, flags, levels, level_count,
            sdk, desc, swapchain, device, chosen, context);
    return finish_create(hr, device, context);
}

HRESULT WINAPI wfdx_launcher_DXGID3D10CreateDevice(HMODULE d3d11,
        IDXGIFactory *factory, IDXGIAdapter *adapter, UINT flags,
        const D3D_FEATURE_LEVEL *levels, UINT level_count, void **out)
{
    ID3D11DeviceContext *context = NULL;
    ID3D11Device *device = NULL;
    ID3D10Device1 *bridge = NULL;
    D3D_FEATURE_LEVEL chosen;
    HRESULT hr;

    (void)d3d11;
    (void)factory;
    if (!out) return E_INVALIDARG;
    *out = NULL;
    if (InterlockedCompareExchange(&init_ready, 0, 0) != 1 ||
        !backend_create_device)
        return E_UNEXPECTED;
    hr = backend_create_device(adapter,
            adapter ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_HARDWARE,
            NULL, flags, levels, level_count, D3D11_SDK_VERSION,
            &device, &chosen, &context);
    if (SUCCEEDED(hr))
    {
        finish_create(hr, &device, &context);
        /* A backend with its own D3D10 device (Mullion) answers ID3D10Device1 directly. */
        if (launcher_flags & WFDX_LAUNCHER_FLAG_NATIVE_D3D10)
            hr = ID3D11Device_QueryInterface(device, &IID_ID3D10Device1, (void **)&bridge);
        else
            hr = wfdx_d3d10_bridge_create(device, flags, &bridge);
    }
    if (context) ID3D11DeviceContext_Release(context);
    if (device) ID3D11Device_Release(device);
    if (SUCCEEDED(hr)) *out = bridge;
    return hr;
}

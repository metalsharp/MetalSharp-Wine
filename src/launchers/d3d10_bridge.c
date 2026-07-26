#define COBJMACROS
#include <windows.h>
#include <d3d10_1.h>
#include <d3d11.h>
#include <stdlib.h>
#include "wfdxcompat/d3d10_bridge.h"

struct d3d10_bridge
{
    ID3D10Device1 ID3D10Device1_iface;
    LONG refcount;
    ID3D11Device *device;
    UINT flags;
    struct d3d10_bridge *next;
};

static SRWLOCK bridge_lock = SRWLOCK_INIT;
static struct d3d10_bridge *bridges;

static struct d3d10_bridge *impl_from_ID3D10Device1(ID3D10Device1 *iface)
{
    return CONTAINING_RECORD(iface, struct d3d10_bridge, ID3D10Device1_iface);
}

static HRESULT STDMETHODCALLTYPE bridge_qi(ID3D10Device1 *iface, REFIID iid,
        void **out)
{
    struct d3d10_bridge *bridge = impl_from_ID3D10Device1(iface);

    if (!out) return E_POINTER;
    *out = NULL;
    if (IsEqualGUID(iid, &IID_ID3D10Device) ||
        IsEqualGUID(iid, &IID_ID3D10Device1))
    {
        *out = iface;
        ID3D10Device1_AddRef(iface);
        return S_OK;
    }
    return ID3D11Device_QueryInterface(bridge->device, iid, out);
}

static ULONG STDMETHODCALLTYPE bridge_addref(ID3D10Device1 *iface)
{
    struct d3d10_bridge *bridge = impl_from_ID3D10Device1(iface);
    ID3D11Device_AddRef(bridge->device);
    return InterlockedIncrement(&bridge->refcount);
}

static ULONG STDMETHODCALLTYPE bridge_release(ID3D10Device1 *iface)
{
    struct d3d10_bridge *bridge = impl_from_ID3D10Device1(iface);
    ULONG refcount = InterlockedDecrement(&bridge->refcount);

    ID3D11Device_Release(bridge->device);
    if (!refcount)
    {
        struct d3d10_bridge **cursor;
        AcquireSRWLockExclusive(&bridge_lock);
        for (cursor = &bridges; *cursor; cursor = &(*cursor)->next)
            if (*cursor == bridge)
            {
                *cursor = bridge->next;
                break;
            }
        ReleaseSRWLockExclusive(&bridge_lock);
        free(bridge);
    }
    return refcount;
}

static HRESULT STDMETHODCALLTYPE bridge_removed_reason(ID3D10Device1 *iface)
{
    return ID3D11Device_GetDeviceRemovedReason(
            impl_from_ID3D10Device1(iface)->device);
}

static HRESULT STDMETHODCALLTYPE bridge_set_exception_mode(ID3D10Device1 *iface,
        UINT mode)
{
    (void)iface;
    return mode ? E_INVALIDARG : S_OK;
}

static UINT STDMETHODCALLTYPE bridge_get_exception_mode(ID3D10Device1 *iface)
{
    (void)iface;
    return 0;
}

static void STDMETHODCALLTYPE bridge_clear_state(ID3D10Device1 *iface)
{
    ID3D11DeviceContext *context;
    ID3D11Device_GetImmediateContext(impl_from_ID3D10Device1(iface)->device,
            &context);
    if (context)
    {
        ID3D11DeviceContext_ClearState(context);
        ID3D11DeviceContext_Release(context);
    }
}

static void STDMETHODCALLTYPE bridge_flush(ID3D10Device1 *iface)
{
    ID3D11DeviceContext *context;
    ID3D11Device_GetImmediateContext(impl_from_ID3D10Device1(iface)->device,
            &context);
    if (context)
    {
        ID3D11DeviceContext_Flush(context);
        ID3D11DeviceContext_Release(context);
    }
}

static UINT STDMETHODCALLTYPE bridge_get_creation_flags(ID3D10Device1 *iface)
{
    return impl_from_ID3D10Device1(iface)->flags;
}

static D3D10_FEATURE_LEVEL1 STDMETHODCALLTYPE bridge_get_feature_level(
        ID3D10Device1 *iface)
{
    D3D_FEATURE_LEVEL level =
            ID3D11Device_GetFeatureLevel(impl_from_ID3D10Device1(iface)->device);
    return level >= D3D_FEATURE_LEVEL_10_1 ? D3D10_FEATURE_LEVEL_10_1 :
            D3D10_FEATURE_LEVEL_10_0;
}

static ID3D10Device1Vtbl bridge_vtbl =
{
    .QueryInterface = bridge_qi,
    .AddRef = bridge_addref,
    .Release = bridge_release,
    .GetDeviceRemovedReason = bridge_removed_reason,
    .SetExceptionMode = bridge_set_exception_mode,
    .GetExceptionMode = bridge_get_exception_mode,
    .ClearState = bridge_clear_state,
    .Flush = bridge_flush,
    .GetCreationFlags = bridge_get_creation_flags,
    .GetFeatureLevel = bridge_get_feature_level,
};

HRESULT wfdx_d3d10_bridge_create(ID3D11Device *device, UINT flags,
        ID3D10Device1 **out)
{
    struct d3d10_bridge *bridge;

    if (!device || !out) return E_INVALIDARG;
    *out = NULL;
    bridge = calloc(1, sizeof(*bridge));
    if (!bridge) return E_OUTOFMEMORY;
    bridge->ID3D10Device1_iface.lpVtbl = &bridge_vtbl;
    bridge->refcount = 1;
    bridge->device = device;
    bridge->flags = flags;
    ID3D11Device_AddRef(device);
    AcquireSRWLockExclusive(&bridge_lock);
    bridge->next = bridges;
    bridges = bridge;
    ReleaseSRWLockExclusive(&bridge_lock);
    *out = &bridge->ID3D10Device1_iface;
    return S_OK;
}

HRESULT wfdx_d3d10_bridge_query(ID3D11Device *device, REFIID iid, void **out)
{
    struct d3d10_bridge *bridge;
    HRESULT hr = E_NOINTERFACE;

    if (!out) return E_POINTER;
    *out = NULL;
    if (!IsEqualGUID(iid, &IID_ID3D10Device) &&
        !IsEqualGUID(iid, &IID_ID3D10Device1))
        return E_NOINTERFACE;
    AcquireSRWLockShared(&bridge_lock);
    for (bridge = bridges; bridge; bridge = bridge->next)
        if (bridge->device == device)
        {
            *out = &bridge->ID3D10Device1_iface;
            bridge_addref(*out);
            hr = S_OK;
            break;
        }
    ReleaseSRWLockShared(&bridge_lock);
    if (SUCCEEDED(hr)) return hr;
    return wfdx_d3d10_bridge_create(device,
            ID3D11Device_GetCreationFlags(device), (ID3D10Device1 **)out);
}

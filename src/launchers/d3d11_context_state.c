#define COBJMACROS
#include <windows.h>
#include <d3d11_1.h>
#include <dxgi.h>
#include <initguid.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "wfdxcompat/d3d11_context_state.h"

#define WFDX_VB_SLOTS D3D11_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT
#define WFDX_CB_SLOTS D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT
#define WFDX_SRV_SLOTS D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT
#define WFDX_SAMPLER_SLOTS D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT
#define WFDX_RT_SLOTS D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT
#define WFDX_VIEWPORTS D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE
#define WFDX_STATE_MAGIC UINT64_C(0x5746445843535431)

/* Only this implementation answers this IID. It is used to identify a state
 * without interpreting an arbitrary foreign COM object's storage. */
DEFINE_GUID(IID_IWfdxContextStateIdentity, 0xce138f19, 0x22eb, 0x49f8,
        0xa4, 0x5a, 0xe0, 0x6d, 0x60, 0x49, 0x67, 0xb1);

struct wfdx_pipeline_state
{
    ID3D11InputLayout *input_layout;
    D3D11_PRIMITIVE_TOPOLOGY topology;
    ID3D11Buffer *index_buffer;
    DXGI_FORMAT index_format;
    UINT index_offset;
    ID3D11Buffer *vertex_buffers[WFDX_VB_SLOTS];
    UINT vertex_strides[WFDX_VB_SLOTS];
    UINT vertex_offsets[WFDX_VB_SLOTS];

    ID3D11VertexShader *vs;
    ID3D11Buffer *vs_cbs[WFDX_CB_SLOTS];
    ID3D11PixelShader *ps;
    ID3D11Buffer *ps_cbs[WFDX_CB_SLOTS];
    ID3D11ShaderResourceView *ps_srvs[WFDX_SRV_SLOTS];
    ID3D11SamplerState *ps_samplers[WFDX_SAMPLER_SLOTS];

    UINT viewport_count;
    D3D11_VIEWPORT viewports[WFDX_VIEWPORTS];
    UINT scissor_count;
    D3D11_RECT scissors[WFDX_VIEWPORTS];
    ID3D11RasterizerState *rasterizer;

    ID3D11RenderTargetView *render_targets[WFDX_RT_SLOTS];
    ID3D11DepthStencilView *depth_stencil;
    ID3D11BlendState *blend;
    FLOAT blend_factor[4];
    UINT sample_mask;
};

struct wfdx_context_state
{
    ID3DDeviceContextState ID3DDeviceContextState_iface;
    LONG refcount;
    uint64_t magic;
    ID3D11Device *device;
    IUnknown *device_identity;
    struct wfdx_pipeline_state pipeline;
};

struct wfdx_context_hook
{
    struct wfdx_context_hook *next;
    ID3D11DeviceContext1 *context;
    ID3D11Device *device;
    IUnknown *device_identity;
    ID3D11DeviceContext1Vtbl *vtable;
    const ID3D11DeviceContext1Vtbl *original_vtable;
    struct wfdx_context_state *default_state;
    struct wfdx_context_state *active_state;
};

static SRWLOCK hook_lock = SRWLOCK_INIT;
static struct wfdx_context_hook *context_hooks;

static inline struct wfdx_context_state *impl_from_state(
        ID3DDeviceContextState *iface)
{
    return CONTAINING_RECORD(iface, struct wfdx_context_state,
            ID3DDeviceContextState_iface);
}

static void pipeline_release(struct wfdx_pipeline_state *state)
{
    UINT i;

#define RELEASE_MEMBER(member) do { \
    if (state->member) IUnknown_Release((IUnknown *)state->member); \
    state->member = NULL; \
} while (0)
#define RELEASE_ARRAY(member, count) do { \
    for (i = 0; i < (count); ++i) RELEASE_MEMBER(member[i]); \
} while (0)

    RELEASE_MEMBER(input_layout);
    RELEASE_MEMBER(index_buffer);
    RELEASE_ARRAY(vertex_buffers, WFDX_VB_SLOTS);
    RELEASE_MEMBER(vs);
    RELEASE_ARRAY(vs_cbs, WFDX_CB_SLOTS);
    RELEASE_MEMBER(ps);
    RELEASE_ARRAY(ps_cbs, WFDX_CB_SLOTS);
    RELEASE_ARRAY(ps_srvs, WFDX_SRV_SLOTS);
    RELEASE_ARRAY(ps_samplers, WFDX_SAMPLER_SLOTS);
    RELEASE_MEMBER(rasterizer);
    RELEASE_ARRAY(render_targets, WFDX_RT_SLOTS);
    RELEASE_MEMBER(depth_stencil);
    RELEASE_MEMBER(blend);
#undef RELEASE_ARRAY
#undef RELEASE_MEMBER
}

static void pipeline_init_default(struct wfdx_pipeline_state *state)
{
    memset(state, 0, sizeof(*state));
    state->topology = D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;
    state->blend_factor[0] = 1.0f;
    state->blend_factor[1] = 1.0f;
    state->blend_factor[2] = 1.0f;
    state->blend_factor[3] = 1.0f;
    state->sample_mask = ~0u;
}

static void pipeline_capture(struct wfdx_pipeline_state *state,
        ID3D11DeviceContext1 *context)
{
    pipeline_release(state);
    pipeline_init_default(state);

    ID3D11DeviceContext1_IAGetInputLayout(context, &state->input_layout);
    ID3D11DeviceContext1_IAGetPrimitiveTopology(context, &state->topology);
    ID3D11DeviceContext1_IAGetIndexBuffer(context, &state->index_buffer,
            &state->index_format, &state->index_offset);
    ID3D11DeviceContext1_IAGetVertexBuffers(context, 0, WFDX_VB_SLOTS,
            state->vertex_buffers, state->vertex_strides,
            state->vertex_offsets);

    ID3D11DeviceContext1_VSGetShader(context, &state->vs, NULL, NULL);
    ID3D11DeviceContext1_VSGetConstantBuffers(context, 0, WFDX_CB_SLOTS,
            state->vs_cbs);
    ID3D11DeviceContext1_PSGetShader(context, &state->ps, NULL, NULL);
    ID3D11DeviceContext1_PSGetConstantBuffers(context, 0, WFDX_CB_SLOTS,
            state->ps_cbs);
    ID3D11DeviceContext1_PSGetShaderResources(context, 0, WFDX_SRV_SLOTS,
            state->ps_srvs);
    ID3D11DeviceContext1_PSGetSamplers(context, 0, WFDX_SAMPLER_SLOTS,
            state->ps_samplers);

    state->viewport_count = WFDX_VIEWPORTS;
    ID3D11DeviceContext1_RSGetViewports(context, &state->viewport_count,
            state->viewports);
    state->scissor_count = WFDX_VIEWPORTS;
    ID3D11DeviceContext1_RSGetScissorRects(context, &state->scissor_count,
            state->scissors);
    ID3D11DeviceContext1_RSGetState(context, &state->rasterizer);

    ID3D11DeviceContext1_OMGetRenderTargets(context, WFDX_RT_SLOTS,
            state->render_targets, &state->depth_stencil);
    ID3D11DeviceContext1_OMGetBlendState(context, &state->blend,
            state->blend_factor, &state->sample_mask);
}

static void pipeline_restore(const struct wfdx_pipeline_state *state,
        ID3D11DeviceContext1 *context)
{
    ID3D11DeviceContext1_IASetInputLayout(context, state->input_layout);
    ID3D11DeviceContext1_IASetPrimitiveTopology(context, state->topology);
    ID3D11DeviceContext1_IASetIndexBuffer(context, state->index_buffer,
            state->index_format, state->index_offset);
    ID3D11DeviceContext1_IASetVertexBuffers(context, 0, WFDX_VB_SLOTS,
            state->vertex_buffers, state->vertex_strides,
            state->vertex_offsets);

    ID3D11DeviceContext1_VSSetShader(context, state->vs, NULL, 0);
    ID3D11DeviceContext1_VSSetConstantBuffers(context, 0, WFDX_CB_SLOTS,
            state->vs_cbs);
    ID3D11DeviceContext1_PSSetShader(context, state->ps, NULL, 0);
    ID3D11DeviceContext1_PSSetConstantBuffers(context, 0, WFDX_CB_SLOTS,
            state->ps_cbs);
    ID3D11DeviceContext1_PSSetShaderResources(context, 0, WFDX_SRV_SLOTS,
            state->ps_srvs);
    ID3D11DeviceContext1_PSSetSamplers(context, 0, WFDX_SAMPLER_SLOTS,
            state->ps_samplers);

    ID3D11DeviceContext1_RSSetViewports(context, state->viewport_count,
            state->viewports);
    ID3D11DeviceContext1_RSSetScissorRects(context, state->scissor_count,
            state->scissors);
    ID3D11DeviceContext1_RSSetState(context, state->rasterizer);

    ID3D11DeviceContext1_OMSetRenderTargets(context, WFDX_RT_SLOTS,
            state->render_targets, state->depth_stencil);
    ID3D11DeviceContext1_OMSetBlendState(context, state->blend,
            state->blend_factor, state->sample_mask);
}

static HRESULT STDMETHODCALLTYPE state_QueryInterface(
        ID3DDeviceContextState *iface, REFIID iid, void **out)
{
    struct wfdx_context_state *state = impl_from_state(iface);

    if (!out) return E_POINTER;
    *out = NULL;
    if (!IsEqualIID(iid, &IID_IUnknown)
            && !IsEqualIID(iid, &IID_ID3D11DeviceChild)
            && !IsEqualIID(iid, &IID_ID3DDeviceContextState)
            && !IsEqualIID(iid, &IID_IWfdxContextStateIdentity))
        return E_NOINTERFACE;
    *out = iface;
    InterlockedIncrement(&state->refcount);
    return S_OK;
}

static ULONG STDMETHODCALLTYPE state_AddRef(ID3DDeviceContextState *iface)
{
    return InterlockedIncrement(&impl_from_state(iface)->refcount);
}

static ULONG STDMETHODCALLTYPE state_Release(ID3DDeviceContextState *iface)
{
    struct wfdx_context_state *state = impl_from_state(iface);
    ULONG refcount = InterlockedDecrement(&state->refcount);

    if (!refcount)
    {
        state->magic = 0;
        pipeline_release(&state->pipeline);
        IUnknown_Release(state->device_identity);
        ID3D11Device_Release(state->device);
        HeapFree(GetProcessHeap(), 0, state);
    }
    return refcount;
}

static void STDMETHODCALLTYPE state_GetDevice(ID3DDeviceContextState *iface,
        ID3D11Device **device)
{
    struct wfdx_context_state *state = impl_from_state(iface);

    if (!device) return;
    *device = state->device;
    ID3D11Device_AddRef(*device);
}

static HRESULT STDMETHODCALLTYPE state_GetPrivateData(
        ID3DDeviceContextState *iface, REFGUID guid, UINT *size, void *data)
{
    (void)iface;
    (void)guid;
    (void)data;
    if (!size) return E_POINTER;
    *size = 0;
    return DXGI_ERROR_NOT_FOUND;
}

static HRESULT STDMETHODCALLTYPE state_SetPrivateData(
        ID3DDeviceContextState *iface, REFGUID guid, UINT size,
        const void *data)
{
    (void)iface;
    (void)guid;
    (void)size;
    (void)data;
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE state_SetPrivateDataInterface(
        ID3DDeviceContextState *iface, REFGUID guid, const IUnknown *data)
{
    (void)iface;
    (void)guid;
    (void)data;
    return E_NOTIMPL;
}

static ID3DDeviceContextStateVtbl state_vtable =
{
    state_QueryInterface,
    state_AddRef,
    state_Release,
    state_GetDevice,
    state_GetPrivateData,
    state_SetPrivateData,
    state_SetPrivateDataInterface,
};

static HRESULT state_alloc(ID3D11Device *device,
        struct wfdx_context_state **out)
{
    struct wfdx_context_state *state;
    HRESULT hr;

    *out = NULL;
    state = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*state));
    if (!state) return E_OUTOFMEMORY;
    state->ID3DDeviceContextState_iface.lpVtbl = &state_vtable;
    state->refcount = 1;
    state->magic = WFDX_STATE_MAGIC;
    state->device = device;
    ID3D11Device_AddRef(device);
    hr = ID3D11Device_QueryInterface(device, &IID_IUnknown,
            (void **)&state->device_identity);
    if (FAILED(hr))
    {
        ID3D11Device_Release(device);
        HeapFree(GetProcessHeap(), 0, state);
        return hr;
    }
    pipeline_init_default(&state->pipeline);
    *out = state;
    return S_OK;
}

static struct wfdx_context_hook *find_hook_locked(
        ID3D11DeviceContext1 *context)
{
    struct wfdx_context_hook *hook;

    for (hook = context_hooks; hook; hook = hook->next)
        if (hook->context == context) return hook;
    return NULL;
}

static struct wfdx_context_state *identify_state(
        ID3DDeviceContextState *iface)
{
    ID3DDeviceContextState *identity;
    struct wfdx_context_state *state;

    if (!iface) return NULL;
    if (FAILED(ID3DDeviceContextState_QueryInterface(iface,
            &IID_IWfdxContextStateIdentity, (void **)&identity)))
        return NULL;
    state = impl_from_state(identity);
    if (state->magic != WFDX_STATE_MAGIC) state = NULL;
    ID3DDeviceContextState_Release(identity);
    return state;
}

static void STDMETHODCALLTYPE hook_SwapDeviceContextState(
        ID3D11DeviceContext1 *context, ID3DDeviceContextState *incoming_iface,
        ID3DDeviceContextState **previous)
{
    struct wfdx_context_hook *hook;
    struct wfdx_context_state *incoming;
    struct wfdx_context_state *outgoing;
    void (STDMETHODCALLTYPE *original)(ID3D11DeviceContext1 *,
            ID3DDeviceContextState *, ID3DDeviceContextState **);

    AcquireSRWLockExclusive(&hook_lock);
    hook = find_hook_locked(context);
    if (!hook)
    {
        ReleaseSRWLockExclusive(&hook_lock);
        if (previous) *previous = NULL;
        return;
    }
    original = hook->original_vtable->SwapDeviceContextState;
    if (!incoming_iface)
    {
        if (previous) *previous = NULL;
        ReleaseSRWLockExclusive(&hook_lock);
        return;
    }
    incoming = identify_state(incoming_iface);
    if (!incoming || incoming->device_identity != hook->device_identity)
    {
        ReleaseSRWLockExclusive(&hook_lock);
        original(context, incoming_iface, previous);
        return;
    }

    outgoing = hook->active_state;
    pipeline_capture(&outgoing->pipeline, context);
    pipeline_restore(&incoming->pipeline, context);

    ID3DDeviceContextState_AddRef(&incoming->ID3DDeviceContextState_iface);
    hook->active_state = incoming;
    if (previous)
    {
        *previous = &outgoing->ID3DDeviceContextState_iface;
        ID3DDeviceContextState_AddRef(*previous);
    }
    ID3DDeviceContextState_Release(&outgoing->ID3DDeviceContextState_iface);
    ReleaseSRWLockExclusive(&hook_lock);
}

static ULONG STDMETHODCALLTYPE hook_context_Release(
        ID3D11DeviceContext1 *context)
{
    struct wfdx_context_hook *hook, **cursor;
    ULONG (STDMETHODCALLTYPE *original_release)(ID3D11DeviceContext1 *);
    ULONG refcount;

    AcquireSRWLockShared(&hook_lock);
    hook = find_hook_locked(context);
    if (!hook)
    {
        ReleaseSRWLockShared(&hook_lock);
        return 0;
    }
    original_release = (void *)hook->original_vtable->Release;
    ReleaseSRWLockShared(&hook_lock);
    refcount = original_release(context);
    if (refcount) return refcount;

    AcquireSRWLockExclusive(&hook_lock);
    for (cursor = &context_hooks; *cursor; cursor = &(*cursor)->next)
    {
        if (*cursor != hook) continue;
        *cursor = hook->next;
        break;
    }
    ReleaseSRWLockExclusive(&hook_lock);

    ID3DDeviceContextState_Release(
            &hook->active_state->ID3DDeviceContextState_iface);
    ID3DDeviceContextState_Release(
            &hook->default_state->ID3DDeviceContextState_iface);
    IUnknown_Release(hook->device_identity);
    ID3D11Device_Release(hook->device);
    HeapFree(GetProcessHeap(), 0, hook->vtable);
    HeapFree(GetProcessHeap(), 0, hook);
    return 0;
}

HRESULT wfdx_context_state_create(ID3D11Device1 *device, UINT flags,
        const D3D_FEATURE_LEVEL *feature_levels, UINT feature_level_count,
        UINT sdk_version, REFIID emulated_interface,
        D3D_FEATURE_LEVEL *chosen_feature_level,
        ID3DDeviceContextState **context_state)
{
    struct wfdx_context_state *state;
    ID3D11Device *base_device;
    D3D_FEATURE_LEVEL actual;
    UINT i;
    HRESULT hr;

    if (!context_state) return E_POINTER;
    *context_state = NULL;
    if (!device || !emulated_interface) return E_INVALIDARG;
    if (flags || sdk_version != D3D11_SDK_VERSION) return E_INVALIDARG;
    if (!IsEqualIID(emulated_interface, &IID_ID3D11Device)
            && !IsEqualIID(emulated_interface, &IID_ID3D11Device1)
            && !IsEqualIID(emulated_interface, &IID_ID3D10Device)
            && !IsEqualIID(emulated_interface, &IID_ID3D10Device1))
        return E_NOINTERFACE;
    if (feature_level_count && !feature_levels) return E_INVALIDARG;

    actual = ID3D11Device1_GetFeatureLevel(device);
    if (feature_level_count)
    {
        for (i = 0; i < feature_level_count; ++i)
            if (feature_levels[i] == actual) break;
        if (i == feature_level_count) return E_INVALIDARG;
    }
    if (chosen_feature_level) *chosen_feature_level = actual;

    hr = ID3D11Device1_QueryInterface(device, &IID_ID3D11Device,
            (void **)&base_device);
    if (FAILED(hr)) return hr;
    hr = state_alloc(base_device, &state);
    ID3D11Device_Release(base_device);
    if (FAILED(hr)) return hr;
    *context_state = &state->ID3DDeviceContextState_iface;
    return S_OK;
}

HRESULT wfdx_context_state_wrap_context(ID3D11DeviceContext1 *context,
        ID3D11Device *device)
{
    struct wfdx_context_hook *hook;
    HRESULT hr;

    if (!context || !device) return E_INVALIDARG;
    hook = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*hook));
    if (!hook) return E_OUTOFMEMORY;
    hook->vtable = HeapAlloc(GetProcessHeap(), 0, sizeof(*hook->vtable));
    if (!hook->vtable)
    {
        HeapFree(GetProcessHeap(), 0, hook);
        return E_OUTOFMEMORY;
    }
    hook->context = context;
    hook->device = device;
    ID3D11Device_AddRef(device);
    hr = ID3D11Device_QueryInterface(device, &IID_IUnknown,
            (void **)&hook->device_identity);
    if (FAILED(hr)) goto fail;
    hr = state_alloc(device, &hook->default_state);
    if (FAILED(hr)) goto fail;
    pipeline_capture(&hook->default_state->pipeline, context);
    hook->active_state = hook->default_state;
    ID3DDeviceContextState_AddRef(
            &hook->active_state->ID3DDeviceContextState_iface);

    AcquireSRWLockExclusive(&hook_lock);
    if (find_hook_locked(context))
    {
        ReleaseSRWLockExclusive(&hook_lock);
        hr = S_FALSE;
        goto fail;
    }
    hook->original_vtable = context->lpVtbl;
    memcpy(hook->vtable, hook->original_vtable, sizeof(*hook->vtable));
    hook->vtable->Release = (void *)hook_context_Release;
    hook->vtable->SwapDeviceContextState = hook_SwapDeviceContextState;
    hook->next = context_hooks;
    context_hooks = hook;
    context->lpVtbl = hook->vtable;
    ReleaseSRWLockExclusive(&hook_lock);
    return S_OK;

fail:
    if (hook->active_state)
        ID3DDeviceContextState_Release(
                &hook->active_state->ID3DDeviceContextState_iface);
    if (hook->default_state)
        ID3DDeviceContextState_Release(
                &hook->default_state->ID3DDeviceContextState_iface);
    if (hook->device_identity) IUnknown_Release(hook->device_identity);
    ID3D11Device_Release(hook->device);
    HeapFree(GetProcessHeap(), 0, hook->vtable);
    HeapFree(GetProcessHeap(), 0, hook);
    return hr;
}

#define COBJMACROS
#include <initguid.h>
#include <windows.h>
#include <d3d12.h>
#include <d3dcommon.h>
#include <limits.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "wfdxcompat/notifier_registry.h"
#include "wfdxcompat/nv12_resource.h"
#include "wfdxcompat/command_list_proxy.h"
#include "wfdxcompat/resource_notifier.h"
#include "wfdxcompat/agility_shim.h"

#ifndef WFDX_BACKEND_MODULE
#define WFDX_BACKEND_MODULE L"wfdxbackend-d3d12.dll"
#endif

struct device_vtable_overlay
{
    struct device_vtable_overlay *next;
    void **original;
    void **patched;
    size_t count;
};

static SRWLOCK overlay_lock = SRWLOCK_INIT;
static struct device_vtable_overlay *device_overlays;

static BOOL video_trace_enabled(void)
{
#ifdef WFDXCOMPAT_FORCE_VIDEO_TRACE
    static LONG redirected;

    if (InterlockedCompareExchange(&redirected, 1, 0) == 0)
        freopen("C:\\wfdxcompat-video.log", "a", stderr);
    return TRUE;
#else
    static LONG enabled = -1;
    static LONG redirected;
    LONG value = InterlockedCompareExchange(&enabled, -1, -1);

    if (value == -1)
    {
        char buffer[2];
        value = GetEnvironmentVariableA("WFDXCOMPAT_TRACE_VIDEO", buffer, sizeof(buffer)) != 0;
        InterlockedCompareExchange(&enabled, value, -1);
    }
    if (value && InterlockedCompareExchange(&redirected, 1, 0) == 0)
    {
        char path[MAX_PATH];

        if (GetEnvironmentVariableA("WFDXCOMPAT_TRACE_FILE", path, sizeof(path)))
            freopen(path, "a", stderr);
    }
    return InterlockedCompareExchange(&enabled, 0, 0) != 0;
#endif
}

static BOOL is_video_format(DXGI_FORMAT format)
{
    return format == DXGI_FORMAT_NV12 || format == DXGI_FORMAT_P010 ||
            format == DXGI_FORMAT_P016 || format == DXGI_FORMAT_420_OPAQUE ||
            format == DXGI_FORMAT_YUY2 || format == DXGI_FORMAT_AYUV ||
            format == DXGI_FORMAT_Y410 || format == DXGI_FORMAT_Y416;
}

static void trace_resource_desc(const char *method, const D3D12_RESOURCE_DESC *desc)
{
    if (!video_trace_enabled() || !desc ||
        (!is_video_format(desc->Format) &&
         !(desc->Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D &&
           desc->Format == DXGI_FORMAT_UNKNOWN))) return;
    fprintf(stderr,
            "wfdxcompat-video: %s dimension=%u width=%llu height=%u depth_array=%u "
            "mips=%u format=%u samples=%u quality=%u layout=%u flags=%#x\n",
            method, desc->Dimension, (unsigned long long)desc->Width, desc->Height,
            desc->DepthOrArraySize, desc->MipLevels, desc->Format, desc->SampleDesc.Count,
            desc->SampleDesc.Quality, desc->Layout, desc->Flags);
    fflush(stderr);
}

static void trace_created_resource(const char *method, D3D12_HEAP_TYPE heap_type,
        const D3D12_RESOURCE_DESC *desc, UINT initial_state, HRESULT hr, void *resource)
{
    if (!video_trace_enabled() || !desc) return;
    fprintf(stderr,
            "wfdxcompat-video: tick=%llu %s heap=%u dimension=%u width=%llu height=%u "
            "depth_array=%u mips=%u format=%u layout=%u flags=%#x state_layout=%#x "
            "hr=%#lx resource=%p\n",
            (unsigned long long)GetTickCount64(), method, heap_type, desc->Dimension,
            (unsigned long long)desc->Width, desc->Height, desc->DepthOrArraySize,
            desc->MipLevels, desc->Format, desc->Layout, desc->Flags, initial_state,
            (unsigned long)hr, resource);
    fflush(stderr);
}

static void trace_created_resource1(const char *method, D3D12_HEAP_TYPE heap_type,
        const D3D12_RESOURCE_DESC1 *desc, UINT initial_state, HRESULT hr, void *resource)
{
    D3D12_RESOURCE_DESC legacy;

    if (!desc) return;
    legacy.Dimension = desc->Dimension;
    legacy.Alignment = desc->Alignment;
    legacy.Width = desc->Width;
    legacy.Height = desc->Height;
    legacy.DepthOrArraySize = desc->DepthOrArraySize;
    legacy.MipLevels = desc->MipLevels;
    legacy.Format = desc->Format;
    legacy.SampleDesc = desc->SampleDesc;
    legacy.Layout = desc->Layout;
    legacy.Flags = desc->Flags;
    trace_created_resource(method, heap_type, &legacy, initial_state, hr, resource);
}

#define VTABLE_COUNT(type) (sizeof(type) / sizeof(void *))
#define SLOT(type, member) (offsetof(type, member) / sizeof(void *))

/* Agility SDK device interfaces newer than the llvm-mingw SDK used to build
 * WineForge. Each interface appends the documented number of vtable slots. */
static const GUID iid_device11 =
    {0x5405c344,0xd457,0x444e,{0xb4,0xdd,0x23,0x66,0xe4,0x5a,0xee,0x39}};
static const GUID iid_device12 =
    {0x5af5c532,0x4c91,0x4cd0,{0xb5,0x41,0x15,0xa4,0x05,0x39,0x5f,0xc5}};
static const GUID iid_device13 =
    {0x14eecffc,0x4df8,0x40f7,{0xa1,0x18,0x5c,0x81,0x6f,0x45,0x69,0x5e}};
static const GUID iid_device14 =
    {0x5f6e592d,0xd895,0x44c2,{0x8e,0x4a,0x88,0xad,0x49,0x26,0xd3,0x23}};
static const GUID iid_device15 =
    {0x76cff76f,0x1e9b,0x4450,{0x8c,0xdc,0x34,0xf1,0xaf,0x78,0x8e,0x5b}};

static BOOL iid_equal(REFIID a, REFIID b) { return IsEqualGUID(a, b); }

static size_t device_vtable_count(REFIID iid)
{
    if (iid_equal(iid, &iid_device15)) return VTABLE_COUNT(ID3D12Device10Vtbl) + 15;
    if (iid_equal(iid, &iid_device14)) return VTABLE_COUNT(ID3D12Device10Vtbl) + 4;
    if (iid_equal(iid, &iid_device13)) return VTABLE_COUNT(ID3D12Device10Vtbl) + 3;
    if (iid_equal(iid, &iid_device12)) return VTABLE_COUNT(ID3D12Device10Vtbl) + 2;
    if (iid_equal(iid, &iid_device11)) return VTABLE_COUNT(ID3D12Device10Vtbl) + 1;
    if (iid_equal(iid, &IID_ID3D12Device10)) return VTABLE_COUNT(ID3D12Device10Vtbl);
    if (iid_equal(iid, &IID_ID3D12Device9)) return VTABLE_COUNT(ID3D12Device9Vtbl);
    if (iid_equal(iid, &IID_ID3D12Device8)) return VTABLE_COUNT(ID3D12Device8Vtbl);
    if (iid_equal(iid, &IID_ID3D12Device7)) return VTABLE_COUNT(ID3D12Device7Vtbl);
    if (iid_equal(iid, &IID_ID3D12Device6)) return VTABLE_COUNT(ID3D12Device6Vtbl);
    if (iid_equal(iid, &IID_ID3D12Device5)) return VTABLE_COUNT(ID3D12Device5Vtbl);
    if (iid_equal(iid, &IID_ID3D12Device4)) return VTABLE_COUNT(ID3D12Device4Vtbl);
    if (iid_equal(iid, &IID_ID3D12Device3)) return VTABLE_COUNT(ID3D12Device3Vtbl);
    if (iid_equal(iid, &IID_ID3D12Device2)) return VTABLE_COUNT(ID3D12Device2Vtbl);
    if (iid_equal(iid, &IID_ID3D12Device1)) return VTABLE_COUNT(ID3D12Device1Vtbl);
    if (iid_equal(iid, &IID_ID3D12Device)) return VTABLE_COUNT(ID3D12DeviceVtbl);
    if (iid_equal(iid, &IID_ID3D12Object)) return VTABLE_COUNT(ID3D12ObjectVtbl);
    if (iid_equal(iid, &IID_IUnknown)) return VTABLE_COUNT(IUnknownVtbl);
    return 0;
}

typedef HRESULT (STDMETHODCALLTYPE *query_interface_fn)(void *, REFIID, void **);
typedef void (STDMETHODCALLTYPE *get_copyable_footprints_fn)(ID3D12Device10 *,
        const D3D12_RESOURCE_DESC *, UINT, UINT, UINT64,
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT *, UINT *, UINT64 *, UINT64 *);
typedef D3D12_RESOURCE_ALLOCATION_INFO *(STDMETHODCALLTYPE *get_resource_allocation_info_fn)(
        ID3D12Device10 *, D3D12_RESOURCE_ALLOCATION_INFO *, UINT, UINT,
        const D3D12_RESOURCE_DESC *);
static HRESULT wrap_device(void *object, REFIID iid);

static void patch_device_vtable(void **table, size_t count);

#define ORIGINAL_DEVICE(This, member) \
    ((ID3D12Device10Vtbl *)find_original_vtable(This))->member

static void **find_original_vtable(void *object)
{
    struct device_vtable_overlay *entry;
    void **current = *(void ***)object;
    void **table = NULL;
    AcquireSRWLockShared(&overlay_lock);
    for (entry = device_overlays; entry; entry = entry->next)
        if (entry->patched == current)
        {
            table = entry->original;
            break;
        }
    ReleaseSRWLockShared(&overlay_lock);
    return table;
}

static HRESULT STDMETHODCALLTYPE proxy_device_query_interface(ID3D12Device10 *This,
        REFIID iid, void **out)
{
    void **original = find_original_vtable(This);
    query_interface_fn query_interface;
    HRESULT hr, wrap_hr;

    if (!original) return E_FAIL;
    query_interface = (query_interface_fn)original[0];
    hr = query_interface(This, iid, out);
    if (video_trace_enabled() && SUCCEEDED(hr))
    {
        fprintf(stderr, "wfdxcompat-video: Device::QueryInterface iid="
                "%08lx-%04x-%04x count=%llu object=%p\n",
                (unsigned long)iid->Data1, iid->Data2, iid->Data3,
                (unsigned long long)device_vtable_count(iid), out ? *out : NULL);
        fflush(stderr);
    }
    if (FAILED(hr) || !out || !*out || !device_vtable_count(iid)) return hr;
    if (FAILED(wrap_hr = wrap_device(*out, iid)))
    {
        IUnknown_Release((IUnknown *)*out);
        *out = NULL;
        return wrap_hr;
    }
    return hr;
}

static HRESULT STDMETHODCALLTYPE proxy_CreateCommittedResource(ID3D12Device10 *This,
        const D3D12_HEAP_PROPERTIES *properties, D3D12_HEAP_FLAGS flags,
        const D3D12_RESOURCE_DESC *desc, D3D12_RESOURCE_STATES state,
        const D3D12_CLEAR_VALUE *clear, REFIID iid, void **resource)
{
    wfdx_create_committed_resource_fn create_resource = ORIGINAL_DEVICE(This, CreateCommittedResource);

    trace_resource_desc("CreateCommittedResource", desc);
    HRESULT hr;

    if (desc && desc->Format == DXGI_FORMAT_NV12)
        hr = wfdx_nv12_create_committed_resource(This, create_resource, properties,
                flags, desc, state, iid, resource);
    else
        hr = create_resource(This, properties, flags, desc, state, clear, iid, resource);
    trace_created_resource("CreateCommittedResource", properties ? properties->Type : 0,
            desc, state, hr, resource ? *resource : NULL);
    return hr;
}

static HRESULT STDMETHODCALLTYPE proxy_CreatePlacedResource(ID3D12Device10 *This,
        ID3D12Heap *heap, UINT64 heap_offset, const D3D12_RESOURCE_DESC *desc,
        D3D12_RESOURCE_STATES state, const D3D12_CLEAR_VALUE *clear,
        REFIID iid, void **resource)
{
    D3D12_HEAP_DESC heap_desc;
    wfdx_create_committed_resource_fn create_committed;
    HRESULT hr, wrap_hr;

    if (desc && desc->Format == DXGI_FORMAT_NV12)
    {
        if (!heap) return E_INVALIDARG;
        heap->lpVtbl->GetDesc(heap, &heap_desc);
        create_committed = ORIGINAL_DEVICE(This, CreateCommittedResource);
        return wfdx_nv12_create_placed_resource(This, create_committed, &heap_desc,
                desc, state, iid, resource);
    }

    hr = ORIGINAL_DEVICE(This, CreatePlacedResource)(This, heap, heap_offset, desc,
            state, clear, iid, resource);
    if (FAILED(hr) || !resource || !*resource || !heap) return hr;

    heap->lpVtbl->GetDesc(heap, &heap_desc);
    wrap_hr = wfdx_wrap_electra_placed_resource(*resource, iid, &heap_desc,
            heap_offset, desc, state);
    if (video_trace_enabled() && desc &&
        desc->Dimension == D3D12_RESOURCE_DIMENSION_BUFFER)
    {
        fprintf(stderr,
                "wfdxcompat-video: placed-notifier-candidate width=%llu alignment=%llu "
                "heap_size=%llu heap_offset=%llu heap_flags=%#x heap_type=%u "
                "state=%#x iid=%08lx wrap_hr=%#lx resource=%p\n",
                (unsigned long long)desc->Width,
                (unsigned long long)desc->Alignment,
                (unsigned long long)heap_desc.SizeInBytes,
                (unsigned long long)heap_offset, heap_desc.Flags,
                heap_desc.Properties.Type, state, (unsigned long)iid->Data1,
                (unsigned long)wrap_hr, *resource);
        fflush(stderr);
    }
    if (FAILED(wrap_hr))
    {
        IUnknown_Release((IUnknown *)*resource);
        *resource = NULL;
        return wrap_hr;
    }
    return hr;
}

static HRESULT STDMETHODCALLTYPE trace_CreateCommittedResource1(ID3D12Device10 *This,
        const D3D12_HEAP_PROPERTIES *properties, D3D12_HEAP_FLAGS flags,
        const D3D12_RESOURCE_DESC *desc, D3D12_RESOURCE_STATES state,
        const D3D12_CLEAR_VALUE *clear, ID3D12ProtectedResourceSession *session,
        REFIID iid, void **resource)
{
    HRESULT hr = ORIGINAL_DEVICE(This, CreateCommittedResource1)(This, properties, flags,
            desc, state, clear, session, iid, resource);
    trace_created_resource("CreateCommittedResource1", properties ? properties->Type : 0,
            desc, state, hr, resource ? *resource : NULL);
    return hr;
}

static HRESULT STDMETHODCALLTYPE trace_CreateCommittedResource2(ID3D12Device10 *This,
        const D3D12_HEAP_PROPERTIES *properties, D3D12_HEAP_FLAGS flags,
        const D3D12_RESOURCE_DESC1 *desc, D3D12_RESOURCE_STATES state,
        const D3D12_CLEAR_VALUE *clear, ID3D12ProtectedResourceSession *session,
        REFIID iid, void **resource)
{
    HRESULT hr = ORIGINAL_DEVICE(This, CreateCommittedResource2)(This, properties, flags,
            desc, state, clear, session, iid, resource);
    trace_created_resource1("CreateCommittedResource2", properties ? properties->Type : 0,
            desc, state, hr, resource ? *resource : NULL);
    return hr;
}

static HRESULT STDMETHODCALLTYPE trace_CreateCommittedResource3(ID3D12Device10 *This,
        const D3D12_HEAP_PROPERTIES *properties, D3D12_HEAP_FLAGS flags,
        const D3D12_RESOURCE_DESC1 *desc, D3D12_BARRIER_LAYOUT layout,
        const D3D12_CLEAR_VALUE *clear, ID3D12ProtectedResourceSession *session,
        UINT32 format_count, DXGI_FORMAT *formats, REFIID iid, void **resource)
{
    HRESULT hr = ORIGINAL_DEVICE(This, CreateCommittedResource3)(This, properties, flags,
            desc, layout, clear, session, format_count, formats, iid, resource);
    trace_created_resource1("CreateCommittedResource3", properties ? properties->Type : 0,
            desc, layout, hr, resource ? *resource : NULL);
    return hr;
}

static HRESULT STDMETHODCALLTYPE proxy_CheckFeatureSupport(ID3D12Device10 *This,
        D3D12_FEATURE feature, void *data, UINT size)
{
    HRESULT hr = ORIGINAL_DEVICE(This, CheckFeatureSupport)(This, feature, data, size);

    if (feature == D3D12_FEATURE_FORMAT_SUPPORT &&
        size >= sizeof(D3D12_FEATURE_DATA_FORMAT_SUPPORT) && data)
    {
        D3D12_FEATURE_DATA_FORMAT_SUPPORT *support = data;

        if (support->Format == DXGI_FORMAT_NV12)
        {
            support->Support1 |= D3D12_FORMAT_SUPPORT1_TEXTURE2D |
                    D3D12_FORMAT_SUPPORT1_SHADER_LOAD |
                    D3D12_FORMAT_SUPPORT1_SHADER_SAMPLE;
            return S_OK;
        }
    }
    else if (feature == D3D12_FEATURE_FORMAT_INFO &&
             size >= sizeof(D3D12_FEATURE_DATA_FORMAT_INFO) && data)
    {
        D3D12_FEATURE_DATA_FORMAT_INFO *info = data;

        if (info->Format == DXGI_FORMAT_NV12)
        {
            info->PlaneCount = 2;
            return S_OK;
        }
    }
    return hr;
}

static UINT64 align_resource_size(UINT64 value, UINT64 alignment)
{
    if (!alignment || (alignment & (alignment - 1)) || value > ULLONG_MAX - alignment + 1)
        return ULLONG_MAX;
    return (value + alignment - 1) & ~(alignment - 1);
}

static D3D12_RESOURCE_ALLOCATION_INFO *STDMETHODCALLTYPE
proxy_GetResourceAllocationInfo(ID3D12Device10 *This,
        D3D12_RESOURCE_ALLOCATION_INFO *result, UINT visible_mask, UINT desc_count,
        const D3D12_RESOURCE_DESC *descs)
{
    get_resource_allocation_info_fn get_info = ORIGINAL_DEVICE(This, GetResourceAllocationInfo);
    D3D12_RESOURCE_ALLOCATION_INFO y_info, uv_info;
    D3D12_RESOURCE_DESC plane_desc;
    UINT64 y_size, total_size;

    if (!result || desc_count != 1 || !descs || descs[0].Format != DXGI_FORMAT_NV12)
        return get_info(This, result, visible_mask, desc_count, descs);

    plane_desc = descs[0];
    plane_desc.Format = DXGI_FORMAT_R8_UNORM;
    get_info(This, &y_info, visible_mask, 1, &plane_desc);

    plane_desc.Width = (descs[0].Width + 1) / 2;
    plane_desc.Height = (descs[0].Height + 1) / 2;
    plane_desc.Format = DXGI_FORMAT_R8G8_UNORM;
    get_info(This, &uv_info, visible_mask, 1, &plane_desc);

    if (!y_info.Alignment || !uv_info.Alignment ||
        y_info.SizeInBytes == ULLONG_MAX || uv_info.SizeInBytes == ULLONG_MAX)
    {
        result->SizeInBytes = ULLONG_MAX;
        result->Alignment = max(y_info.Alignment, uv_info.Alignment);
        return result;
    }

    result->Alignment = max(y_info.Alignment, uv_info.Alignment);
    y_size = align_resource_size(y_info.SizeInBytes, uv_info.Alignment);
    if (y_size == ULLONG_MAX || uv_info.SizeInBytes > ULLONG_MAX - y_size)
        total_size = ULLONG_MAX;
    else
        total_size = align_resource_size(y_size + uv_info.SizeInBytes, result->Alignment);
    result->SizeInBytes = total_size;
    return result;
}

static UINT64 align_up(UINT64 value, UINT64 alignment)
{
    return (value + alignment - 1) & ~(alignment - 1);
}

static void invalidate_footprints(UINT count, D3D12_PLACED_SUBRESOURCE_FOOTPRINT *layouts,
        UINT *row_counts, UINT64 *row_sizes, UINT64 *total_bytes)
{
    if (layouts) memset(layouts, 0xff, count * sizeof(*layouts));
    if (row_counts) memset(row_counts, 0xff, count * sizeof(*row_counts));
    if (row_sizes) memset(row_sizes, 0xff, count * sizeof(*row_sizes));
    if (total_bytes) *total_bytes = ~(UINT64)0;
}

static void STDMETHODCALLTYPE proxy_GetCopyableFootprints(ID3D12Device10 *This,
        const D3D12_RESOURCE_DESC *desc, UINT first_subresource, UINT subresource_count,
        UINT64 base_offset, D3D12_PLACED_SUBRESOURCE_FOOTPRINT *layouts,
        UINT *row_counts, UINT64 *row_sizes, UINT64 *total_bytes)
{
    get_copyable_footprints_fn get_footprints = ORIGINAL_DEVICE(This, GetCopyableFootprints);
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT layout;
    D3D12_RESOURCE_DESC plane_desc;
    UINT mip_levels, array_size, plane_stride, total_subresources, i;
    UINT64 offset = 0, end = 0;

    if (!desc || desc->Format != DXGI_FORMAT_NV12)
    {
        get_footprints(This, desc, first_subresource, subresource_count, base_offset,
                layouts, row_counts, row_sizes, total_bytes);
        return;
    }

    mip_levels = desc->MipLevels;
    array_size = desc->DepthOrArraySize;
    if (desc->Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || !mip_levels ||
        !array_size || desc->SampleDesc.Count != 1 ||
        mip_levels > UINT_MAX / array_size)
    {
        invalidate_footprints(subresource_count, layouts, row_counts, row_sizes, total_bytes);
        return;
    }
    plane_stride = mip_levels * array_size;
    if (plane_stride > UINT_MAX / 2)
    {
        invalidate_footprints(subresource_count, layouts, row_counts, row_sizes, total_bytes);
        return;
    }
    total_subresources = plane_stride * 2;
    if (first_subresource > total_subresources ||
        subresource_count > total_subresources - first_subresource)
    {
        invalidate_footprints(subresource_count, layouts, row_counts, row_sizes, total_bytes);
        return;
    }

    for (i = 0; i < subresource_count; ++i)
    {
        UINT subresource = first_subresource + i;
        UINT plane = subresource / plane_stride;
        UINT local_subresource = subresource % plane_stride;
        UINT rows = 0;
        UINT64 row_size = 0, bytes = 0;

        plane_desc = *desc;
        if (!plane) plane_desc.Format = DXGI_FORMAT_R8_UNORM;
        else
        {
            plane_desc.Width = (desc->Width + 1) / 2;
            plane_desc.Height = (desc->Height + 1) / 2;
            plane_desc.Format = DXGI_FORMAT_R8G8_UNORM;
        }
        memset(&layout, 0, sizeof(layout));
        get_footprints(This, &plane_desc, local_subresource, 1, base_offset + offset,
                &layout, &rows, &row_size, &bytes);
        if (layout.Offset == ~(UINT64)0 || layout.Footprint.Format == DXGI_FORMAT_UNKNOWN ||
            layout.Footprint.RowPitch == UINT_MAX || rows == UINT_MAX ||
            row_size == ~(UINT64)0 || bytes == ~(UINT64)0)
        {
            invalidate_footprints(subresource_count, layouts, row_counts, row_sizes, total_bytes);
            return;
        }
        if (layouts) layouts[i] = layout;
        if (row_counts) row_counts[i] = rows;
        if (row_sizes) row_sizes[i] = row_size;
        end = offset + bytes;
        offset = align_up(end, D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT);
    }
    if (total_bytes) *total_bytes = end;
}

static HRESULT STDMETHODCALLTYPE proxy_CreateCommandList(ID3D12Device10 *This,
        UINT node_mask, D3D12_COMMAND_LIST_TYPE type,
        ID3D12CommandAllocator *command_allocator, ID3D12PipelineState *initial_state,
        REFIID iid, void **command_list)
{
    HRESULT hr = ORIGINAL_DEVICE(This, CreateCommandList)(This, node_mask, type,
            command_allocator, initial_state, iid, command_list);

    if (SUCCEEDED(hr) && command_list && *command_list)
    {
        HRESULT wrap_hr = wfdx_wrap_command_list(*command_list, iid);
        if (FAILED(wrap_hr))
        {
            IUnknown_Release((IUnknown *)*command_list);
            *command_list = NULL;
            return wrap_hr;
        }
    }
    return hr;
}

static HRESULT STDMETHODCALLTYPE proxy_CreateCommandList1(ID3D12Device10 *This,
        UINT node_mask, D3D12_COMMAND_LIST_TYPE type, D3D12_COMMAND_LIST_FLAGS flags,
        REFIID iid, void **command_list)
{
    HRESULT hr = ORIGINAL_DEVICE(This, CreateCommandList1)(This, node_mask, type,
            flags, iid, command_list);

    if (SUCCEEDED(hr) && command_list && *command_list)
    {
        HRESULT wrap_hr = wfdx_wrap_command_list(*command_list, iid);
        if (FAILED(wrap_hr))
        {
            IUnknown_Release((IUnknown *)*command_list);
            *command_list = NULL;
            return wrap_hr;
        }
    }
    return hr;
}

static void STDMETHODCALLTYPE proxy_CreateShaderResourceView(ID3D12Device10 *This,
        ID3D12Resource *resource, const D3D12_SHADER_RESOURCE_VIEW_DESC *desc,
        D3D12_CPU_DESCRIPTOR_HANDLE handle)
{
    D3D12_SHADER_RESOURCE_VIEW_DESC translated;
    ID3D12Resource *plane;

    if (video_trace_enabled() && desc &&
        (is_video_format(desc->Format) ||
         (desc->ViewDimension == D3D12_SRV_DIMENSION_TEXTURE2D && desc->Texture2D.PlaneSlice)))
    {
        fprintf(stderr,
                "wfdxcompat-video: CreateShaderResourceView resource=%p format=%u dimension=%u "
                "mips=%u plane=%u handle=%llu\n",
                resource, desc->Format, desc->ViewDimension,
                desc->ViewDimension == D3D12_SRV_DIMENSION_TEXTURE2D ? desc->Texture2D.MipLevels : 0,
                desc->ViewDimension == D3D12_SRV_DIMENSION_TEXTURE2D ? desc->Texture2D.PlaneSlice : 0,
                (unsigned long long)handle.ptr);
        fflush(stderr);
    }
    if (wfdx_nv12_is_resource(resource) && desc &&
        desc->ViewDimension == D3D12_SRV_DIMENSION_TEXTURE2D)
    {
        translated = *desc;
        plane = wfdx_nv12_get_plane(resource, desc->Texture2D.PlaneSlice);
        if (!plane) return;
        translated.Texture2D.PlaneSlice = 0;
        ORIGINAL_DEVICE(This, CreateShaderResourceView)(This, plane, &translated, handle);
        return;
    }
    ORIGINAL_DEVICE(This, CreateShaderResourceView)(This, resource, desc, handle);
}

static void patch_slot(void **table, size_t count, size_t slot, void *function)
{
    if (slot < count) table[slot] = function;
}

static void patch_device_vtable(void **table, size_t count)
{
    patch_slot(table, count, SLOT(ID3D12Device10Vtbl, QueryInterface), proxy_device_query_interface);
    patch_slot(table, count, SLOT(ID3D12Device10Vtbl, CheckFeatureSupport), proxy_CheckFeatureSupport);
    patch_slot(table, count, SLOT(ID3D12Device10Vtbl, CreateCommandList), proxy_CreateCommandList);
    patch_slot(table, count, SLOT(ID3D12Device10Vtbl, CreateCommandList1), proxy_CreateCommandList1);
    patch_slot(table, count, SLOT(ID3D12Device10Vtbl, CreateShaderResourceView), proxy_CreateShaderResourceView);
    patch_slot(table, count, SLOT(ID3D12Device10Vtbl, CreateCommittedResource), proxy_CreateCommittedResource);
    patch_slot(table, count, SLOT(ID3D12Device10Vtbl, CreatePlacedResource), proxy_CreatePlacedResource);
    patch_slot(table, count, SLOT(ID3D12Device10Vtbl, GetResourceAllocationInfo),
            proxy_GetResourceAllocationInfo);
    patch_slot(table, count, SLOT(ID3D12Device10Vtbl, GetCopyableFootprints), proxy_GetCopyableFootprints);
    if (video_trace_enabled())
    {
        patch_slot(table, count, SLOT(ID3D12Device10Vtbl, CreateCommittedResource1),
                trace_CreateCommittedResource1);
        patch_slot(table, count, SLOT(ID3D12Device10Vtbl, CreateCommittedResource2),
                trace_CreateCommittedResource2);
        patch_slot(table, count, SLOT(ID3D12Device10Vtbl, CreateCommittedResource3),
                trace_CreateCommittedResource3);
    }
}

static HRESULT wrap_device(void *object, REFIID iid)
{
    struct device_vtable_overlay *entry, *existing;
    void **current, **source, **table;
    size_t count = device_vtable_count(iid);

    if (!object || !count) return E_INVALIDARG;
    /* D3DMetal exposes newer device interfaces through the same COM object. */
    if (count < VTABLE_COUNT(ID3D12Device10Vtbl))
        count = VTABLE_COUNT(ID3D12Device10Vtbl);
    current = *(void ***)object;
    if (!(entry = calloc(1, sizeof(*entry)))) return E_OUTOFMEMORY;
    if (!(table = malloc(count * sizeof(*table))))
    {
        free(entry);
        return E_OUTOFMEMORY;
    }

    AcquireSRWLockExclusive(&overlay_lock);
    source = current;
    for (existing = device_overlays; existing; existing = existing->next)
        if (existing->patched == current)
        {
            if (existing->count >= count)
            {
                ReleaseSRWLockExclusive(&overlay_lock);
                free(table);
                free(entry);
                return S_OK;
            }
            source = existing->original;
            break;
        }

    memcpy(table, source, count * sizeof(*table));
    patch_device_vtable(table, count);
    entry->original = source;
    entry->patched = table;
    entry->count = count;
    entry->next = device_overlays;
    device_overlays = entry;
    InterlockedExchangePointer((void *volatile *)object, table);
    ReleaseSRWLockExclusive(&overlay_lock);
    return S_OK;
}


/* WineForge-Internal: wfdxcompat/versioned-root-signature-callable-export-v1.
 * Some export-table consumers call the raw export RVA without resolving PE
 * forwarders. Keep this entry executable and resolve the backend through the
 * loader. Retain the backend reference for the lifetime of returned blobs. */
static INIT_ONCE serialize_versioned_once = INIT_ONCE_STATIC_INIT;
static PFN_D3D12_SERIALIZE_VERSIONED_ROOT_SIGNATURE serialize_versioned_backend;

static BOOL CALLBACK initialize_serialize_versioned(INIT_ONCE *once,
        void *parameter, void **context)
{
    HMODULE module;
    DWORD error;

    (void)once;
    (void)parameter;
    (void)context;
    if (!(module = LoadLibraryW(WFDX_BACKEND_MODULE))) return FALSE;
    serialize_versioned_backend = (PFN_D3D12_SERIALIZE_VERSIONED_ROOT_SIGNATURE)
            (void *)GetProcAddress(module, "D3D12SerializeVersionedRootSignature");
    if (serialize_versioned_backend) return TRUE;
    error = GetLastError();
    FreeLibrary(module);
    SetLastError(error);
    return FALSE;
}

HRESULT WINAPI D3D12SerializeVersionedRootSignature(
        const D3D12_VERSIONED_ROOT_SIGNATURE_DESC *desc,
        ID3DBlob **blob, ID3DBlob **error_blob)
{
    if (!InitOnceExecuteOnce(&serialize_versioned_once,
            initialize_serialize_versioned, NULL, NULL))
        return HRESULT_FROM_WIN32(GetLastError());
    return serialize_versioned_backend(desc, blob, error_blob);
}

typedef HRESULT (WINAPI *backend_create_device_fn)(IUnknown *, D3D_FEATURE_LEVEL, REFIID, void **);
typedef HRESULT (WINAPI *backend_get_interface_fn)(REFCLSID, REFIID, void **);

HRESULT WINAPI D3D12GetInterface(REFCLSID clsid, REFIID iid, void **object)
{
    backend_get_interface_fn get_interface;
    HMODULE module;
    HRESULT hr;

    if (!object) return E_POINTER;
    *object = NULL;
    if ((module = LoadLibraryW(WFDX_BACKEND_MODULE)) &&
        (get_interface = (backend_get_interface_fn)(void *)GetProcAddress(module,
                "D3D12GetInterface")))
    {
        hr = get_interface(clsid, iid, object);
        if (SUCCEEDED(hr)) return hr;
    }
    return wfdx_agility_get_interface(clsid, iid, object);
}

HRESULT WINAPI D3D12CreateDevice(IUnknown *adapter,
        D3D_FEATURE_LEVEL minimum_level, REFIID iid, void **device)
{
    HMODULE module;
    backend_create_device_fn create_device;
    HRESULT hr, wrap_hr;

    wfdx_agility_initialize_from_application();
    if (!(module = LoadLibraryW(WFDX_BACKEND_MODULE))) return HRESULT_FROM_WIN32(GetLastError());
    create_device = (backend_create_device_fn)(void *)GetProcAddress(module, "D3D12CreateDevice");
    if (!create_device) return HRESULT_FROM_WIN32(GetLastError());
    if (!device) return create_device(adapter, minimum_level, iid, NULL);
    *device = NULL;
    hr = create_device(adapter, minimum_level, iid, device);
    if (video_trace_enabled())
    {
        fprintf(stderr, "wfdxcompat-video: D3D12CreateDevice hr=%#lx iid="
                "%08lx-%04x-%04x count=%llu object=%p\n", (unsigned long)hr,
                (unsigned long)iid->Data1, iid->Data2, iid->Data3,
                (unsigned long long)device_vtable_count(iid), device ? *device : NULL);
        fflush(stderr);
    }
    if (FAILED(hr) || !*device) return hr;
    if (FAILED(wrap_hr = wrap_device(*device, iid)))
    {
        IUnknown_Release((IUnknown *)*device);
        *device = NULL;
        return wrap_hr;
    }
    return hr;
}

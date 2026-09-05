#define COBJMACROS
#include <initguid.h>
#include <windows.h>
#include <d3d12.h>
#include <d3dcommon.h>
#include <stdio.h>
#include "wfdxcompat/notifier_registry.h"

#ifndef WFDX_BACKEND_MODULE_A
#define WFDX_BACKEND_MODULE_A "wfdxbackend-d3d12.dll"
#endif

typedef HRESULT (WINAPI *create_device_fn)(IUnknown *, D3D_FEATURE_LEVEL, REFIID, void **);
typedef HRESULT (WINAPI *get_interface_fn)(REFCLSID, REFIID, void **);
typedef void (WINAPI *set_native_fn)(BOOL);
typedef LONG (WINAPI *get_long_fn)(void);
struct callback_state { ID3DDestructionNotifier *notifier; LONG calls; HRESULT reenter_hr; };

__declspec(dllexport) const UINT D3D12SDKVersion = 611;
__declspec(dllexport) const char *D3D12SDKPath = ".\\D3D12-REDIST\\";
struct thread_state { ID3DDestructionNotifier *notifier; LONG *calls; LONG failures; };
struct probe_heap { ID3D12Heap iface; D3D12_HEAP_DESC desc; };

static void STDMETHODCALLTYPE callback(void *data)
{
    struct callback_state *state = data;
    InterlockedIncrement(&state->calls);
    if (state->notifier)
        state->reenter_hr = ID3DDestructionNotifier_UnregisterDestructionCallback(state->notifier, 0xdead);
}

static void STDMETHODCALLTYPE atomic_callback(void *data) { InterlockedIncrement((LONG *)data); }

static DWORD WINAPI registry_thread(void *data)
{
    struct thread_state *state = data;
    UINT id;
    unsigned int i;
    for (i = 0; i < 100; ++i)
    {
        if (ID3DDestructionNotifier_RegisterDestructionCallback(state->notifier,
                atomic_callback, state->calls, &id) != S_OK)
            InterlockedIncrement(&state->failures);
        else if (!(i & 1) && ID3DDestructionNotifier_UnregisterDestructionCallback(
                state->notifier, id) != S_OK)
            InterlockedIncrement(&state->failures);
    }
    return 0;
}

#define check(x) do { if (!(x)) { fprintf(stderr, "FAIL line %u: %s\n", __LINE__, #x); return 1; } } while (0)

static D3D12_HEAP_DESC *STDMETHODCALLTYPE probe_heap_get_desc(ID3D12Heap *iface,
        D3D12_HEAP_DESC *desc)
{
    *desc = ((struct probe_heap *)iface)->desc;
    return desc;
}

static ID3D12HeapVtbl probe_heap_vtable = { .GetDesc = probe_heap_get_desc };

static ID3D12Resource2 *create_placed_resource(ID3D12Device10 *device, UINT64 width,
        UINT64 heap_size, UINT64 heap_offset, D3D12_HEAP_FLAGS heap_flags)
{
    D3D12_RESOURCE_DESC resource_desc = {0};
    struct probe_heap heap = {0};
    ID3D12Resource2 *resource = NULL;
    HRESULT hr;

    heap.iface.lpVtbl = &probe_heap_vtable;
    heap.desc.SizeInBytes = heap_size;
    heap.desc.Properties.Type = D3D12_HEAP_TYPE_UPLOAD;
    heap.desc.Properties.CreationNodeMask = 1;
    heap.desc.Properties.VisibleNodeMask = 1;
    heap.desc.Flags = heap_flags;
    resource_desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    resource_desc.Width = width;
    resource_desc.Height = 1;
    resource_desc.DepthOrArraySize = 1;
    resource_desc.MipLevels = 1;
    resource_desc.SampleDesc.Count = 1;
    resource_desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    hr = ID3D12Device10_CreatePlacedResource(device, &heap.iface, heap_offset, &resource_desc,
            D3D12_RESOURCE_STATE_GENERIC_READ, NULL, &IID_ID3D12Resource2,
            (void **)&resource);
    return SUCCEEDED(hr) ? resource : NULL;
}

static ID3D12Resource2 *create_committed_resource(ID3D12Device10 *device,
        D3D12_HEAP_TYPE heap_type, UINT64 width, D3D12_RESOURCE_STATES state)
{
    D3D12_HEAP_PROPERTIES heap_properties = {0};
    D3D12_RESOURCE_DESC resource_desc = {0};
    ID3D12Resource2 *resource = NULL;
    HRESULT hr;

    heap_properties.Type = heap_type;
    heap_properties.CreationNodeMask = 1;
    heap_properties.VisibleNodeMask = 1;
    resource_desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    resource_desc.Width = width;
    resource_desc.Height = 1;
    resource_desc.DepthOrArraySize = 1;
    resource_desc.MipLevels = 1;
    resource_desc.SampleDesc.Count = 1;
    resource_desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    hr = ID3D12Device10_CreateCommittedResource(device, &heap_properties,
            D3D12_HEAP_FLAG_NONE, &resource_desc, state, NULL,
            &IID_ID3D12Resource2, (void **)&resource);
    return SUCCEEDED(hr) ? resource : NULL;
}

static ID3D12Resource2 *create_placed_nv12_resource(ID3D12Device10 *device)
{
    D3D12_RESOURCE_DESC resource_desc = {0};
    struct probe_heap heap = {0};
    ID3D12Resource2 *resource = NULL;
    HRESULT hr;

    heap.iface.lpVtbl = &probe_heap_vtable;
    heap.desc.SizeInBytes = 4 * 1024 * 1024;
    heap.desc.Properties.Type = D3D12_HEAP_TYPE_DEFAULT;
    heap.desc.Properties.CreationNodeMask = 1;
    heap.desc.Properties.VisibleNodeMask = 1;
    heap.desc.Flags = D3D12_HEAP_FLAG_ALLOW_ONLY_NON_RT_DS_TEXTURES;
    resource_desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    resource_desc.Width = 1920;
    resource_desc.Height = 1088;
    resource_desc.DepthOrArraySize = 1;
    resource_desc.MipLevels = 1;
    resource_desc.Format = DXGI_FORMAT_NV12;
    resource_desc.SampleDesc.Count = 1;
    hr = ID3D12Device10_CreatePlacedResource(device, &heap.iface, 0, &resource_desc,
            D3D12_RESOURCE_STATE_COPY_DEST, NULL, &IID_ID3D12Resource2,
            (void **)&resource);
    return SUCCEEDED(hr) ? resource : NULL;
}

int main(void)
{
    HMODULE frontend = LoadLibraryA("d3d12.dll"), backend;
    create_device_fn create_device;
    get_interface_fn get_interface;
    ID3D12SDKConfiguration *sdk_configuration;
    set_native_fn set_native;
    get_long_fn get_native_registers, get_destroyed, get_device_destroyed, get_resource_queries;
    ID3D12Device10 *device, *device2;
    ID3D12Resource2 *resource, *resource_qi;
    ID3DDestructionNotifier *notifier;
    IUnknown *identity;
    struct callback_state removed = {0}, fired = {0}, pooled = {0}, native = {0};
    UINT removed_id, fired_id, pooled_id, native_id;
    ID3D12Device *base_device;
    D3D12_RESOURCE_ALLOCATION_INFO allocation_info;
    D3D12_RESOURCE_DESC nv12_desc = {0};
    HANDLE threads[4];
    struct thread_state thread_states[4];
    LONG destroyed_before_native, threaded_calls = 0;
    unsigned int i;

    check(frontend != NULL);
    create_device = (create_device_fn)(void *)GetProcAddress(frontend, "D3D12CreateDevice");
    get_interface = (get_interface_fn)(void *)GetProcAddress(frontend, "D3D12GetInterface");
    check(create_device != NULL);
    check(get_interface != NULL);
    check(create_device(NULL, D3D_FEATURE_LEVEL_11_0, &IID_ID3D12Device10, (void **)&device) == S_OK);
    check(GetModuleHandleA("d3d12core.dll") != NULL);
    sdk_configuration = NULL;
    check(get_interface(&CLSID_D3D12SDKConfiguration, &IID_ID3D12SDKConfiguration,
            (void **)&sdk_configuration) == S_OK);
    check(sdk_configuration != NULL);
    check(ID3D12SDKConfiguration_SetSDKVersion(sdk_configuration, 610,
            ".\\D3D12\\") == S_OK);
    check(ID3D12SDKConfiguration_Release(sdk_configuration) == 0);
    check(ID3D12Device10_QueryInterface(device, &IID_IUnknown, (void **)&identity) == S_OK);
    check(identity == (IUnknown *)device);
    check(IUnknown_Release(identity) == 1);
    check(ID3D12Device10_QueryInterface(device, &IID_ID3D12Device, (void **)&base_device) == S_OK);
    check((void *)base_device == (void *)device);
    check(ID3D12Device_Release(base_device) == 1);

    nv12_desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    nv12_desc.Width = 1920;
    nv12_desc.Height = 1088;
    nv12_desc.DepthOrArraySize = 1;
    nv12_desc.MipLevels = 1;
    nv12_desc.Format = DXGI_FORMAT_NV12;
    nv12_desc.SampleDesc.Count = 1;
    device->lpVtbl->GetResourceAllocationInfo(device, &allocation_info, 0, 1, &nv12_desc);
    check(allocation_info.SizeInBytes != ~(UINT64)0);
    check(allocation_info.Alignment == D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT);

    backend = GetModuleHandleA(WFDX_BACKEND_MODULE_A);
    check(backend != NULL);
    set_native = (set_native_fn)(void *)GetProcAddress(backend, "fake_set_native_notifier");
    get_native_registers = (get_long_fn)(void *)GetProcAddress(backend, "fake_get_native_registers");
    get_destroyed = (get_long_fn)(void *)GetProcAddress(backend, "fake_get_resource_destroyed");
    get_device_destroyed = (get_long_fn)(void *)GetProcAddress(backend, "fake_get_device_destroyed");
    get_resource_queries = (get_long_fn)(void *)GetProcAddress(backend, "fake_get_resource_queries");
    check(set_native && get_native_registers && get_destroyed && get_device_destroyed &&
            get_resource_queries);

    /* A second factory call must create an independent overlay and COM identity. */
    check(create_device(NULL, D3D_FEATURE_LEVEL_11_0, &IID_ID3D12Device10,
            (void **)&device2) == S_OK);
    check(device2 != device);
    check(ID3D12Device10_QueryInterface(device2, &IID_IUnknown, (void **)&identity) == S_OK);
    check(identity == (IUnknown *)device2 && identity != (IUnknown *)device);
    check(IUnknown_Release(identity) == 1);
    check(ID3D12Device10_Release(device2) == 0);
    check(get_device_destroyed() == 1);

    /* A non-matching placed resource remains entirely backend-native. */
    set_native(FALSE);
    resource = create_placed_resource(device, 32768, 65536, 0,
            D3D12_HEAP_FLAG_ALLOW_ONLY_BUFFERS);
    check(resource != NULL);
    check(ID3D12Resource2_QueryInterface(resource, &IID_ID3DDestructionNotifier,
            (void **)&notifier) == E_NOINTERFACE);
    check(ID3D12Resource2_Release(resource) == 0);

    /* A smaller committed upload buffer remains backend-native. */
    resource = create_committed_resource(device, D3D12_HEAP_TYPE_UPLOAD, 65536,
            D3D12_RESOURCE_STATE_GENERIC_READ);
    check(resource != NULL);
    check(ID3D12Resource2_QueryInterface(resource, &IID_ID3DDestructionNotifier,
            (void **)&notifier) == E_NOINTERFACE);
    check(ID3D12Resource2_Release(resource) == 0);

    /* The previously suspected 64 KiB readback buffer is not the Electra object. */
    resource = create_committed_resource(device, D3D12_HEAP_TYPE_READBACK, 65536,
            D3D12_RESOURCE_STATE_COPY_DEST);
    check(resource != NULL);
    check(ID3D12Resource2_QueryInterface(resource, &IID_ID3DDestructionNotifier,
            (void **)&notifier) == E_NOINTERFACE);
    check(ID3D12Resource2_Release(resource) == 0);

    /* Only Electra's exact placed upload buffer receives the notifier overlay. */
    resource = create_placed_resource(device, 65536, 65536, 0,
            D3D12_HEAP_FLAG_ALLOW_ONLY_BUFFERS);
    check(resource != NULL);
    check(ID3D12Resource2_QueryInterface(resource, &IID_ID3D12Resource2, (void **)&resource_qi) == S_OK);
    check(resource_qi == resource);
    check(ID3D12Resource2_Release(resource_qi) == 1);
    check(ID3D12Resource2_QueryInterface(resource, &IID_ID3DDestructionNotifier,
            (void **)&notifier) == S_OK);
    check(ID3DDestructionNotifier_QueryInterface(notifier, &IID_IUnknown, (void **)&identity) == S_OK);
    check(identity == (IUnknown *)resource);
    check(IUnknown_Release(identity) == 2);
    removed.notifier = notifier;
    fired.notifier = notifier;
    check(ID3DDestructionNotifier_RegisterDestructionCallback(notifier, callback,
            &removed, &removed_id) == S_OK);
    check(ID3DDestructionNotifier_RegisterDestructionCallback(notifier, callback,
            &fired, &fired_id) == S_OK);
    check(ID3DDestructionNotifier_UnregisterDestructionCallback(notifier, removed_id) == S_OK);
    check(ID3DDestructionNotifier_Release(notifier) == 1);
    check(ID3D12Resource2_Release(resource) == 0);
    check(removed.calls == 0 && fired.calls == 1);
    check(fired.reenter_hr == WFDX_E_CLOSED);

    /* Match the real Electra pool: multiple aligned slots and CREATE_NOT_ZEROED. */
    resource = create_placed_resource(device, 256 * 1024, 8 * 256 * 1024,
            7 * 256 * 1024, D3D12_HEAP_FLAG_ALLOW_ONLY_BUFFERS |
            D3D12_HEAP_FLAG_CREATE_NOT_ZEROED);
    check(resource != NULL);
    check(ID3D12Resource2_QueryInterface(resource, &IID_ID3DDestructionNotifier,
            (void **)&notifier) == S_OK);
    check(ID3DDestructionNotifier_RegisterDestructionCallback(notifier, callback,
            &pooled, &pooled_id) == S_OK);
    check(ID3DDestructionNotifier_Release(notifier) == 1);
    check(ID3D12Resource2_Release(resource) == 0);
    check(pooled.calls == 1);

    /* A non-slot-aligned offset must not receive the Electra overlay. */
    resource = create_placed_resource(device, 256 * 1024, 8 * 256 * 1024,
            128 * 1024, D3D12_HEAP_FLAG_ALLOW_ONLY_BUFFERS |
            D3D12_HEAP_FLAG_CREATE_NOT_ZEROED);
    check(resource != NULL);
    check(ID3D12Resource2_QueryInterface(resource, &IID_ID3DDestructionNotifier,
            (void **)&notifier) == E_NOINTERFACE);
    check(ID3D12Resource2_Release(resource) == 0);

    /* Committed upload buffers remain backend-native. */
    resource = create_committed_resource(device, D3D12_HEAP_TYPE_UPLOAD, 8 * 1024 * 1024,
            D3D12_RESOURCE_STATE_GENERIC_READ);
    check(resource != NULL);
    check(ID3D12Resource2_QueryInterface(resource, &IID_ID3DDestructionNotifier,
            (void **)&notifier) == E_NOINTERFACE);
    check(ID3D12Resource2_Release(resource) == 0);

    /* Electra's placed NV12 output must be emulated before reaching a backend
     * which cannot represent native NV12 textures. */
    resource = create_placed_nv12_resource(device);
    check(resource != NULL);
    check(ID3D12Resource2_QueryInterface(resource, &IID_ID3DDestructionNotifier,
            (void **)&notifier) == S_OK);
    check(ID3DDestructionNotifier_Release(notifier) == 1);
    check(ID3D12Resource2_Release(resource) == 0);

    resource = create_placed_resource(device, 65536, 65536, 0,
            D3D12_HEAP_FLAG_ALLOW_ONLY_BUFFERS);
    check(resource != NULL);
    check(ID3D12Resource2_QueryInterface(resource, &IID_ID3DDestructionNotifier,
            (void **)&notifier) == S_OK);
    for (i = 0; i < 4; ++i)
    {
        thread_states[i].notifier = notifier;
        thread_states[i].calls = &threaded_calls;
        thread_states[i].failures = 0;
        threads[i] = CreateThread(NULL, 0, registry_thread, &thread_states[i], 0, NULL);
        check(threads[i] != NULL);
    }
    check(WaitForMultipleObjects(4, threads, TRUE, INFINITE) == WAIT_OBJECT_0);
    for (i = 0; i < 4; ++i)
    {
        check(thread_states[i].failures == 0);
        CloseHandle(threads[i]);
    }
    check(ID3DDestructionNotifier_Release(notifier) == 1);
    check(ID3D12Resource2_Release(resource) == 0);
    check(threaded_calls == 200);

    destroyed_before_native = get_destroyed();
    set_native(TRUE);
    resource = create_placed_resource(device, 65536, 65536, 0,
            D3D12_HEAP_FLAG_ALLOW_ONLY_BUFFERS);
    check(resource != NULL);
    check(ID3D12Resource2_QueryInterface(resource, &IID_ID3DDestructionNotifier, (void **)&notifier) == S_OK);
    check(ID3DDestructionNotifier_QueryInterface(notifier, &IID_IUnknown, (void **)&identity) == S_OK);
    check(identity == (IUnknown *)resource);
    check(IUnknown_Release(identity) == 2);
    check(ID3DDestructionNotifier_RegisterDestructionCallback(notifier, callback, &native, &native_id) == S_OK);
    check(native_id == 77 && get_native_registers() == 1);
    check(ID3DDestructionNotifier_Release(notifier) == 1);
    check(ID3D12Resource2_Release(resource) == 0);
    check(native.calls == 1 && get_destroyed() == destroyed_before_native + 1);
    check(ID3D12Device10_Release(device) == 0);
    check(get_device_destroyed() == 2);
    puts("d3d12_proxy_notifier: PASS");
    return 0;
}

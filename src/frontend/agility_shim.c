#define COBJMACROS
#include <initguid.h>
#include <windows.h>
#include <d3d12.h>
#include <stdlib.h>
#include <wchar.h>
#include "wfdxcompat/agility_shim.h"

static INIT_ONCE legacy_exports_once = INIT_ONCE_STATIC_INIT;
static SRWLOCK agility_core_lock = SRWLOCK_INIT;
static HMODULE agility_core;
static const char frontend_anchor;

struct agility_configuration
{
    ID3D12SDKConfiguration iface;
    LONG refs;
};

static HRESULT activate_agility_core(UINT version, const char *path)
{
    WCHAR core_path[MAX_PATH];
    WCHAR *filename;
    HMODULE frontend, module;
    DWORD length, error;
    size_t directory_length;
    HRESULT hr = S_OK;

    if (!version || !path || !path[0])
        return E_INVALIDARG;

    AcquireSRWLockExclusive(&agility_core_lock);
    if (agility_core) goto done;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
            GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            (const WCHAR *)&frontend_anchor, &frontend))
    {
        hr = HRESULT_FROM_WIN32(GetLastError());
        goto done;
    }
    length = GetModuleFileNameW(frontend, core_path, ARRAYSIZE(core_path));
    if (!length || length == ARRAYSIZE(core_path))
    {
        error = GetLastError();
        hr = HRESULT_FROM_WIN32(error ? error : ERROR_INSUFFICIENT_BUFFER);
        goto done;
    }
    if (!(filename = wcsrchr(core_path, L'\\')) &&
        !(filename = wcsrchr(core_path, L'/')))
    {
        hr = HRESULT_FROM_WIN32(ERROR_BAD_PATHNAME);
        goto done;
    }
    directory_length = filename - core_path + 1;
    if (directory_length + ARRAYSIZE(L"d3d12core.dll") > ARRAYSIZE(core_path))
    {
        hr = HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER);
        goto done;
    }
    memcpy(core_path + directory_length, L"d3d12core.dll",
            sizeof(L"d3d12core.dll"));
    if (!(module = LoadLibraryExW(core_path, NULL, LOAD_WITH_ALTERED_SEARCH_PATH)))
        hr = HRESULT_FROM_WIN32(GetLastError());
    else
        agility_core = module;

done:
    ReleaseSRWLockExclusive(&agility_core_lock);
    return hr;
}

static BOOL CALLBACK initialize_from_legacy_exports(INIT_ONCE *once, void *parameter,
        void **context)
{
    const UINT *version_export;
    const char *const *path_export;
    HMODULE application = GetModuleHandleW(NULL);

    (void)once;
    (void)parameter;
    (void)context;
    version_export = (const UINT *)(void *)GetProcAddress(application,
            "D3D12SDKVersion");
    path_export = (const char *const *)(void *)GetProcAddress(application,
            "D3D12SDKPath");
    if (version_export && path_export && *path_export)
        activate_agility_core(*version_export, *path_export);
    return TRUE;
}

void wfdx_agility_initialize_from_application(void)
{
    InitOnceExecuteOnce(&legacy_exports_once, initialize_from_legacy_exports, NULL, NULL);
}

static struct agility_configuration *impl_from_ID3D12SDKConfiguration(
        ID3D12SDKConfiguration *iface)
{
    return CONTAINING_RECORD(iface, struct agility_configuration, iface);
}

static HRESULT STDMETHODCALLTYPE agility_query_interface(ID3D12SDKConfiguration *iface,
        REFIID iid, void **object)
{
    if (!object) return E_POINTER;
    *object = NULL;
    if (!IsEqualIID(iid, &IID_IUnknown) &&
        !IsEqualIID(iid, &IID_ID3D12SDKConfiguration)) return E_NOINTERFACE;
    *object = iface;
    ID3D12SDKConfiguration_AddRef(iface);
    return S_OK;
}

static ULONG STDMETHODCALLTYPE agility_add_ref(ID3D12SDKConfiguration *iface)
{
    return InterlockedIncrement(&impl_from_ID3D12SDKConfiguration(iface)->refs);
}

static ULONG STDMETHODCALLTYPE agility_release(ID3D12SDKConfiguration *iface)
{
    struct agility_configuration *configuration =
            impl_from_ID3D12SDKConfiguration(iface);
    ULONG refs = InterlockedDecrement(&configuration->refs);

    if (!refs) free(configuration);
    return refs;
}

static HRESULT STDMETHODCALLTYPE agility_set_sdk_version(ID3D12SDKConfiguration *iface,
        UINT version, const char *path)
{
    (void)iface;
    return activate_agility_core(version, path);
}

static ID3D12SDKConfigurationVtbl agility_vtable =
{
    agility_query_interface,
    agility_add_ref,
    agility_release,
    agility_set_sdk_version,
};

HRESULT wfdx_agility_get_interface(REFCLSID clsid, REFIID iid, void **object)
{
    struct agility_configuration *configuration;
    HRESULT hr;

    if (!object) return E_POINTER;
    *object = NULL;
    if (!IsEqualGUID(clsid, &CLSID_D3D12SDKConfiguration)) return E_NOINTERFACE;
    if (!(configuration = calloc(1, sizeof(*configuration)))) return E_OUTOFMEMORY;
    configuration->iface.lpVtbl = &agility_vtable;
    configuration->refs = 1;
    hr = ID3D12SDKConfiguration_QueryInterface(&configuration->iface, iid, object);
    ID3D12SDKConfiguration_Release(&configuration->iface);
    return hr;
}

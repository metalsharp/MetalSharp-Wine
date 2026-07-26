#define COBJMACROS
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include "wfdxcompat/d3d11_gdi_surface.h"

struct gdi_texture
{
    ID3D11Texture2DVtbl texture_vtable;
    const ID3D11Texture2DVtbl *original_vtable;
    ID3D11Texture2D *texture;
    IDXGISurface1 surface;
    D3D11_TEXTURE2D_DESC desc;
    CRITICAL_SECTION lock;
    ID3D11Texture2D *staging;
    HDC dc;
    HBITMAP bitmap;
    HGDIOBJ old_bitmap;
    BYTE *bits;
    BOOL dc_acquired;
};

static HRESULT STDMETHODCALLTYPE texture_query_interface(ID3D11Texture2D *, REFIID, void **);
static ULONG STDMETHODCALLTYPE texture_release(ID3D11Texture2D *);

static struct gdi_texture *impl_from_texture(ID3D11Texture2D *texture)
{
    return CONTAINING_RECORD(texture->lpVtbl, struct gdi_texture, texture_vtable);
}

static struct gdi_texture *impl_from_surface(IDXGISurface1 *surface)
{
    return CONTAINING_RECORD(surface, struct gdi_texture, surface);
}

static HRESULT STDMETHODCALLTYPE surface_query_interface(IDXGISurface1 *iface, REFIID iid,
        void **out)
{
    struct gdi_texture *impl = impl_from_surface(iface);

    if (!out) return E_POINTER;
    *out = NULL;
    if (IsEqualGUID(iid, &IID_IDXGISurface) || IsEqualGUID(iid, &IID_IDXGISurface1))
    {
        *out = iface;
        ID3D11Texture2D_AddRef(impl->texture);
        return S_OK;
    }
    return ID3D11Texture2D_QueryInterface(impl->texture, iid, out);
}

static ULONG STDMETHODCALLTYPE surface_add_ref(IDXGISurface1 *iface)
{
    return ID3D11Texture2D_AddRef(impl_from_surface(iface)->texture);
}

static ULONG STDMETHODCALLTYPE surface_release(IDXGISurface1 *iface)
{
    return ID3D11Texture2D_Release(impl_from_surface(iface)->texture);
}

static HRESULT STDMETHODCALLTYPE surface_set_private_data(IDXGISurface1 *iface, REFGUID guid,
        UINT size, const void *data)
{
    return ID3D11Texture2D_SetPrivateData(impl_from_surface(iface)->texture, guid, size, data);
}

static HRESULT STDMETHODCALLTYPE surface_set_private_data_interface(IDXGISurface1 *iface,
        REFGUID guid, const IUnknown *object)
{
    return ID3D11Texture2D_SetPrivateDataInterface(impl_from_surface(iface)->texture, guid, object);
}

static HRESULT STDMETHODCALLTYPE surface_get_private_data(IDXGISurface1 *iface, REFGUID guid,
        UINT *size, void *data)
{
    return ID3D11Texture2D_GetPrivateData(impl_from_surface(iface)->texture, guid, size, data);
}

static HRESULT surface_get_device(struct gdi_texture *impl, REFIID iid, void **out)
{
    ID3D11Device *device;
    HRESULT hr;

    if (!out) return E_POINTER;
    *out = NULL;
    ID3D11Texture2D_GetDevice(impl->texture, &device);
    if (!device) return E_NOINTERFACE;
    hr = ID3D11Device_QueryInterface(device, iid, out);
    ID3D11Device_Release(device);
    return hr;
}

static HRESULT STDMETHODCALLTYPE surface_get_parent(IDXGISurface1 *iface, REFIID iid, void **out)
{
    return surface_get_device(impl_from_surface(iface), iid, out);
}

static HRESULT STDMETHODCALLTYPE surface_get_device_iface(IDXGISurface1 *iface, REFIID iid,
        void **out)
{
    return surface_get_device(impl_from_surface(iface), iid, out);
}

static HRESULT STDMETHODCALLTYPE surface_get_desc(IDXGISurface1 *iface, DXGI_SURFACE_DESC *desc)
{
    struct gdi_texture *impl = impl_from_surface(iface);

    if (!desc) return E_INVALIDARG;
    desc->Width = impl->desc.Width;
    desc->Height = impl->desc.Height;
    desc->Format = impl->desc.Format;
    desc->SampleDesc = impl->desc.SampleDesc;
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE surface_map(IDXGISurface1 *iface, DXGI_MAPPED_RECT *mapped,
        UINT flags)
{
    (void)iface;
    (void)mapped;
    (void)flags;
    return DXGI_ERROR_UNSUPPORTED;
}

static HRESULT STDMETHODCALLTYPE surface_unmap(IDXGISurface1 *iface)
{
    (void)iface;
    return DXGI_ERROR_UNSUPPORTED;
}

static HRESULT ensure_dib(struct gdi_texture *impl)
{
    BITMAPINFO info;

    if (impl->dc) return S_OK;
    memset(&info, 0, sizeof(info));
    info.bmiHeader.biSize = sizeof(info.bmiHeader);
    info.bmiHeader.biWidth = impl->desc.Width;
    info.bmiHeader.biHeight = -(LONG)impl->desc.Height;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    if (!(impl->dc = CreateCompatibleDC(NULL))) return HRESULT_FROM_WIN32(GetLastError());
    impl->bitmap = CreateDIBSection(impl->dc, &info, DIB_RGB_COLORS,
            (void **)&impl->bits, NULL, 0);
    if (!impl->bitmap)
    {
        DeleteDC(impl->dc);
        impl->dc = NULL;
        return HRESULT_FROM_WIN32(GetLastError());
    }
    impl->old_bitmap = SelectObject(impl->dc, impl->bitmap);
    return S_OK;
}

static HRESULT ensure_staging(struct gdi_texture *impl, ID3D11Device *device)
{
    D3D11_TEXTURE2D_DESC desc = impl->desc;

    if (impl->staging) return S_OK;
    desc.Usage = D3D11_USAGE_STAGING;
    desc.BindFlags = 0;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    desc.MiscFlags = 0;
    return ID3D11Device_CreateTexture2D(device, &desc, NULL, &impl->staging);
}

static HRESULT STDMETHODCALLTYPE surface_get_dc(IDXGISurface1 *iface, BOOL discard, HDC *dc)
{
    struct gdi_texture *impl = impl_from_surface(iface);
    ID3D11DeviceContext *context = NULL;
    ID3D11Device *device = NULL;
    D3D11_MAPPED_SUBRESOURCE mapped;
    UINT y, row_size = impl->desc.Width * 4;
    HRESULT hr;

    if (!dc) return E_INVALIDARG;
    *dc = NULL;
    EnterCriticalSection(&impl->lock);
    if (impl->dc_acquired)
    {
        hr = DXGI_ERROR_INVALID_CALL;
        goto done;
    }
    if (FAILED(hr = ensure_dib(impl))) goto done;
    if (!discard)
    {
        ID3D11Texture2D_GetDevice(impl->texture, &device);
        if (!device)
        {
            hr = E_NOINTERFACE;
            goto done;
        }
        if (FAILED(hr = ensure_staging(impl, device))) goto done;
        ID3D11Device_GetImmediateContext(device, &context);
        if (!context)
        {
            hr = E_NOINTERFACE;
            goto done;
        }
        ID3D11DeviceContext_CopyResource(context, (ID3D11Resource *)impl->staging,
                (ID3D11Resource *)impl->texture);
        hr = ID3D11DeviceContext_Map(context, (ID3D11Resource *)impl->staging, 0,
                D3D11_MAP_READ, 0, &mapped);
        if (FAILED(hr)) goto done;
        for (y = 0; y < impl->desc.Height; ++y)
            memcpy(impl->bits + y * row_size, (BYTE *)mapped.pData + y * mapped.RowPitch,
                    row_size);
        ID3D11DeviceContext_Unmap(context, (ID3D11Resource *)impl->staging, 0);
    }
    impl->dc_acquired = TRUE;
    *dc = impl->dc;
    hr = S_OK;

done:
    if (context) ID3D11DeviceContext_Release(context);
    if (device) ID3D11Device_Release(device);
    LeaveCriticalSection(&impl->lock);
    return hr;
}

static HRESULT STDMETHODCALLTYPE surface_release_dc(IDXGISurface1 *iface, RECT *dirty)
{
    struct gdi_texture *impl = impl_from_surface(iface);
    ID3D11DeviceContext *context = NULL;
    ID3D11Device *device = NULL;
    D3D11_BOX box;
    const BYTE *data;
    UINT row_pitch = impl->desc.Width * 4;
    HRESULT hr = S_OK;

    EnterCriticalSection(&impl->lock);
    if (!impl->dc_acquired)
    {
        hr = DXGI_ERROR_INVALID_CALL;
        goto done;
    }
    box.left = dirty ? max(0, dirty->left) : 0;
    box.top = dirty ? max(0, dirty->top) : 0;
    box.right = dirty ? min((LONG)impl->desc.Width, dirty->right) : impl->desc.Width;
    box.bottom = dirty ? min((LONG)impl->desc.Height, dirty->bottom) : impl->desc.Height;
    box.front = 0;
    box.back = 1;
    if (box.left < box.right && box.top < box.bottom)
    {
        ID3D11Texture2D_GetDevice(impl->texture, &device);
        if (!device)
        {
            hr = E_NOINTERFACE;
            goto done;
        }
        ID3D11Device_GetImmediateContext(device, &context);
        if (!context)
        {
            hr = E_NOINTERFACE;
            goto done;
        }
        data = impl->bits + box.top * row_pitch + box.left * 4;
        ID3D11DeviceContext_UpdateSubresource(context, (ID3D11Resource *)impl->texture,
                0, &box, data, row_pitch, row_pitch * impl->desc.Height);
    }
    impl->dc_acquired = FALSE;

done:
    if (context) ID3D11DeviceContext_Release(context);
    if (device) ID3D11Device_Release(device);
    LeaveCriticalSection(&impl->lock);
    return hr;
}

static IDXGISurface1Vtbl surface_vtable =
{
    surface_query_interface,
    surface_add_ref,
    surface_release,
    surface_set_private_data,
    surface_set_private_data_interface,
    surface_get_private_data,
    surface_get_parent,
    surface_get_device_iface,
    surface_get_desc,
    surface_map,
    surface_unmap,
    surface_get_dc,
    surface_release_dc,
};

static HRESULT STDMETHODCALLTYPE texture_query_interface(ID3D11Texture2D *texture, REFIID iid,
        void **out)
{
    struct gdi_texture *impl = impl_from_texture(texture);

    if (!out) return E_POINTER;
    *out = NULL;
    if (IsEqualGUID(iid, &IID_IDXGISurface) || IsEqualGUID(iid, &IID_IDXGISurface1))
    {
        *out = &impl->surface;
        impl->original_vtable->AddRef(texture);
        return S_OK;
    }
    return impl->original_vtable->QueryInterface(texture, iid, out);
}

static void destroy_overlay(struct gdi_texture *impl)
{
    if (impl->staging) ID3D11Texture2D_Release(impl->staging);
    if (impl->dc)
    {
        SelectObject(impl->dc, impl->old_bitmap);
        DeleteObject(impl->bitmap);
        DeleteDC(impl->dc);
    }
    DeleteCriticalSection(&impl->lock);
    free(impl);
}

static ULONG STDMETHODCALLTYPE texture_release(ID3D11Texture2D *texture)
{
    struct gdi_texture *impl = impl_from_texture(texture);
    ULONG refs = impl->original_vtable->Release(texture);

    if (!refs) destroy_overlay(impl);
    return refs;
}

HRESULT wfdx_gdi_wrap_texture(ID3D11Texture2D *texture)
{
    struct gdi_texture *impl;
    D3D11_TEXTURE2D_DESC desc;
    const ID3D11Texture2DVtbl *original;

    if (!texture) return E_INVALIDARG;
    if (texture->lpVtbl->QueryInterface == texture_query_interface) return S_FALSE;
    ID3D11Texture2D_GetDesc(texture, &desc);
    if (desc.Format != DXGI_FORMAT_B8G8R8A8_UNORM ||
            desc.Usage != D3D11_USAGE_DEFAULT || desc.MipLevels != 1 ||
            desc.ArraySize != 1 || desc.SampleDesc.Count != 1 ||
            !(desc.MiscFlags & D3D11_RESOURCE_MISC_GDI_COMPATIBLE))
        return S_FALSE;
    if (!(impl = calloc(1, sizeof(*impl)))) return E_OUTOFMEMORY;
    original = texture->lpVtbl;
    impl->texture_vtable = *original;
    impl->original_vtable = original;
    impl->texture = texture;
    impl->surface.lpVtbl = &surface_vtable;
    impl->desc = desc;
    InitializeCriticalSection(&impl->lock);
    impl->texture_vtable.QueryInterface = texture_query_interface;
    impl->texture_vtable.Release = texture_release;
    if (InterlockedCompareExchangePointer((void *volatile *)&texture->lpVtbl,
            &impl->texture_vtable, (void *)original) != original)
    {
        DeleteCriticalSection(&impl->lock);
        free(impl);
        return E_UNEXPECTED;
    }
    return S_OK;
}

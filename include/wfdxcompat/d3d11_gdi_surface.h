#ifndef WFDXCOMPAT_D3D11_GDI_SURFACE_H
#define WFDXCOMPAT_D3D11_GDI_SURFACE_H

#include <d3d11.h>

/*
 * Installs a per-instance QueryInterface overlay.  The caller must only invoke
 * this for textures created while the launcher profile is active.
 */
HRESULT wfdx_gdi_wrap_texture(ID3D11Texture2D *texture);

#endif

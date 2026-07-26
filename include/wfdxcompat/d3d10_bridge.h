#ifndef WFDXCOMPAT_D3D10_BRIDGE_H
#define WFDXCOMPAT_D3D10_BRIDGE_H

#include <d3d10_1.h>
#include <d3d11.h>

HRESULT wfdx_d3d10_bridge_create(ID3D11Device *device, UINT d3d10_flags,
        ID3D10Device1 **out);
HRESULT wfdx_d3d10_bridge_query(ID3D11Device *device, REFIID iid, void **out);

#endif

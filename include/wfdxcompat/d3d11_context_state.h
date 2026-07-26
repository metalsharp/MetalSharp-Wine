#ifndef WFDXCOMPAT_D3D11_CONTEXT_STATE_H
#define WFDXCOMPAT_D3D11_CONTEXT_STATE_H

#include <d3d11_1.h>

#ifdef __cplusplus
extern "C" {
#endif

HRESULT wfdx_context_state_create(ID3D11Device1 *device, UINT flags,
        const D3D_FEATURE_LEVEL *feature_levels, UINT feature_level_count,
        UINT sdk_version, REFIID emulated_interface,
        D3D_FEATURE_LEVEL *chosen_feature_level,
        ID3DDeviceContextState **context_state);

HRESULT wfdx_context_state_wrap_context(ID3D11DeviceContext1 *context,
        ID3D11Device *device);

#ifdef __cplusplus
}
#endif

#endif

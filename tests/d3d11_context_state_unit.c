#define COBJMACROS
#include <windows.h>
#include <d3d11_1.h>
#include <stdio.h>

/*
 * Focused public-ABI runtime probe. Under a routed launcher name every call
 * enters through d3d11.dll; this test does not call WFDX internals.
 */
int main(void)
{
    D3D_FEATURE_LEVEL requested[] = {D3D_FEATURE_LEVEL_11_0};
    D3D_FEATURE_LEVEL chosen;
    ID3D11Device *device = NULL;
    ID3D11Device1 *device1 = NULL;
    ID3D11DeviceContext *context = NULL;
    ID3D11DeviceContext1 *context1 = NULL;
    ID3DDeviceContextState *a = NULL, *b = NULL, *previous = NULL;
    D3D11_PRIMITIVE_TOPOLOGY topology;
    HRESULT hr;
    int result = 1;

    hr = D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 0,
            requested, 1, D3D11_SDK_VERSION, &device, &chosen, &context);
    if (FAILED(hr)) goto done;
    hr = ID3D11Device_QueryInterface(device, &IID_ID3D11Device1,
            (void **)&device1);
    if (FAILED(hr)) goto done;
    hr = ID3D11DeviceContext_QueryInterface(context,
            &IID_ID3D11DeviceContext1, (void **)&context1);
    if (FAILED(hr)) goto done;
    hr = ID3D11Device1_CreateDeviceContextState(device1, 0, requested, 1,
            D3D11_SDK_VERSION, &IID_ID3D11Device1, &chosen, &a);
    if (FAILED(hr)) goto done;
    hr = ID3D11Device1_CreateDeviceContextState(device1, 0, requested, 1,
            D3D11_SDK_VERSION, &IID_ID3D11Device1, &chosen, &b);
    if (FAILED(hr)) goto done;

    ID3D11DeviceContext1_IASetPrimitiveTopology(context1,
            D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D11DeviceContext1_SwapDeviceContextState(context1, a, &previous);
    if (!previous) goto done;
    ID3DDeviceContextState_Release(previous);
    previous = NULL;
    ID3D11DeviceContext1_IASetPrimitiveTopology(context1,
            D3D11_PRIMITIVE_TOPOLOGY_LINELIST);
    ID3D11DeviceContext1_SwapDeviceContextState(context1, b, &previous);
    if (previous != a) goto done;
    ID3DDeviceContextState_Release(previous);
    previous = NULL;
    ID3D11DeviceContext1_IASetPrimitiveTopology(context1,
            D3D11_PRIMITIVE_TOPOLOGY_POINTLIST);
    ID3D11DeviceContext1_SwapDeviceContextState(context1, a, &previous);
    if (previous != b) goto done;
    ID3DDeviceContextState_Release(previous);
    previous = NULL;
    ID3D11DeviceContext1_IAGetPrimitiveTopology(context1, &topology);
    if (topology != D3D11_PRIMITIVE_TOPOLOGY_LINELIST) goto done;
    ID3D11DeviceContext1_SwapDeviceContextState(context1, NULL, &previous);
    if (previous != a) goto done;
    ID3DDeviceContextState_Release(previous);
    previous = NULL;
    ID3D11DeviceContext1_IAGetPrimitiveTopology(context1, &topology);
    if (topology != D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST) goto done;
    result = 0;

done:
    if (previous) ID3DDeviceContextState_Release(previous);
    if (b) ID3DDeviceContextState_Release(b);
    if (a) ID3DDeviceContextState_Release(a);
    if (context1) ID3D11DeviceContext1_Release(context1);
    if (context) ID3D11DeviceContext_Release(context);
    if (device1) ID3D11Device1_Release(device1);
    if (device) ID3D11Device_Release(device);
    printf("wfdx context state: %s\n", result ? "FAIL" : "PASS");
    return result;
}

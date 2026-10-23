/* archs: aarch64 x86_64 */
/* D3D12 smoke test: device, queue, flip swap chain, RTV heap; clear and present 30 frames with
 * a fence wait per frame. Run with GRAPHICS_BACKEND=dxmt (Mullion). */
#define COBJMACROS
#define WIDL_C_INLINE_WRAPPERS
#define INITGUID
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <stdio.h>

int main(void)
{
    ID3D12Device *device = NULL;
    ID3D12CommandQueue *queue = NULL;
    ID3D12CommandAllocator *alloc = NULL;
    ID3D12GraphicsCommandList *list = NULL;
    ID3D12DescriptorHeap *heap = NULL;
    ID3D12Resource *buffers[2] = { 0 };
    ID3D12Fence *fence = NULL;
    IDXGIFactory4 *factory = NULL;
    IDXGISwapChain1 *swap1 = NULL;
    IDXGISwapChain3 *swap = NULL;
    D3D12_COMMAND_QUEUE_DESC qd = { D3D12_COMMAND_LIST_TYPE_DIRECT };
    D3D12_DESCRIPTOR_HEAP_DESC hd = { D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 2 };
    DXGI_SWAP_CHAIN_DESC1 sd = { 0 };
    D3D12_CPU_DESCRIPTOR_HANDLE rtv;
    LUID luid;
    UINT inc, i, frame;
    HANDLE event = CreateEventW( NULL, FALSE, FALSE, NULL );
    HWND hwnd;
    HRESULT hr;

    hwnd = CreateWindowA( "static", "d3d12info", WS_OVERLAPPEDWINDOW | WS_VISIBLE, 100, 100, 320, 240, 0, 0, 0, 0 );
    if (FAILED(hr = D3D12CreateDevice( NULL, D3D_FEATURE_LEVEL_11_0, &IID_ID3D12Device, (void **)&device )))
    {
        printf( "D3D12CreateDevice failed: %#lx\n", hr );
        return 1;
    }
    luid = ID3D12Device_GetAdapterLuid( device );
    printf( "device created, adapter luid %08lx:%08lx, node count %u\n", luid.HighPart, luid.LowPart,
            ID3D12Device_GetNodeCount( device ) );

    hr = ID3D12Device_CreateCommandQueue( device, &qd, &IID_ID3D12CommandQueue, (void **)&queue );
    if (SUCCEEDED(hr)) hr = CreateDXGIFactory1( &IID_IDXGIFactory4, (void **)&factory );
    sd.Width = 320; sd.Height = 240; sd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.SampleDesc.Count = 1; sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT; sd.BufferCount = 2;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    if (SUCCEEDED(hr)) hr = IDXGIFactory4_CreateSwapChainForHwnd( factory, (IUnknown *)queue, hwnd, &sd, NULL, NULL, &swap1 );
    if (SUCCEEDED(hr)) hr = IDXGISwapChain1_QueryInterface( swap1, &IID_IDXGISwapChain3, (void **)&swap );
    if (SUCCEEDED(hr)) hr = ID3D12Device_CreateDescriptorHeap( device, &hd, &IID_ID3D12DescriptorHeap, (void **)&heap );
    if (FAILED(hr)) { printf( "setup failed: %#lx\n", hr ); return 1; }

    inc = ID3D12Device_GetDescriptorHandleIncrementSize( device, D3D12_DESCRIPTOR_HEAP_TYPE_RTV );
    for (i = 0; i < 2 && SUCCEEDED(hr); i++)
    {
        rtv = ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart( heap );
        rtv.ptr += i * inc;
        hr = IDXGISwapChain3_GetBuffer( swap, i, &IID_ID3D12Resource, (void **)&buffers[i] );
        if (SUCCEEDED(hr)) ID3D12Device_CreateRenderTargetView( device, buffers[i], NULL, rtv );
    }
    if (SUCCEEDED(hr)) hr = ID3D12Device_CreateCommandAllocator( device, D3D12_COMMAND_LIST_TYPE_DIRECT, &IID_ID3D12CommandAllocator, (void **)&alloc );
    if (SUCCEEDED(hr)) hr = ID3D12Device_CreateCommandList( device, 0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc, NULL, &IID_ID3D12GraphicsCommandList, (void **)&list );
    if (SUCCEEDED(hr)) hr = ID3D12GraphicsCommandList_Close( list );
    if (SUCCEEDED(hr)) hr = ID3D12Device_CreateFence( device, 0, D3D12_FENCE_FLAG_NONE, &IID_ID3D12Fence, (void **)&fence );
    if (FAILED(hr)) { printf( "resource setup failed: %#lx\n", hr ); return 1; }

    for (frame = 1; frame <= 30 && SUCCEEDED(hr); frame++)
    {
        UINT index = IDXGISwapChain3_GetCurrentBackBufferIndex( swap );
        const float color[4] = { 0.8f, 0.2f * (frame % 2), 0.1f, 1.0f };
        D3D12_RESOURCE_BARRIER barrier = { D3D12_RESOURCE_BARRIER_TYPE_TRANSITION };

        ID3D12CommandAllocator_Reset( alloc );
        ID3D12GraphicsCommandList_Reset( list, alloc, NULL );
        barrier.Transition.pResource = buffers[index];
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
        ID3D12GraphicsCommandList_ResourceBarrier( list, 1, &barrier );
        rtv = ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart( heap );
        rtv.ptr += index * inc;
        ID3D12GraphicsCommandList_ClearRenderTargetView( list, rtv, color, 0, NULL );
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
        ID3D12GraphicsCommandList_ResourceBarrier( list, 1, &barrier );
        ID3D12GraphicsCommandList_Close( list );
        ID3D12CommandQueue_ExecuteCommandLists( queue, 1, (ID3D12CommandList **)&list );
        hr = IDXGISwapChain3_Present( swap, 1, 0 );
        ID3D12CommandQueue_Signal( queue, fence, frame );
        ID3D12Fence_SetEventOnCompletion( fence, frame, event );
        if (WaitForSingleObject( event, 5000 )) { printf( "fence %u timed out\n", frame ); return 1; }
    }
    printf( "30 frames presented: %s (%#lx), fence value %llu\n", SUCCEEDED(hr) ? "ok" : "FAILED", hr,
            (unsigned long long)ID3D12Fence_GetCompletedValue( fence ) );
    return FAILED(hr);
}

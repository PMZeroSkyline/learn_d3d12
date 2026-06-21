#include "d3dx12.h"
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <d3dcompiler.h>
#include <wrl.h>
#include <vector>

#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3dcompiler.lib")

using namespace Microsoft::WRL;

// --- Constants ---
const UINT FrameCount = 2;
const int WindowWidth = 800;
const int WindowHeight = 600;

struct Vertex {
    float position[3];
    float color[4];
};

// --- Globals ---
HWND g_hWnd = nullptr;
ComPtr<ID3D12Device> g_device;
ComPtr<ID3D12CommandQueue> g_commandQueue;
ComPtr<IDXGISwapChain3> g_swapChain;
ComPtr<ID3D12DescriptorHeap> g_rtvHeap; // Holds SwapChain RTVs + Offscreen RTV
ComPtr<ID3D12DescriptorHeap> g_srvHeap; // Holds Offscreen SRV
ComPtr<ID3D12Resource> g_renderTargets[FrameCount];
ComPtr<ID3D12Resource> g_offscreenBuffer;
ComPtr<ID3D12CommandAllocator> g_commandAllocator;
ComPtr<ID3D12GraphicsCommandList> g_commandList;

ComPtr<ID3D12RootSignature> g_rootSig;
ComPtr<ID3D12PipelineState> g_scenePSO;
ComPtr<ID3D12PipelineState> g_postPSO;
ComPtr<ID3D12Resource> g_vertexBuffer;
D3D12_VERTEX_BUFFER_VIEW g_vertexBufferView;

UINT g_rtvDescriptorSize = 0;
UINT g_frameIndex = 0;
ComPtr<ID3D12Fence> g_fence;
HANDLE g_fenceEvent;
UINT64 g_fenceValue = 0;

// --- Shaders (HLSL) ---
const char* g_shaderSource = R"(
struct V2P {
    float4 pos : SV_POSITION;
    float4 col : COLOR;
};

// Pass 1: Simple Scene Shaders
V2P VS_Scene(float3 pos : POSITION, float4 col : COLOR) {
    V2P output;
    output.pos = float4(pos, 1.0f);
    output.col = col;
    return output;
}
float4 PS_Scene(V2P input) : SV_Target {
    return input.col;
}

// Pass 2: Post-process Inversion
Texture2D g_texture : register(t0);
SamplerState g_sampler : register(s0);

struct PostV2P {
    float4 pos : SV_POSITION;
    float2 uv  : TEXCOORD;
};

PostV2P VS_Post(uint vID : SV_VertexID) {
    PostV2P output;
    // Generates a fullscreen triangle
    output.uv = float2((vID << 1) & 2, vID & 2);
    output.pos = float4(output.uv * float2(2.0f, -2.0f) + float2(-1.0f, 1.0f), 0.0f, 1.0f);
    return output;
}

float4 PS_Post(PostV2P input) : SV_Target {
    float4 color = g_texture.Sample(g_sampler, input.uv);
    return float4(1.0 - color.rgb, 1.0); // INVERT COLORS
}
)";

LRESULT CALLBACK WindowProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam) {
    if (message == WM_DESTROY) { PostQuitMessage(0); return 0; }
    return DefWindowProc(hWnd, message, wParam, lParam);
}

void WaitForGPU() {
    g_commandQueue->Signal(g_fence.Get(), ++g_fenceValue);
    if (g_fence->GetCompletedValue() < g_fenceValue) {
        g_fence->SetEventOnCompletion(g_fenceValue, g_fenceEvent);
        WaitForSingleObject(g_fenceEvent, INFINITE);
    }
    g_frameIndex = g_swapChain->GetCurrentBackBufferIndex();
}

void InitD3D() {
    // 1. Device & Factory
    CreateDXGIFactory1(IID_PPV_ARGS(ComPtr<IDXGIFactory4>().GetAddressOf()));
    D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&g_device));

    // 2. Command Queue
    D3D12_COMMAND_QUEUE_DESC queueDesc = { D3D12_COMMAND_LIST_TYPE_DIRECT, 0, D3D12_COMMAND_QUEUE_FLAG_NONE, 0 };
    g_device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&g_commandQueue));

    // 3. Swap Chain
    DXGI_SWAP_CHAIN_DESC1 scDesc = {};
    scDesc.BufferCount = FrameCount;
    scDesc.Width = WindowWidth; scDesc.Height = WindowHeight;
    scDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    scDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    scDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    scDesc.SampleDesc.Count = 1;
    ComPtr<IDXGISwapChain1> sc1;
    ComPtr<IDXGIFactory4> factory;
    CreateDXGIFactory1(IID_PPV_ARGS(&factory));
    factory->CreateSwapChainForHwnd(g_commandQueue.Get(), g_hWnd, &scDesc, nullptr, nullptr, &sc1);
    sc1.As(&g_swapChain);

    // 4. Descriptor Heaps
    // RTV Heap: 2 for Swapchain + 1 for Offscreen Buffer
    D3D12_DESCRIPTOR_HEAP_DESC rtvHeapDesc = { D3D12_DESCRIPTOR_HEAP_TYPE_RTV, FrameCount + 1, D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0 };
    g_device->CreateDescriptorHeap(&rtvHeapDesc, IID_PPV_ARGS(&g_rtvHeap));
    g_rtvDescriptorSize = g_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

    // SRV Heap: 1 for Offscreen Buffer (to read in post-process)
    D3D12_DESCRIPTOR_HEAP_DESC srvHeapDesc = { D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 1, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0 };
    g_device->CreateDescriptorHeap(&srvHeapDesc, IID_PPV_ARGS(&g_srvHeap));

    // 5. Create Resources (RTVs)
    CD3DX12_CPU_DESCRIPTOR_HANDLE rtvHandle(g_rtvHeap->GetCPUDescriptorHandleForHeapStart());
    for (UINT i = 0; i < FrameCount; i++) {
        g_swapChain->GetBuffer(i, IID_PPV_ARGS(&g_renderTargets[i]));
        g_device->CreateRenderTargetView(g_renderTargets[i].Get(), nullptr, rtvHandle);
        rtvHandle.Offset(1, g_rtvDescriptorSize);
    }

    // 6. Create Offscreen Framebuffer Resource
    auto offscreenDesc = CD3DX12_RESOURCE_DESC::Tex2D(DXGI_FORMAT_R8G8B8A8_UNORM, WindowWidth, WindowHeight, 1, 1, 1, 0, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
    auto heapProps = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
    D3D12_CLEAR_VALUE clearVal = { DXGI_FORMAT_R8G8B8A8_UNORM, {0,0,0,1} };
    g_device->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE, &offscreenDesc, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, &clearVal, IID_PPV_ARGS(&g_offscreenBuffer));
    
    // Create RTV for Offscreen
    g_device->CreateRenderTargetView(g_offscreenBuffer.Get(), nullptr, rtvHandle);
    // Create SRV for Offscreen
    g_device->CreateShaderResourceView(g_offscreenBuffer.Get(), nullptr, g_srvHeap->GetCPUDescriptorHandleForHeapStart());

    // 7. Root Signature & PSOs
    CD3DX12_DESCRIPTOR_RANGE1 range;
    range.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0);
    CD3DX12_ROOT_PARAMETER1 rootParam;
    rootParam.InitAsDescriptorTable(1, &range, D3D12_SHADER_VISIBILITY_PIXEL);
    CD3DX12_STATIC_SAMPLER_DESC sampler(0, D3D12_FILTER_MIN_MAG_MIP_POINT);
    
    CD3DX12_VERSIONED_ROOT_SIGNATURE_DESC rsDesc;
    rsDesc.Init_1_1(1, &rootParam, 1, &sampler, D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT);
    
    ComPtr<ID3DBlob> rsBlob, errorBlob;
    D3DX12SerializeVersionedRootSignature(&rsDesc, D3D_ROOT_SIGNATURE_VERSION_1_1, &rsBlob, &errorBlob);
    g_device->CreateRootSignature(0, rsBlob->GetBufferPointer(), rsBlob->GetBufferSize(), IID_PPV_ARGS(&g_rootSig));

    // Compile Shaders & Create PSOs
    ComPtr<ID3DBlob> vsScene, psScene, vsPost, psPost;
    D3DCompile(g_shaderSource, strlen(g_shaderSource), nullptr, nullptr, nullptr, "VS_Scene", "vs_5_0", 0, 0, &vsScene, nullptr);
    D3DCompile(g_shaderSource, strlen(g_shaderSource), nullptr, nullptr, nullptr, "PS_Scene", "ps_5_0", 0, 0, &psScene, nullptr);
    D3DCompile(g_shaderSource, strlen(g_shaderSource), nullptr, nullptr, nullptr, "VS_Post", "vs_5_0", 0, 0, &vsPost, nullptr);
    D3DCompile(g_shaderSource, strlen(g_shaderSource), nullptr, nullptr, nullptr, "PS_Post", "ps_5_0", 0, 0, &psPost, nullptr);

    D3D12_INPUT_ELEMENT_DESC inputLayout[] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 }
    };

    D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc = {};
    psoDesc.InputLayout = { inputLayout, _countof(inputLayout) };
    psoDesc.pRootSignature = g_rootSig.Get();
    psoDesc.VS = CD3DX12_SHADER_BYTECODE(vsScene.Get());
    psoDesc.PS = CD3DX12_SHADER_BYTECODE(psScene.Get());
    psoDesc.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
    psoDesc.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
    psoDesc.DepthStencilState.DepthEnable = FALSE; psoDesc.DepthStencilState.StencilEnable = FALSE;
    psoDesc.SampleMask = UINT_MAX;
    psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    psoDesc.NumRenderTargets = 1;
    psoDesc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    psoDesc.SampleDesc.Count = 1;
    g_device->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&g_scenePSO));

    // Post PSO
    psoDesc.InputLayout = { nullptr, 0 };
    psoDesc.VS = CD3DX12_SHADER_BYTECODE(vsPost.Get());
    psoDesc.PS = CD3DX12_SHADER_BYTECODE(psPost.Get());
    g_device->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&g_postPSO));

    // 8. Geometry (Triangle)
    Vertex verts[] = {
        {{-0.5f, -0.5f, 0}, {1, 0, 0, 1}}, // Red
        {{ 0.0f,  0.5f, 0}, {0, 1, 0, 1}}, // Green
        {{ 0.5f, -0.5f, 0}, {0, 0, 1, 1}}  // Blue
    };
    g_device->CreateCommittedResource(&CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD), D3D12_HEAP_FLAG_NONE, &CD3DX12_RESOURCE_DESC::Buffer(sizeof(verts)), D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&g_vertexBuffer));
    void* pData; g_vertexBuffer->Map(0, nullptr, &pData); memcpy(pData, verts, sizeof(verts)); g_vertexBuffer->Unmap(0, nullptr);
    g_vertexBufferView = { g_vertexBuffer->GetGPUVirtualAddress(), sizeof(verts), sizeof(Vertex) };

    g_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&g_commandAllocator));
    g_device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g_commandAllocator.Get(), nullptr, IID_PPV_ARGS(&g_commandList));
    g_commandList->Close();
    g_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g_fence));
    g_fenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
}

void Render() {
    g_commandAllocator->Reset();
    g_commandList->Reset(g_commandAllocator.Get(), g_scenePSO.Get());

    CD3DX12_VIEWPORT vp(0.0f, 0.0f, (float)WindowWidth, (float)WindowHeight);
    CD3DX12_RECT scissor(0, 0, WindowWidth, WindowHeight);
    g_commandList->RSSetViewports(1, &vp);
    g_commandList->RSSetScissorRects(1, &scissor);

    // --- PASS 1: Render Objects to Offscreen Framebuffer ---
    auto barrier1 = CD3DX12_RESOURCE_BARRIER::Transition(g_offscreenBuffer.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
    g_commandList->ResourceBarrier(1, &barrier1);

    CD3DX12_CPU_DESCRIPTOR_HANDLE offscreenRtv(g_rtvHeap->GetCPUDescriptorHandleForHeapStart(), FrameCount, g_rtvDescriptorSize);
    float clearCol[] = { 0.1f, 0.1f, 0.1f, 1.0f };
    g_commandList->ClearRenderTargetView(offscreenRtv, clearCol, 0, nullptr);
    g_commandList->OMSetRenderTargets(1, &offscreenRtv, FALSE, nullptr);
    
    g_commandList->SetGraphicsRootSignature(g_rootSig.Get());
    g_commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    g_commandList->IASetVertexBuffers(0, 1, &g_vertexBufferView);
    g_commandList->DrawInstanced(3, 1, 0, 0);

    // --- PASS 2: Post-process (Invert) to SwapChain ---
    auto barrier2 = CD3DX12_RESOURCE_BARRIER::Transition(g_offscreenBuffer.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    auto barrier3 = CD3DX12_RESOURCE_BARRIER::Transition(g_renderTargets[g_frameIndex].Get(), D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);
    D3D12_RESOURCE_BARRIER barriers[] = { barrier2, barrier3 };
    g_commandList->ResourceBarrier(2, barriers);

    CD3DX12_CPU_DESCRIPTOR_HANDLE swapRtv(g_rtvHeap->GetCPUDescriptorHandleForHeapStart(), g_frameIndex, g_rtvDescriptorSize);
    g_commandList->OMSetRenderTargets(1, &swapRtv, FALSE, nullptr);
    g_commandList->SetPipelineState(g_postPSO.Get());
    
    ID3D12DescriptorHeap* heaps[] = { g_srvHeap.Get() };
    g_commandList->SetDescriptorHeaps(1, heaps);
    g_commandList->SetGraphicsRootDescriptorTable(0, g_srvHeap->GetGPUDescriptorHandleForHeapStart());
    
    g_commandList->DrawInstanced(3, 1, 0, 0); // Draws fullscreen triangle

    auto barrier4 = CD3DX12_RESOURCE_BARRIER::Transition(g_renderTargets[g_frameIndex].Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);
    g_commandList->ResourceBarrier(1, &barrier4);

    g_commandList->Close();
    ID3D12CommandList* lists[] = { g_commandList.Get() };
    g_commandQueue->ExecuteCommandLists(1, lists);
    g_swapChain->Present(1, 0);
    WaitForGPU();
}

int main(int argc, char** argv) {
    HINSTANCE hInstance = GetModuleHandle(NULL);
    WNDCLASSEX wc = { sizeof(WNDCLASSEX), CS_HREDRAW | CS_VREDRAW, WindowProc, 0, 0, hInstance, NULL, NULL, NULL, NULL, L"D3D12FB", NULL };
    RegisterClassEx(&wc);
    g_hWnd = CreateWindow(wc.lpszClassName, L"D3D12: Scene -> Offscreen FB -> Invert -> Screen", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, WindowWidth, WindowHeight, NULL, NULL, hInstance, NULL);
    ShowWindow(g_hWnd, SW_SHOW);

    InitD3D();

    MSG msg = {};
    while (msg.message != WM_QUIT) {
        if (PeekMessage(&msg, NULL, 0, 0, PM_REMOVE)) { TranslateMessage(&msg); DispatchMessage(&msg); }
        else { Render(); }
    }
    return 0;
}
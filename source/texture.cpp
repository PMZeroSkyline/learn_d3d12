#include <windows.h>
#include <d3d12.h>
#include "d3dx12.h"
#include <dxgi1_6.h>
#include <d3dcompiler.h>
#include <DirectXMath.h>
#include <wrl.h>
#include <vector>
#include <stdexcept>

#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3dcompiler.lib")

using namespace DirectX;
using Microsoft::WRL::ComPtr;

// --- Configuration ---
const UINT FrameCount = 2;
const UINT ObjectCount = 2;
const UINT TextureSize = 256;

struct Vertex {
    XMFLOAT3 position;
    XMFLOAT2 uv;
};

// --- Globals ---
HWND g_hwnd = nullptr;
ComPtr<ID3D12Device> g_device;
ComPtr<ID3D12CommandQueue> g_commandQueue;
ComPtr<IDXGISwapChain3> g_swapChain;
ComPtr<ID3D12DescriptorHeap> g_rtvHeap;
ComPtr<ID3D12DescriptorHeap> g_srvHeap;
ComPtr<ID3D12Resource> g_renderTargets[FrameCount];
ComPtr<ID3D12CommandAllocator> g_commandAllocator;
ComPtr<ID3D12GraphicsCommandList> g_commandList;
ComPtr<ID3D12RootSignature> g_rootSignature;
ComPtr<ID3D12PipelineState> g_pipelineState;
ComPtr<ID3D12Resource> g_vertexBuffer;
D3D12_VERTEX_BUFFER_VIEW g_vertexBufferView;

ComPtr<ID3D12Resource> g_textures[ObjectCount];
std::vector<ComPtr<ID3D12Resource>> g_uploadHeaps; // Keep upload heaps alive during init

UINT g_rtvDescriptorSize;
UINT g_srvDescriptorSize;
UINT g_frameIndex;

// Synchronization objects
HANDLE g_fenceEvent;
ComPtr<ID3D12Fence> g_fence;
UINT64 g_fenceValue;

void ThrowIfFailed(HRESULT hr) {
    if (FAILED(hr)) throw std::runtime_error("DirectX Error");
}

void WaitForGpu() {
    ThrowIfFailed(g_commandQueue->Signal(g_fence.Get(), g_fenceValue));
    if (g_fence->GetCompletedValue() < g_fenceValue) {
        ThrowIfFailed(g_fence->SetEventOnCompletion(g_fenceValue, g_fenceEvent));
        WaitForSingleObject(g_fenceEvent, INFINITE);
    }
    g_fenceValue++;
}

// Generate procedural checkerboard data
std::vector<uint32_t> GenerateCheckerboardData(uint32_t color1, uint32_t color2) {
    std::vector<uint32_t> data(TextureSize * TextureSize);
    const uint32_t cellSize = 32;
    for (uint32_t y = 0; y < TextureSize; y++) {
        for (uint32_t x = 0; x < TextureSize; x++) {
            bool isColor1 = ((x / cellSize) % 2) == ((y / cellSize) % 2);
            data[y * TextureSize + x] = isColor1 ? color1 : color2;
        }
    }
    return data;
}

void InitD3D(HWND hwnd) {
#if defined(_DEBUG)
    ComPtr<ID3D12Debug> debugController;
    if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debugController)))) {
        debugController->EnableDebugLayer();
    }
#endif

    ComPtr<IDXGIFactory4> factory;
    ThrowIfFailed(CreateDXGIFactory1(IID_PPV_ARGS(&factory)));
    ThrowIfFailed(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&g_device)));

    // Create Command Queue
    D3D12_COMMAND_QUEUE_DESC queueDesc = {};
    queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    ThrowIfFailed(g_device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&g_commandQueue)));

    // Create Swap Chain
    DXGI_SWAP_CHAIN_DESC1 swapChainDesc = {};
    swapChainDesc.BufferCount = FrameCount;
    swapChainDesc.Width = 800;
    swapChainDesc.Height = 600;
    swapChainDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    swapChainDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    swapChainDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    swapChainDesc.SampleDesc.Count = 1;

    ComPtr<IDXGISwapChain1> swapChain;
    ThrowIfFailed(factory->CreateSwapChainForHwnd(g_commandQueue.Get(), hwnd, &swapChainDesc, nullptr, nullptr, &swapChain));
    swapChain.As(&g_swapChain);
    g_frameIndex = g_swapChain->GetCurrentBackBufferIndex();

    // Create Descriptor Heaps
    D3D12_DESCRIPTOR_HEAP_DESC rtvHeapDesc = {};
    rtvHeapDesc.NumDescriptors = FrameCount;
    rtvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    ThrowIfFailed(g_device->CreateDescriptorHeap(&rtvHeapDesc, IID_PPV_ARGS(&g_rtvHeap)));
    g_rtvDescriptorSize = g_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

    // SRV Heap to hold textures for all objects
    D3D12_DESCRIPTOR_HEAP_DESC srvHeapDesc = {};
    srvHeapDesc.NumDescriptors = ObjectCount;
    srvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    srvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    ThrowIfFailed(g_device->CreateDescriptorHeap(&srvHeapDesc, IID_PPV_ARGS(&g_srvHeap)));
    g_srvDescriptorSize = g_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

    // Create Render Target Views (RTV)
    CD3DX12_CPU_DESCRIPTOR_HANDLE rtvHandle(g_rtvHeap->GetCPUDescriptorHandleForHeapStart());
    for (UINT n = 0; n < FrameCount; n++) {
        ThrowIfFailed(g_swapChain->GetBuffer(n, IID_PPV_ARGS(&g_renderTargets[n])));
        g_device->CreateRenderTargetView(g_renderTargets[n].Get(), nullptr, rtvHandle);
        rtvHandle.Offset(1, g_rtvDescriptorSize);
    }

    ThrowIfFailed(g_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&g_commandAllocator)));

    // --- Root Signature ---
    CD3DX12_DESCRIPTOR_RANGE1 ranges[1];
    ranges[0].Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0); // t0

    CD3DX12_ROOT_PARAMETER1 rootParameters[2];
    rootParameters[0].InitAsConstants(16, 0, 0, D3D12_SHADER_VISIBILITY_VERTEX); // b0: Transformation Matrix
    rootParameters[1].InitAsDescriptorTable(1, &ranges[0], D3D12_SHADER_VISIBILITY_PIXEL); // t0: Texture

    CD3DX12_STATIC_SAMPLER_DESC sampler(0, D3D12_FILTER_MIN_MAG_MIP_LINEAR);

    CD3DX12_VERSIONED_ROOT_SIGNATURE_DESC rootSigDesc;
    rootSigDesc.Init_1_1(_countof(rootParameters), rootParameters, 1, &sampler, D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT);

    ComPtr<ID3DBlob> signature;
    ComPtr<ID3DBlob> error;
    HRESULT hr = D3DX12SerializeVersionedRootSignature(&rootSigDesc, D3D_ROOT_SIGNATURE_VERSION_1_1, &signature, &error);
    if (FAILED(hr)) {
        if (error) OutputDebugStringA((char*)error->GetBufferPointer());
        ThrowIfFailed(hr);
    }
    ThrowIfFailed(g_device->CreateRootSignature(0, signature->GetBufferPointer(), signature->GetBufferSize(), IID_PPV_ARGS(&g_rootSignature)));

    // --- Shader Compilation ---
    const char* shaderCode = R"(
        struct VSInput { float3 pos : POSITION; float2 uv : TEXCOORD; };
        struct PSInput { float4 pos : SV_POSITION; float2 uv : TEXCOORD; };
        cbuffer RootConstants : register(b0) { float4x4 model; };
        Texture2D tex : register(t0);
        SamplerState samp : register(s0);

        PSInput VSMain(VSInput input) {
            PSInput result;
            result.pos = mul(float4(input.pos, 1.0f), model);
            result.uv = input.uv;
            return result;
        }
        float4 PSMain(PSInput input) : SV_TARGET {
            return tex.Sample(samp, input.uv);
        }
    )";

    ComPtr<ID3DBlob> vs, ps;
    ThrowIfFailed(D3DCompile(shaderCode, strlen(shaderCode), nullptr, nullptr, nullptr, "VSMain", "vs_5_0", 0, 0, &vs, nullptr));
    ThrowIfFailed(D3DCompile(shaderCode, strlen(shaderCode), nullptr, nullptr, nullptr, "PSMain", "ps_5_0", 0, 0, &ps, nullptr));

    D3D12_INPUT_ELEMENT_DESC inputLayout[] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 }
    };

    // --- Create PSO ---
    D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc = {};
    psoDesc.InputLayout = { inputLayout, _countof(inputLayout) };
    psoDesc.pRootSignature = g_rootSignature.Get();
    psoDesc.VS = CD3DX12_SHADER_BYTECODE(vs.Get());
    psoDesc.PS = CD3DX12_SHADER_BYTECODE(ps.Get());
    psoDesc.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
    psoDesc.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
    psoDesc.DepthStencilState.DepthEnable = FALSE;
    psoDesc.DepthStencilState.StencilEnable = FALSE;
    psoDesc.SampleMask = UINT_MAX;
    psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    psoDesc.NumRenderTargets = 1;
    psoDesc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    psoDesc.SampleDesc.Count = 1;
    ThrowIfFailed(g_device->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&g_pipelineState)));

    ThrowIfFailed(g_device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g_commandAllocator.Get(), g_pipelineState.Get(), IID_PPV_ARGS(&g_commandList)));

    // --- Create Vertex Buffer ---
    Vertex vertices[] = {
        { {-0.3f, -0.4f, 0.0f}, {0.0f, 1.0f} },
        { {-0.3f,  0.4f, 0.0f}, {0.0f, 0.0f} },
        { { 0.3f, -0.4f, 0.0f}, {1.0f, 1.0f} },
        { { 0.3f,  0.4f, 0.0f}, {1.0f, 0.0f} }
    };

    ThrowIfFailed(g_device->CreateCommittedResource(
        &CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD),
        D3D12_HEAP_FLAG_NONE,
        &CD3DX12_RESOURCE_DESC::Buffer(sizeof(vertices)),
        D3D12_RESOURCE_STATE_GENERIC_READ,
        nullptr,
        IID_PPV_ARGS(&g_vertexBuffer)));

    UINT8* pVertexData;
    g_vertexBuffer->Map(0, nullptr, reinterpret_cast<void**>(&pVertexData));
    memcpy(pVertexData, vertices, sizeof(vertices));
    g_vertexBuffer->Unmap(0, nullptr);

    g_vertexBufferView.BufferLocation = g_vertexBuffer->GetGPUVirtualAddress();
    g_vertexBufferView.StrideInBytes = sizeof(Vertex);
    g_vertexBufferView.SizeInBytes = sizeof(vertices);

    // --- Create Checkerboard Textures ---
    uint32_t patterns[ObjectCount][2] = {
        { 0xFF0000FF, 0xFFFFFFFF }, // Red & White
        { 0xFFFF0000, 0xFF00FFFF }  // Blue & Yellow
    };

    CD3DX12_CPU_DESCRIPTOR_HANDLE srvHandle(g_srvHeap->GetCPUDescriptorHandleForHeapStart());

    for (int i = 0; i < ObjectCount; i++) {
        auto texData = GenerateCheckerboardData(patterns[i][0], patterns[i][1]);
        auto desc = CD3DX12_RESOURCE_DESC::Tex2D(DXGI_FORMAT_R8G8B8A8_UNORM, TextureSize, TextureSize, 1, 1);
        
        ThrowIfFailed(g_device->CreateCommittedResource(
            &CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT),
            D3D12_HEAP_FLAG_NONE,
            &desc,
            D3D12_RESOURCE_STATE_COPY_DEST,
            nullptr,
            IID_PPV_ARGS(&g_textures[i])));

        const UINT64 uploadSize = GetRequiredIntermediateSize(g_textures[i].Get(), 0, 1);
        ComPtr<ID3D12Resource> uploadHeap;
        ThrowIfFailed(g_device->CreateCommittedResource(
            &CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD),
            D3D12_HEAP_FLAG_NONE,
            &CD3DX12_RESOURCE_DESC::Buffer(uploadSize),
            D3D12_RESOURCE_STATE_GENERIC_READ,
            nullptr,
            IID_PPV_ARGS(&uploadHeap)));

        D3D12_SUBRESOURCE_DATA subData = {};
        subData.pData = texData.data();
        subData.RowPitch = TextureSize * 4;
        subData.SlicePitch = subData.RowPitch * TextureSize;

        UpdateSubresources(g_commandList.Get(), g_textures[i].Get(), uploadHeap.Get(), 0, 0, 1, &subData);
        g_commandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(g_textures[i].Get(), 
            D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE));
        
        g_device->CreateShaderResourceView(g_textures[i].Get(), nullptr, srvHandle);
        srvHandle.Offset(1, g_srvDescriptorSize);
        g_uploadHeaps.push_back(uploadHeap);
    }

    g_commandList->Close();
    ID3D12CommandList* lists[] = { g_commandList.Get() };
    g_commandQueue->ExecuteCommandLists(1, lists);

    ThrowIfFailed(g_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g_fence)));
    g_fenceValue = 1;
    g_fenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
    WaitForGpu();
    
    // Clear upload heaps after GPU is done
    g_uploadHeaps.clear();
}

void Render() {
    g_commandAllocator->Reset();
    g_commandList->Reset(g_commandAllocator.Get(), g_pipelineState.Get());

    g_commandList->SetGraphicsRootSignature(g_rootSignature.Get());
    ID3D12DescriptorHeap* heaps[] = { g_srvHeap.Get() };
    g_commandList->SetDescriptorHeaps(1, heaps);

    // Transition Backbuffer
    g_commandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(g_renderTargets[g_frameIndex].Get(), 
        D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET));

    CD3DX12_CPU_DESCRIPTOR_HANDLE rtvHandle(g_rtvHeap->GetCPUDescriptorHandleForHeapStart(), g_frameIndex, g_rtvDescriptorSize);
    g_commandList->OMSetRenderTargets(1, &rtvHandle, FALSE, nullptr);
    
    const float clearColor[] = { 0.15f, 0.15f, 0.15f, 1.0f };
    g_commandList->ClearRenderTargetView(rtvHandle, clearColor, 0, nullptr);

    g_commandList->RSSetViewports(1, &CD3DX12_VIEWPORT(0.0f, 0.0f, 800.0f, 600.0f));
    g_commandList->RSSetScissorRects(1, &CD3DX12_RECT(0, 0, 800, 600));

    g_commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
    g_commandList->IASetVertexBuffers(0, 1, &g_vertexBufferView);

    // Draw Object 1 (Left, Red Pattern)
    XMMATRIX m1 = XMMatrixTranslation(-0.45f, 0.0f, 0.0f);
    g_commandList->SetGraphicsRoot32BitConstants(0, 16, &m1, 0);
    g_commandList->SetGraphicsRootDescriptorTable(1, CD3DX12_GPU_DESCRIPTOR_HANDLE(g_srvHeap->GetGPUDescriptorHandleForHeapStart(), 0, g_srvDescriptorSize));
    g_commandList->DrawInstanced(4, 1, 0, 0);

    // Draw Object 2 (Right, Blue Pattern)
    XMMATRIX m2 = XMMatrixTranslation(0.45f, 0.0f, 0.0f);
    g_commandList->SetGraphicsRoot32BitConstants(0, 16, &m2, 0);
    g_commandList->SetGraphicsRootDescriptorTable(1, CD3DX12_GPU_DESCRIPTOR_HANDLE(g_srvHeap->GetGPUDescriptorHandleForHeapStart(), 1, g_srvDescriptorSize));
    g_commandList->DrawInstanced(4, 1, 0, 0);

    // Presentation transition
    g_commandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(g_renderTargets[g_frameIndex].Get(), 
        D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT));

    g_commandList->Close();
    ID3D12CommandList* lists[] = { g_commandList.Get() };
    g_commandQueue->ExecuteCommandLists(1, lists);
    
    g_swapChain->Present(1, 0);
    WaitForGpu();
    g_frameIndex = g_swapChain->GetCurrentBackBufferIndex();
}

LRESULT CALLBACK WindowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    if (message == WM_DESTROY) { PostQuitMessage(0); return 0; }
    return DefWindowProc(hwnd, message, wParam, lParam);
}

int main(int argc, char** argv) {
    HINSTANCE hI = GetModuleHandle(NULL);
    
    WNDCLASSEX windowClass = { sizeof(WNDCLASSEX), CS_HREDRAW | CS_VREDRAW, WindowProc, 0, 0, hI, 0, 0, 0, 0, L"D3D12Sample", 0 };
    RegisterClassEx(&windowClass);
    g_hwnd = CreateWindow(L"D3D12Sample", L"D3D12 Multiple Checkerboard Textures", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 800, 600, nullptr, nullptr, hI, nullptr);

    try {
        InitD3D(g_hwnd);
        ShowWindow(g_hwnd, SW_SHOW);

        MSG msg = {};
        while (msg.message != WM_QUIT) {
            if (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE)) {
                TranslateMessage(&msg);
                DispatchMessage(&msg);
            } else {
                Render();
            }
        }
    }
    catch (const std::exception& e) {
        MessageBoxA(NULL, e.what(), "Error", MB_OK);
    }

    return 0;
}
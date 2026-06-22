#include "d3dx12.h"
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <iostream>
#include <vector>

#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3dcompiler.lib")

using Microsoft::WRL::ComPtr;

// Window settings
const UINT WindowWidth = 1280;
const UINT WindowHeight = 720;
const UINT FrameCount = 2;

// Struct definitions
struct Vertex {
    float position[3];
    float uv[2];
};

struct BindlessIndices {
    uint32_t vertexBufferIndex;
    uint32_t textureIndex;
};

// Global Direct3D 12 Variables
ComPtr<IDXGIFactory4> g_Factory;
ComPtr<ID3D12Device> g_Device;
ComPtr<ID3D12CommandQueue> g_CommandQueue;
ComPtr<IDXGISwapChain3> g_SwapChain;
ComPtr<ID3D12DescriptorHeap> g_RtvHeap;
ComPtr<ID3D12DescriptorHeap> g_SrvHeap;
UINT g_RtvDescriptorSize = 0;
UINT g_SrvDescriptorSize = 0;
ComPtr<ID3D12Resource> g_RenderTargets[FrameCount];
ComPtr<ID3D12CommandAllocator> g_CommandAllocators[FrameCount];
ComPtr<ID3D12GraphicsCommandList> g_CommandList;
ComPtr<ID3D12RootSignature> g_RootSignature;
ComPtr<ID3D12PipelineState> g_PipelineState;

// Resource handles
ComPtr<ID3D12Resource> g_VertexBuffer;
ComPtr<ID3D12Resource> g_Texture;
ComPtr<ID3D12Resource> g_UploadBuffer;

// Synchronization objects
UINT g_FrameIndex = 0;
HANDLE g_FenceEvent;
ComPtr<ID3D12Fence> g_Fence;
UINT64 g_FenceValues[FrameCount] = { 0, 0 };

// HLSL Shaders with bounded arrays for SM 5.1 compatibility
const char* g_ShaderSource = R"(
struct VSInput {
    uint vertexID : SV_VertexID;
};

struct PSInput {
    float4 position : SV_POSITION;
    float2 uv : TEXCOORD;
};

// Traditional constant buffer structure compatible with legacy compilers
cbuffer BindlessIndices : register(b0)
{
    uint g_vertexBufferIdx;
    uint g_textureIdx;
};

// Bounded descriptor arrays of size 1000 to satisfy the compiler and diagnostic tools
ByteAddressBuffer g_ByteAddressBuffers[1000] : register(t0, space0);
Texture2D         g_Textures2D[1000]         : register(t0, space1);
SamplerState      g_Sampler                  : register(s0);

PSInput VSMain(VSInput input) {
    PSInput result;
    
    // Each vertex consists of 5 floats (3 position, 2 UV) = 20 bytes
    uint offset = input.vertexID * 20;
    
    // Direct array indexing
    float3 pos = asfloat(g_ByteAddressBuffers[g_vertexBufferIdx].Load3(offset));
    float2 uv = asfloat(g_ByteAddressBuffers[g_vertexBufferIdx].Load2(offset + 12));
    
    result.position = float4(pos, 1.0f);
    result.uv = uv;
    return result;
}

float4 PSMain(PSInput input) : SV_TARGET {
    // Direct array indexing compatible with SM 5.1 compilation
    return g_Textures2D[g_textureIdx].Sample(g_Sampler, input.uv);
}
)";

// Forward declarations
LRESULT CALLBACK WndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam);
void WaitForGpu();
void MoveToNextFrame();

int main(int argc, char** argv)
{
    HINSTANCE hI = GetModuleHandle(NULL);

    // Initialize Win32 Window
    WNDCLASSEXW wcex = {};
    wcex.cbSize = sizeof(WNDCLASSEXW);
    wcex.style = CS_HREDRAW | CS_VREDRAW;
    wcex.lpfnWndProc = WndProc;
    wcex.hInstance = hI;
    wcex.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wcex.lpszClassName = L"BindlessSampleWindowClass";
    RegisterClassExW(&wcex);

    RECT rc = { 0, 0, static_cast<LONG>(WindowWidth), static_cast<LONG>(WindowHeight) };
    AdjustWindowRect(&rc, WS_OVERLAPPEDWINDOW, FALSE);

    HWND hWnd = CreateWindowW(
        L"BindlessSampleWindowClass",
        L"D3D12 Bindless Resources Sample",
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT,
        rc.right - rc.left, rc.bottom - rc.top,
        nullptr, nullptr, hI, nullptr
    );

    if (!hWnd)
    {
        return -1;
    }

    ShowWindow(hWnd, SW_SHOW);

    // Initialize Direct3D 12
    UINT dxgiFactoryFlags = 0;
#if defined(_DEBUG)
    ComPtr<ID3D12Debug> debugController;
    if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debugController))))
    {
        debugController->EnableDebugLayer();
        dxgiFactoryFlags |= DXGI_CREATE_FACTORY_DEBUG;
    }
#endif

    CreateDXGIFactory2(dxgiFactoryFlags, IID_PPV_ARGS(&g_Factory));
    
    // Select compatible adapter and create device
    ComPtr<IDXGIAdapter1> adapter;
    for (UINT adapterIndex = 0; DXGI_ERROR_NOT_FOUND != g_Factory->EnumAdapters1(adapterIndex, &adapter); ++adapterIndex)
    {
        DXGI_ADAPTER_DESC1 desc;
        adapter->GetDesc1(&desc);
        if (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) continue;
        if (SUCCEEDED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&g_Device)))) break;
    }

    // Command Queue Setup
    D3D12_COMMAND_QUEUE_DESC queueDesc = {};
    queueDesc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
    queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    g_Device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&g_CommandQueue));

    // Swap Chain Setup
    DXGI_SWAP_CHAIN_DESC1 swapChainDesc = {};
    swapChainDesc.BufferCount = FrameCount;
    swapChainDesc.Width = WindowWidth;
    swapChainDesc.Height = WindowHeight;
    swapChainDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    swapChainDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    swapChainDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    swapChainDesc.SampleDesc.Count = 1;

    ComPtr<IDXGISwapChain1> swapChain;
    g_Factory->CreateSwapChainForHwnd(g_CommandQueue.Get(), hWnd, &swapChainDesc, nullptr, nullptr, &swapChain);
    swapChain.As(&g_SwapChain);
    g_FrameIndex = g_SwapChain->GetCurrentBackBufferIndex();

    // Descriptor Heaps Setup
    // RTV Descriptor Heap
    D3D12_DESCRIPTOR_HEAP_DESC rtvHeapDesc = {};
    rtvHeapDesc.NumDescriptors = FrameCount;
    rtvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    rtvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    g_Device->CreateDescriptorHeap(&rtvHeapDesc, IID_PPV_ARGS(&g_RtvHeap));
    g_RtvDescriptorSize = g_Device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

    // CBV_SRV_UAV Descriptor Heap (Shader Visible for Bindless lookup)
    const UINT MaxBindlessDescriptors = 2000;
    D3D12_DESCRIPTOR_HEAP_DESC srvHeapDesc = {};
    srvHeapDesc.NumDescriptors = MaxBindlessDescriptors;
    srvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    srvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    g_Device->CreateDescriptorHeap(&srvHeapDesc, IID_PPV_ARGS(&g_SrvHeap));
    g_SrvDescriptorSize = g_Device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

    // Create Frame Resources (RTVs)
    CD3DX12_CPU_DESCRIPTOR_HANDLE rtvHandle(g_RtvHeap->GetCPUDescriptorHandleForHeapStart());
    for (UINT n = 0; n < FrameCount; n++)
    {
        g_SwapChain->GetBuffer(n, IID_PPV_ARGS(&g_RenderTargets[n]));
        g_Device->CreateRenderTargetView(g_RenderTargets[n].Get(), nullptr, rtvHandle);
        rtvHandle.Offset(1, g_RtvDescriptorSize);
        g_Device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&g_CommandAllocators[n]));
    }

    // Root Signature Construction
    CD3DX12_DESCRIPTOR_RANGE1 ranges[2];
    ranges[0].Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1000, 0, 0, D3D12_DESCRIPTOR_RANGE_FLAG_DESCRIPTORS_VOLATILE); 
    ranges[1].Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1000, 0, 1, D3D12_DESCRIPTOR_RANGE_FLAG_DESCRIPTORS_VOLATILE); 

    CD3DX12_ROOT_PARAMETER1 rootParameters[2];
    rootParameters[0].InitAsConstants(2, 0, 0, D3D12_SHADER_VISIBILITY_ALL); // b0, space0
    rootParameters[1].InitAsDescriptorTable(2, ranges, D3D12_SHADER_VISIBILITY_ALL);

    CD3DX12_STATIC_SAMPLER_DESC staticSampler(0, D3D12_FILTER_MIN_MAG_MIP_POINT);

    CD3DX12_VERSIONED_ROOT_SIGNATURE_DESC rootSigDesc;
    rootSigDesc.Init_1_1(_countof(rootParameters), rootParameters, 1, &staticSampler, D3D12_ROOT_SIGNATURE_FLAG_NONE);

    ComPtr<ID3DBlob> signature;
    ComPtr<ID3DBlob> error;
    D3D12SerializeVersionedRootSignature(&rootSigDesc, &signature, &error);
    g_Device->CreateRootSignature(0, signature->GetBufferPointer(), signature->GetBufferSize(), IID_PPV_ARGS(&g_RootSignature));

    // Compile Shaders with error diagnostic handling
    ComPtr<ID3DBlob> vertexShader;
    ComPtr<ID3DBlob> vsError;
    HRESULT hrVS = D3DCompile(g_ShaderSource, strlen(g_ShaderSource), nullptr, nullptr, nullptr, "VSMain", "vs_5_1", 0, 0, &vertexShader, &vsError);
    if (FAILED(hrVS))
    {
        if (vsError)
        {
            OutputDebugStringA(static_cast<const char*>(vsError->GetBufferPointer()));
            std::cerr << "Vertex Shader Compilation Error:\n" << static_cast<const char*>(vsError->GetBufferPointer()) << std::endl;
        }
        return -1;
    }

    ComPtr<ID3DBlob> pixelShader;
    ComPtr<ID3DBlob> psError;
    HRESULT hrPS = D3DCompile(g_ShaderSource, strlen(g_ShaderSource), nullptr, nullptr, nullptr, "PSMain", "ps_5_1", 0, 0, &pixelShader, &psError);
    if (FAILED(hrPS))
    {
        if (psError)
        {
            OutputDebugStringA(static_cast<const char*>(psError->GetBufferPointer()));
            std::cerr << "Pixel Shader Compilation Error:\n" << static_cast<const char*>(psError->GetBufferPointer()) << std::endl;
        }
        return -1;
    }

    // Create Graphics Pipeline State (Empty Input Layout due to manual vertex retrieval)
    D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc = {};
    psoDesc.InputLayout = { nullptr, 0 };
    psoDesc.pRootSignature = g_RootSignature.Get();
    psoDesc.VS = CD3DX12_SHADER_BYTECODE(vertexShader.Get());
    psoDesc.PS = CD3DX12_SHADER_BYTECODE(pixelShader.Get());
    psoDesc.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
    psoDesc.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
    psoDesc.DepthStencilState.DepthEnable = FALSE;
    psoDesc.DepthStencilState.StencilEnable = FALSE;
    psoDesc.SampleMask = UINT_MAX;
    psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    psoDesc.NumRenderTargets = 1;
    psoDesc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    psoDesc.SampleDesc.Count = 1;
    g_Device->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&g_PipelineState));

    // Create temporary Command List for setup and data uploads
    g_Device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g_CommandAllocators[g_FrameIndex].Get(), nullptr, IID_PPV_ARGS(&g_CommandList));

    // Define Vertex Data
    Vertex vertices[] = {
        { {  0.0f,  0.5f, 0.0f }, { 0.5f, 0.0f } },
        { {  0.5f, -0.5f, 0.0f }, { 1.0f, 1.0f } },
        { { -0.5f, -0.5f, 0.0f }, { 0.0f, 1.0f } }
    };
    const UINT vertexBufferSize = sizeof(vertices);

    // Allocate Vertex Buffer in an upload-compatible state
    CD3DX12_HEAP_PROPERTIES uploadHeapProps(D3D12_HEAP_TYPE_UPLOAD);
    CD3DX12_RESOURCE_DESC bufferDesc = CD3DX12_RESOURCE_DESC::Buffer(vertexBufferSize);
    g_Device->CreateCommittedResource(
        &uploadHeapProps,
        D3D12_HEAP_FLAG_NONE,
        &bufferDesc,
        D3D12_RESOURCE_STATE_GENERIC_READ,
        nullptr,
        IID_PPV_ARGS(&g_VertexBuffer)
    );

    // Copy vertex data to CPU-mapped pointer
    void* pVertexDataBegin = nullptr;
    g_VertexBuffer->Map(0, nullptr, &pVertexDataBegin);
    memcpy(pVertexDataBegin, vertices, sizeof(vertices));
    g_VertexBuffer->Unmap(0, nullptr);

    // Bindless Space 0 (Index 0): Set up Vertex Buffer Raw SRV
    D3D12_SHADER_RESOURCE_VIEW_DESC vbSrvDesc = {};
    vbSrvDesc.Format = DXGI_FORMAT_R32_TYPELESS;
    vbSrvDesc.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
    vbSrvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    vbSrvDesc.Buffer.FirstElement = 0;
    vbSrvDesc.Buffer.NumElements = vertexBufferSize / 4; 
    vbSrvDesc.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_RAW;

    CD3DX12_CPU_DESCRIPTOR_HANDLE srvHandle(g_SrvHeap->GetCPUDescriptorHandleForHeapStart());
    g_Device->CreateShaderResourceView(g_VertexBuffer.Get(), &vbSrvDesc, srvHandle);

    // Create and upload a 2x2 colored texture
    const UINT TextureWidth = 2;
    const UINT TextureHeight = 2;

    CD3DX12_HEAP_PROPERTIES defaultHeapProps(D3D12_HEAP_TYPE_DEFAULT);
    CD3DX12_RESOURCE_DESC texDesc = CD3DX12_RESOURCE_DESC::Tex2D(
        DXGI_FORMAT_R8G8B8A8_UNORM,
        TextureWidth,
        TextureHeight,
        1, 1
    );

    g_Device->CreateCommittedResource(
        &defaultHeapProps,
        D3D12_HEAP_FLAG_NONE,
        &texDesc,
        D3D12_RESOURCE_STATE_COPY_DEST,
        nullptr,
        IID_PPV_ARGS(&g_Texture)
    );

    // Texture upload buffer allocation
    const UINT64 uploadBufferSize = GetRequiredIntermediateSize(g_Texture.Get(), 0, 1);
    CD3DX12_RESOURCE_DESC uploadBufferDesc = CD3DX12_RESOURCE_DESC::Buffer(uploadBufferSize);
    g_Device->CreateCommittedResource(
        &uploadHeapProps,
        D3D12_HEAP_FLAG_NONE,
        &uploadBufferDesc,
        D3D12_RESOURCE_STATE_GENERIC_READ,
        nullptr,
        IID_PPV_ARGS(&g_UploadBuffer)
    );

    // Raw 2x2 color pixel data (Red, Green, Blue, White)
    uint32_t pixels[] = {
        0xFF0000FF, 0xFF00FF00,
        0xFFFF0000, 0xFFFFFFFF
    };

    // Prepare and copy texture footprint
    D3D12_TEXTURE_COPY_LOCATION dstCopyLoc = {};
    dstCopyLoc.pResource = g_Texture.Get();
    dstCopyLoc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    dstCopyLoc.SubresourceIndex = 0;

    D3D12_TEXTURE_COPY_LOCATION srcCopyLoc = {};
    srcCopyLoc.pResource = g_UploadBuffer.Get();
    srcCopyLoc.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    srcCopyLoc.PlacedFootprint.Offset = 0;
    srcCopyLoc.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    srcCopyLoc.PlacedFootprint.Footprint.Width = TextureWidth;
    srcCopyLoc.PlacedFootprint.Footprint.Height = TextureHeight;
    srcCopyLoc.PlacedFootprint.Footprint.Depth = 1;
    srcCopyLoc.PlacedFootprint.Footprint.RowPitch = D3D12_TEXTURE_DATA_PITCH_ALIGNMENT;

    // Map to upload buffer with correct pitch alignment
    void* pTextureData = nullptr;
    g_UploadBuffer->Map(0, nullptr, &pTextureData);
    BYTE* pDestMem = reinterpret_cast<BYTE*>(pTextureData);
    memcpy(pDestMem, &pixels[0], sizeof(uint32_t) * 2); // Row 0
    memcpy(pDestMem + D3D12_TEXTURE_DATA_PITCH_ALIGNMENT, &pixels[2], sizeof(uint32_t) * 2); // Row 1
    g_UploadBuffer->Unmap(0, nullptr);

    g_CommandList->CopyTextureRegion(&dstCopyLoc, 0, 0, 0, &srcCopyLoc, nullptr);

    // Transition texture to Shader Resource state
    CD3DX12_RESOURCE_BARRIER transitionBarrier = CD3DX12_RESOURCE_BARRIER::Transition(
        g_Texture.Get(),
        D3D12_RESOURCE_STATE_COPY_DEST,
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE
    );
    g_CommandList->ResourceBarrier(1, &transitionBarrier);

    // Bindless Space 1 (Index 1000): Setup Texture SRV
    D3D12_SHADER_RESOURCE_VIEW_DESC texSrvDesc = {};
    texSrvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    texSrvDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    texSrvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    texSrvDesc.Texture2D.MipLevels = 1;

    CD3DX12_CPU_DESCRIPTOR_HANDLE texSrvHandle(g_SrvHeap->GetCPUDescriptorHandleForHeapStart(), 1000, g_SrvDescriptorSize);
    g_Device->CreateShaderResourceView(g_Texture.Get(), &texSrvDesc, texSrvHandle);

    // Execute setup command list and wait
    g_CommandList->Close();
    ID3D12CommandList* ppCommandLists[] = { g_CommandList.Get() };
    g_CommandQueue->ExecuteCommandLists(_countof(ppCommandLists), ppCommandLists);

    // Fence Synchronization Initialization
    g_Device->CreateFence(g_FenceValues[g_FrameIndex], D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g_Fence));
    g_FenceValues[g_FrameIndex]++;
    g_FenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);

    WaitForGpu();

    // Main Loop
    MSG msg = {};
    while (msg.message != WM_QUIT)
    {
        if (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE))
        {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }
        else
        {
            // Frame Rendering
            g_CommandAllocators[g_FrameIndex]->Reset();
            g_CommandList->Reset(g_CommandAllocators[g_FrameIndex].Get(), g_PipelineState.Get());

            // Set up Render Viewports
            D3D12_VIEWPORT viewport = { 0.0f, 0.0f, static_cast<float>(WindowWidth), static_cast<float>(WindowHeight), 0.0f, 1.0f };
            D3D12_RECT scissorRect = { 0, 0, static_cast<LONG>(WindowWidth), static_cast<LONG>(WindowHeight) };
            g_CommandList->RSSetViewports(1, &viewport);
            g_CommandList->RSSetScissorRects(1, &scissorRect);

            // Transition Backbuffer to Render Target
            CD3DX12_RESOURCE_BARRIER renderBarrier = CD3DX12_RESOURCE_BARRIER::Transition(
                g_RenderTargets[g_FrameIndex].Get(),
                D3D12_RESOURCE_STATE_PRESENT,
                D3D12_RESOURCE_STATE_RENDER_TARGET
            );
            g_CommandList->ResourceBarrier(1, &renderBarrier);

            CD3DX12_CPU_DESCRIPTOR_HANDLE rtvHandle(g_RtvHeap->GetCPUDescriptorHandleForHeapStart(), g_FrameIndex, g_RtvDescriptorSize);
            g_CommandList->OMSetRenderTargets(1, &rtvHandle, FALSE, nullptr);

            // Clear Screen
            const float clearColor[] = { 0.15f, 0.15f, 0.15f, 1.0f };
            g_CommandList->ClearRenderTargetView(rtvHandle, clearColor, 0, nullptr);

            // Set root signature and dynamic shader descriptor heaps
            g_CommandList->SetGraphicsRootSignature(g_RootSignature.Get());
            ID3D12DescriptorHeap* heaps[] = { g_SrvHeap.Get() };
            g_CommandList->SetDescriptorHeaps(_countof(heaps), heaps);

            // Parameter 1: Bind global descriptor heap start
            g_CommandList->SetGraphicsRootDescriptorTable(1, g_SrvHeap->GetGPUDescriptorHandleForHeapStart());

            // Parameter 0: Bindless index constants mapping to resources dynamically
            BindlessIndices indices = {};
            indices.vertexBufferIndex = 0; 
            indices.textureIndex = 0;      
            g_CommandList->SetGraphicsRoot32BitConstants(0, 2, &indices, 0);

            // Draw full geometry directly without Input Layouts or traditional VBV binding
            g_CommandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            g_CommandList->DrawInstanced(3, 1, 0, 0);

            // Transition Backbuffer to Present state
            CD3DX12_RESOURCE_BARRIER presentBarrier = CD3DX12_RESOURCE_BARRIER::Transition(
                g_RenderTargets[g_FrameIndex].Get(),
                D3D12_RESOURCE_STATE_RENDER_TARGET,
                D3D12_RESOURCE_STATE_PRESENT
            );
            g_CommandList->ResourceBarrier(1, &presentBarrier);

            g_CommandList->Close();

            // Execute frame drawing
            ID3D12CommandList* ppRenderCommandLists[] = { g_CommandList.Get() };
            g_CommandQueue->ExecuteCommandLists(_countof(ppRenderCommandLists), ppRenderCommandLists);

            g_SwapChain->Present(1, 0);

            MoveToNextFrame();
        }
    }

    // Terminating and synchronizing remaining commands
    WaitForGpu();
    CloseHandle(g_FenceEvent);

    return 0;
}

// Win32 Window message handling function
LRESULT CALLBACK WndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    switch (message)
    {
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    default:
        return DefWindowProc(hWnd, message, wParam, lParam);
    }
}

// Ensure complete graphics pipeline synchronization
void WaitForGpu()
{
    g_CommandQueue->Signal(g_Fence.Get(), g_FenceValues[g_FrameIndex]);
    g_Fence->SetEventOnCompletion(g_FenceValues[g_FrameIndex], g_FenceEvent);
    WaitForSingleObjectEx(g_FenceEvent, INFINITE, FALSE);
    g_FenceValues[g_FrameIndex]++;
}

// Frame pacing for the swap chain backbuffers
void MoveToNextFrame()
{
    const UINT64 currentFenceValue = g_FenceValues[g_FrameIndex];
    g_CommandQueue->Signal(g_Fence.Get(), currentFenceValue);

    g_FrameIndex = g_SwapChain->GetCurrentBackBufferIndex();

    if (g_Fence->GetCompletedValue() < g_FenceValues[g_FrameIndex])
    {
        g_Fence->SetEventOnCompletion(g_FenceValues[g_FrameIndex], g_FenceEvent);
        WaitForSingleObjectEx(g_FenceEvent, INFINITE, FALSE);
    }

    g_FenceValues[g_FrameIndex] = currentFenceValue + 1;
}
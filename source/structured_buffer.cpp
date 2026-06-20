#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <d3dcompiler.h>
#include <DirectXMath.h>
#include <vector>
#include <algorithm>
#include "d3dx12.h" // third

#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3dcompiler.lib")

using namespace DirectX;

// --- const ---
const int FRAME_COUNT = 2;
const int OBJECT_COUNT = 100; // more objects

struct Vertex {
    XMFLOAT3 position;
    XMFLOAT2 uv;
};

// StructuredBuffer 
struct ObjectData {
    XMFLOAT4X4 world;
    XMFLOAT4 color;
    uint32_t type; 
    uint32_t padding[3]; 
};

struct SceneConstants {
    XMFLOAT4X4 viewProj;
};

struct RenderObject {
    XMFLOAT3 position;
    XMFLOAT4 color;
    int type;
    int shaderType;
    bool isTransparent;
    float depth;
};

// --- global ---
HWND g_hWnd = nullptr;
ID3D12Device* g_device = nullptr;
ID3D12CommandQueue* g_commandQueue = nullptr;
IDXGISwapChain3* g_swapChain = nullptr;
ID3D12DescriptorHeap* g_rtvHeap = nullptr;
ID3D12Resource* g_renderTargets[FRAME_COUNT];
ID3D12CommandAllocator* g_commandAllocators[FRAME_COUNT];
ID3D12GraphicsCommandList* g_commandList = nullptr;
ID3D12RootSignature* g_rootSignature = nullptr;
ID3D12PipelineState* g_psoArray[2][2]; 
ID3D12Resource* g_vertexBuffer = nullptr;
D3D12_VERTEX_BUFFER_VIEW g_vertexBufferView;
ID3D12Resource* g_depthStencil = nullptr;
ID3D12DescriptorHeap* g_dsvHeap = nullptr;

// store StructuredBuffer
ID3D12Resource* g_objectDataBuffer = nullptr;
ObjectData* g_pObjectDataBegin = nullptr;

UINT g_frameIndex = 0;
HANDLE g_fenceEvent;
ID3D12Fence* g_fence = nullptr;
UINT64 g_fenceValue = 0;
XMFLOAT3 g_camPos = { 0.0f, 2.0f, -20.0f };
std::vector<RenderObject> g_objects;

// --- HLSL ---
const char* g_shaderSource = R"(
struct SceneCB { float4x4 viewProj; };
struct ObjectData { 
    float4x4 world; 
    float4 color; 
    uint type; 
    uint3 padding; // append 12 byte, make struct up to  96 bytes
};


ConstantBuffer<SceneCB> scene : register(b0);
StructuredBuffer<ObjectData> allObjects : register(t0); // struct buffer

// throw Root Constant pass current object ID
cbuffer RootConstants : register(b1) {
    uint objectIndex;
};

struct VOut {
    float4 pos : SV_POSITION;
    float2 uv : TEXCOORD;
};

VOut VSMain(float3 pos : POSITION, float2 uv : TEXCOORD) {
    VOut vout;
    ObjectData data = allObjects[objectIndex]; // get Buffer data use id
    float4 wpos = mul(float4(pos, 1.0f), data.world);
    vout.pos = mul(wpos, scene.viewProj);
    vout.uv = uv;
    return vout;
}

float4 PSMain(VOut pin) : SV_Target {
    ObjectData data = allObjects[objectIndex];
    if (data.type == 1 && distance(pin.uv, float2(0.5, 0.5)) > 0.5) discard;
    return data.color;
}

float4 PSStripes(VOut pin) : SV_Target {
    ObjectData data = allObjects[objectIndex];
    if (data.type == 1 && distance(pin.uv, float2(0.5, 0.5)) > 0.5) discard;
    float stripes = frac(pin.uv.y * 10.0) > 0.5 ? 1.0 : 0.7;
    return data.color * stripes;
}
)";

// --- init ---

void WaitForPreviousFrame() {
    const UINT64 fence = g_fenceValue;
    g_commandQueue->Signal(g_fence, fence);
    g_fenceValue++;
    if (g_fence->GetCompletedValue() < fence) {
        g_fence->SetEventOnCompletion(fence, g_fenceEvent);
        WaitForSingleObject(g_fenceEvent, INFINITE);
    }
    g_frameIndex = g_swapChain->GetCurrentBackBufferIndex();
}

void InitD3D() {
#if defined(_DEBUG)
    ID3D12Debug* debugController;
    if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debugController)))) {
        debugController->EnableDebugLayer();
    }
#endif

    D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&g_device));

    D3D12_COMMAND_QUEUE_DESC queueDesc = {};
    queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    g_device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&g_commandQueue));

    DXGI_SWAP_CHAIN_DESC1 swapChainDesc = {};
    swapChainDesc.BufferCount = FRAME_COUNT;
    swapChainDesc.Width = 1280;
    swapChainDesc.Height = 720;
    swapChainDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    swapChainDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    swapChainDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    swapChainDesc.SampleDesc.Count = 1;

    IDXGIFactory4* factory;
    CreateDXGIFactory1(IID_PPV_ARGS(&factory));
    IDXGISwapChain1* tempSwapChain;
    factory->CreateSwapChainForHwnd(g_commandQueue, g_hWnd, &swapChainDesc, nullptr, nullptr, &tempSwapChain);
    tempSwapChain->QueryInterface(IID_PPV_ARGS(&g_swapChain));
    g_frameIndex = g_swapChain->GetCurrentBackBufferIndex();

    D3D12_DESCRIPTOR_HEAP_DESC rtvHeapDesc = { D3D12_DESCRIPTOR_HEAP_TYPE_RTV, FRAME_COUNT, D3D12_DESCRIPTOR_HEAP_FLAG_NONE };
    g_device->CreateDescriptorHeap(&rtvHeapDesc, IID_PPV_ARGS(&g_rtvHeap));
    D3D12_DESCRIPTOR_HEAP_DESC dsvHeapDesc = { D3D12_DESCRIPTOR_HEAP_TYPE_DSV, 1, D3D12_DESCRIPTOR_HEAP_FLAG_NONE };
    g_device->CreateDescriptorHeap(&dsvHeapDesc, IID_PPV_ARGS(&g_dsvHeap));

    SIZE_T rtvDescriptorSize = g_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle(g_rtvHeap->GetCPUDescriptorHandleForHeapStart());
    for (int i = 0; i < FRAME_COUNT; i++) {
        g_swapChain->GetBuffer(i, IID_PPV_ARGS(&g_renderTargets[i]));
        g_device->CreateRenderTargetView(g_renderTargets[i], nullptr, rtvHandle);
        rtvHandle.ptr += rtvDescriptorSize;
        g_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&g_commandAllocators[i]));
    }

    // depth buffer
    D3D12_HEAP_PROPERTIES depthHeapProp = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
    D3D12_RESOURCE_DESC depthDesc = CD3DX12_RESOURCE_DESC::Tex2D(DXGI_FORMAT_D32_FLOAT, 1280, 720, 1, 1, 1, 0, D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL);
    D3D12_CLEAR_VALUE depthClear = { DXGI_FORMAT_D32_FLOAT, {1.0f, 0} };
    g_device->CreateCommittedResource(&depthHeapProp, D3D12_HEAP_FLAG_NONE, &depthDesc, D3D12_RESOURCE_STATE_DEPTH_WRITE, &depthClear, IID_PPV_ARGS(&g_depthStencil));
    g_device->CreateDepthStencilView(g_depthStencil, nullptr, g_dsvHeap->GetCPUDescriptorHandleForHeapStart());

    // --- root signature ---
    CD3DX12_ROOT_PARAMETER rootParameters[3];
    rootParameters[0].InitAsConstants(16, 0); // b0: Scene ViewProj
    rootParameters[1].InitAsShaderResourceView(0); // t0: StructuredBuffer (object list)
    rootParameters[2].InitAsConstants(1, 1);  // b1: current object ref (Root Constant)

    CD3DX12_ROOT_SIGNATURE_DESC rootSigDesc(3, rootParameters, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT);
    ID3DBlob* signature;
    ID3DBlob* error;
    D3D12SerializeRootSignature(&rootSigDesc, D3D_ROOT_SIGNATURE_VERSION_1, &signature, &error);
    g_device->CreateRootSignature(0, signature->GetBufferPointer(), signature->GetBufferSize(), IID_PPV_ARGS(&g_rootSignature));

    // PSO compile
    ID3DBlob* vs, * ps1, * ps2;
    D3DCompile(g_shaderSource, strlen(g_shaderSource), nullptr, nullptr, nullptr, "VSMain", "vs_5_1", 0, 0, &vs, &error);
    D3DCompile(g_shaderSource, strlen(g_shaderSource), nullptr, nullptr, nullptr, "PSMain", "ps_5_1", 0, 0, &ps1, &error);
    D3DCompile(g_shaderSource, strlen(g_shaderSource), nullptr, nullptr, nullptr, "PSStripes", "ps_5_1", 0, 0, &ps2, &error);

    D3D12_INPUT_ELEMENT_DESC layout[] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 }
    };

    D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc = {};
    psoDesc.InputLayout = { layout, 2 };
    psoDesc.pRootSignature = g_rootSignature;
    psoDesc.VS = CD3DX12_SHADER_BYTECODE(vs);
    psoDesc.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
    psoDesc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    psoDesc.DepthStencilState = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
    psoDesc.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
    psoDesc.SampleMask = UINT_MAX;
    psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    psoDesc.NumRenderTargets = 1;
    psoDesc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    psoDesc.DSVFormat = DXGI_FORMAT_D32_FLOAT;
    psoDesc.SampleDesc.Count = 1;

    for (int s = 0; s < 2; s++) {
        psoDesc.PS = s == 0 ? CD3DX12_SHADER_BYTECODE(ps1) : CD3DX12_SHADER_BYTECODE(ps2);
        // Opaque
        psoDesc.BlendState.RenderTarget[0].BlendEnable = FALSE;
        psoDesc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
        g_device->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&g_psoArray[s][0]));
        // Alpha
        psoDesc.BlendState.RenderTarget[0].BlendEnable = TRUE;
        psoDesc.BlendState.RenderTarget[0].SrcBlend = D3D12_BLEND_SRC_ALPHA;
        psoDesc.BlendState.RenderTarget[0].DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
        psoDesc.BlendState.RenderTarget[0].BlendOp = D3D12_BLEND_OP_ADD;
        psoDesc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO; 
        g_device->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&g_psoArray[s][1]));
    }

    // vertex buffer
    Vertex verts[] = {
        {{-1,-1,0},{0,1}}, {{-1,1,0},{0,0}}, {{1,-1,0},{1,1}},
        {{1,-1,0},{1,1}}, {{-1,1,0},{0,0}}, {{1,1,0},{1,0}}
    };
    CD3DX12_HEAP_PROPERTIES vUpload(D3D12_HEAP_TYPE_UPLOAD);
    auto vBufferDesc = CD3DX12_RESOURCE_DESC::Buffer(sizeof(verts));
    g_device->CreateCommittedResource(&vUpload, D3D12_HEAP_FLAG_NONE, &vBufferDesc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&g_vertexBuffer));
    void* p; g_vertexBuffer->Map(0, nullptr, &p); memcpy(p, verts, sizeof(verts)); g_vertexBuffer->Unmap(0, nullptr);
    g_vertexBufferView = { g_vertexBuffer->GetGPUVirtualAddress(), sizeof(verts), sizeof(Vertex) };

    // --- create StructuredBuffer ---
    UINT bufferSize = sizeof(ObjectData) * OBJECT_COUNT;
    auto srvBufferDesc = CD3DX12_RESOURCE_DESC::Buffer(bufferSize);
    g_device->CreateCommittedResource(&vUpload, D3D12_HEAP_FLAG_NONE, &srvBufferDesc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&g_objectDataBuffer));
    // persistent mapping, for update refresh
    g_objectDataBuffer->Map(0, nullptr, (void**)&g_pObjectDataBegin);

    g_device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g_commandAllocators[0], nullptr, IID_PPV_ARGS(&g_commandList));
    g_commandList->Close();
    g_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g_fence));
    g_fenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);

    // random object
    for (int i = 0; i < OBJECT_COUNT; i++) {
        RenderObject obj;
        obj.position = { (float)(rand()%40-20), (float)(rand()%20-10), (float)(rand()%40-10) };
        obj.color = { (rand()%100)/100.f, (rand()%100)/100.f, (rand()%100)/100.f, (rand()%10 > 7) ? 0.4f : 1.0f };
        obj.isTransparent = obj.color.w < 1.0f;
        obj.type = rand() % 2;
        obj.shaderType = rand() % 2;
        g_objects.push_back(obj);
    }
}

void Update() {
    float speed = 0.2f;
    if (GetAsyncKeyState('W')) g_camPos.z += speed;
    if (GetAsyncKeyState('S')) g_camPos.z -= speed;
    if (GetAsyncKeyState('A')) g_camPos.x -= speed;
    if (GetAsyncKeyState('D')) g_camPos.x += speed;

    XMMATRIX view = XMMatrixLookAtLH(XMLoadFloat3(&g_camPos), XMLoadFloat3(&g_camPos) + XMVectorSet(0,0,1,0), XMVectorSet(0,1,0,0));
    XMMATRIX proj = XMMatrixPerspectiveFovLH(XM_PIDIV4, 1280.f/720.f, 0.1f, 1000.f);
    XMMATRIX vp = view * proj;

    // 1. update matrix
    for (int i = 0; i < OBJECT_COUNT; i++) {
        auto& obj = g_objects[i];
        XMMATRIX world = XMMatrixRotationY(GetTickCount64()*0.001f + i) * XMMatrixTranslation(obj.position.x, obj.position.y, obj.position.z);
        XMStoreFloat4x4(&g_pObjectDataBegin[i].world, XMMatrixTranspose(world));
        g_pObjectDataBegin[i].color = obj.color;
        g_pObjectDataBegin[i].type = (uint32_t)obj.type;

        // sort
        XMVECTOR p = XMLoadFloat3(&obj.position);
        obj.depth = XMVectorGetX(XMVector3Length(p - XMLoadFloat3(&g_camPos)));
    }

    // 2. sort for blend
    // need save object raw index for hlsl access buffer
    struct SortEntry { int originalIndex; float depth; bool isTransparent; };
    std::vector<SortEntry> sortedIndices;
    for(int i=0; i<OBJECT_COUNT; ++i) sortedIndices.push_back({i, g_objects[i].depth, g_objects[i].isTransparent});

    std::sort(sortedIndices.begin(), sortedIndices.end(), [](const SortEntry& a, const SortEntry& b) {
        if (a.isTransparent != b.isTransparent) return !a.isTransparent;
        return a.depth > b.depth;
    });

    // 3. start render command
    g_commandAllocators[g_frameIndex]->Reset();
    g_commandList->Reset(g_commandAllocators[g_frameIndex], nullptr);

    CD3DX12_RESOURCE_BARRIER b1 = CD3DX12_RESOURCE_BARRIER::Transition(g_renderTargets[g_frameIndex], D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);
    g_commandList->ResourceBarrier(1, &b1);

    CD3DX12_CPU_DESCRIPTOR_HANDLE rtvHandle(g_rtvHeap->GetCPUDescriptorHandleForHeapStart(), g_frameIndex, g_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV));
    CD3DX12_CPU_DESCRIPTOR_HANDLE dsvHandle(g_dsvHeap->GetCPUDescriptorHandleForHeapStart());

    const float clearCol[] = { 0.05f, 0.05f, 0.1f, 1.0f };
    g_commandList->ClearRenderTargetView(rtvHandle, clearCol, 0, nullptr);
    g_commandList->ClearDepthStencilView(dsvHandle, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);

    g_commandList->SetGraphicsRootSignature(g_rootSignature);
    g_commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    g_commandList->IASetVertexBuffers(0, 1, &g_vertexBufferView);
    g_commandList->RSSetViewports(1, &CD3DX12_VIEWPORT(0.0f, 0.0f, 1280.0f, 720.0f));
    g_commandList->RSSetScissorRects(1, &CD3DX12_RECT(0, 0, 1280, 720));
    g_commandList->OMSetRenderTargets(1, &rtvHandle, FALSE, &dsvHandle);

    // set Scene const (b0)
    XMFLOAT4X4 vpData; XMStoreFloat4x4(&vpData, XMMatrixTranspose(vp));
    g_commandList->SetGraphicsRoot32BitConstants(0, 16, &vpData, 0);

    // set StructuredBuffer (t0) --- only need once !
    g_commandList->SetGraphicsRootShaderResourceView(1, g_objectDataBuffer->GetGPUVirtualAddress());

    // draw objects
    for (auto& entry : sortedIndices) {
        int idx = entry.originalIndex;
        auto& obj = g_objects[idx];

        g_commandList->SetPipelineState(g_psoArray[obj.shaderType][obj.isTransparent ? 1 : 0]);
        
        // pass a Index (b1) --- very light
        g_commandList->SetGraphicsRoot32BitConstants(2, 1, &idx, 0);
        
        g_commandList->DrawInstanced(6, 1, 0, 0);
    }

    CD3DX12_RESOURCE_BARRIER b2 = CD3DX12_RESOURCE_BARRIER::Transition(g_renderTargets[g_frameIndex], D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);
    g_commandList->ResourceBarrier(1, &b2);
    g_commandList->Close();
    ID3D12CommandList* lists[] = { g_commandList };
    g_commandQueue->ExecuteCommandLists(1, lists);
    g_swapChain->Present(1, 0);
    WaitForPreviousFrame();
}

LRESULT CALLBACK WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == WM_DESTROY) { PostQuitMessage(0); return 0; }
    return DefWindowProc(hWnd, msg, wParam, lParam);
}
int main(int argc, char** argv)
{
    HINSTANCE hI = GetModuleHandle(NULL);
    WNDCLASSEX wc = { sizeof(WNDCLASSEX), CS_HREDRAW|CS_VREDRAW, WndProc, 0,0,hI,0,0,0,0, L"D3D12Structured", 0 };
    RegisterClassEx(&wc);
    g_hWnd = CreateWindow(wc.lpszClassName, L"D3D12 StructuredBuffer Sample", WS_OVERLAPPEDWINDOW, 100, 100, 1280, 720, 0, 0, hI, 0);
    InitD3D(); ShowWindow(g_hWnd, true);
    MSG msg = {}; while (msg.message != WM_QUIT) {
        if (PeekMessage(&msg, 0, 0, 0, PM_REMOVE)) { TranslateMessage(&msg); DispatchMessage(&msg); }
        else Update();
    }
    return 0;
}
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <d3dcompiler.h>
#include <DirectXMath.h>
#include <vector>
#include <string>
#include <wrl.h>
#include "d3dx12.h" // third

#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3dcompiler.lib")

using namespace DirectX;
using Microsoft::WRL::ComPtr;

// --- config ---
const int FrameCount = 2;
const int ObjectCount = 20;

struct SceneCB {
    XMMATRIX viewProj;
};

struct ObjectCB {
    XMMATRIX world;
    XMFLOAT4 color;
    uint32_t type; // 0: Square, 1: Circle
    float padding[3];
};

struct Vertex {
    XMFLOAT3 position;
    XMFLOAT2 uv;
};

// --- shader ---
const char* shaderCode = R"(
struct SceneCB {
    float4x4 viewProj;
};
struct ObjectCB {
    float4x4 world;
    float4 color;
    uint type;
};

ConstantBuffer<SceneCB> sceneData : register(b0);
ConstantBuffer<ObjectCB> objectData : register(b1);

struct PSInput {
    float4 position : SV_POSITION;
    float2 uv : TEXCOORD;
};

PSInput VSMain(float3 position : POSITION, float2 uv : TEXCOORD) {
    PSInput result;
    result.position = mul(mul(float4(position, 1.0f), objectData.world), sceneData.viewProj);
    result.uv = uv;
    return result;
}

float4 PSMain(PSInput input) : SV_TARGET {
    if (objectData.type == 1) { // Circle
        float dist = length(input.uv - float2(0.5, 0.5));
        if (dist > 0.5) discard;
    }
    return objectData.color;
}
)";

// --- global ---
HWND hwnd;
ComPtr<ID3D12Device> device;
ComPtr<ID3D12CommandQueue> commandQueue;
ComPtr<IDXGISwapChain3> swapChain;
ComPtr<ID3D12DescriptorHeap> rtvHeap;
ComPtr<ID3D12DescriptorHeap> cbvHeap;
ComPtr<ID3D12Resource> renderTargets[FrameCount];
ComPtr<ID3D12CommandAllocator> commandAllocators[FrameCount];
ComPtr<ID3D12GraphicsCommandList> commandList;
ComPtr<ID3D12RootSignature> rootSignature;
ComPtr<ID3D12PipelineState> psoOpaque;
ComPtr<ID3D12PipelineState> psoAlpha;
ComPtr<ID3D12Resource> vertexBuffer;
D3D12_VERTEX_BUFFER_VIEW vertexBufferView;
ComPtr<ID3D12Resource> constantBuffer;
UINT8* cbvDataBegin = nullptr;

UINT rtvDescriptorSize;
UINT frameIndex;
HANDLE fenceEvent;
ComPtr<ID3D12Fence> fence;
UINT64 fenceValues[FrameCount];

// camera
XMVECTOR camPos = XMVectorSet(0, 0, -10, 0);
float camYaw = 0, camPitch = 0;
POINT lastMouse;

// object
struct GameObject {
    XMFLOAT3 pos;
    XMFLOAT4 color;
    int type; // 0: Square, 1: Circle
    bool transparent;
};
std::vector<GameObject> objects;

// --- functions ---
bool InitD3D();
void Update();
void Render();
void Cleanup();
LRESULT CALLBACK WindowProc(HWND, UINT, WPARAM, LPARAM);

int main(int argc, char** argv)
{
    HINSTANCE hI = GetModuleHandle(NULL);

    WNDCLASSEX windowClass = { sizeof(WNDCLASSEX), CS_HREDRAW | CS_VREDRAW, WindowProc, 0, 0, hI, 0, 0, 0, 0, L"D3D12Sample", 0 };
    RegisterClassEx(&windowClass);
    hwnd = CreateWindow(windowClass.lpszClassName, L"D3D12 CBV Multi-Object Sample", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 1280, 720, 0, 0, hI, 0);

    if (InitD3D()) {
        ShowWindow(hwnd, true);
        MSG msg = {};
        while (msg.message != WM_QUIT) {
            if (PeekMessage(&msg, NULL, 0, 0, PM_REMOVE)) {
                TranslateMessage(&msg);
                DispatchMessage(&msg);
            }
            Update();
            Render();
        }
    }
    Cleanup();
    return 0;
}

bool InitD3D() {
    // 1. create devices
    CreateDXGIFactory2(0, IID_PPV_ARGS(&swapChain)); // temp swap chain
    D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device));

    // 2. command list
    D3D12_COMMAND_QUEUE_DESC queueDesc = {};
    queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&commandQueue));

    // 3. swap chain
    DXGI_SWAP_CHAIN_DESC1 scDesc = {};
    scDesc.BufferCount = FrameCount;
    scDesc.Width = 1280;
    scDesc.Height = 720;
    scDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    scDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    scDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    scDesc.SampleDesc.Count = 1;

    ComPtr<IDXGIFactory4> factory;
    CreateDXGIFactory2(0, IID_PPV_ARGS(&factory));
    ComPtr<IDXGISwapChain1> sc1;
    factory->CreateSwapChainForHwnd(commandQueue.Get(), hwnd, &scDesc, nullptr, nullptr, &sc1);
    sc1.As(&swapChain);
    frameIndex = swapChain->GetCurrentBackBufferIndex();

    // 4. decs heap (RTV and CBV)
    D3D12_DESCRIPTOR_HEAP_DESC rtvHeapDesc = { D3D12_DESCRIPTOR_HEAP_TYPE_RTV, FrameCount, D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0 };
    device->CreateDescriptorHeap(&rtvHeapDesc, IID_PPV_ARGS(&rtvHeap));
    rtvDescriptorSize = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

    // CBV heap: 1 SceneCB + ObjectCount * ObjectCB (each Frame)
    D3D12_DESCRIPTOR_HEAP_DESC cbvHeapDesc = { D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, (1 + ObjectCount) * FrameCount, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0 };
    device->CreateDescriptorHeap(&cbvHeapDesc, IID_PPV_ARGS(&cbvHeap));

    // 5. RTV resource
    CD3DX12_CPU_DESCRIPTOR_HANDLE rtvHandle(rtvHeap->GetCPUDescriptorHandleForHeapStart());
    for (int n = 0; n < FrameCount; n++) {
        swapChain->GetBuffer(n, IID_PPV_ARGS(&renderTargets[n]));
        device->CreateRenderTargetView(renderTargets[n].Get(), nullptr, rtvHandle);
        rtvHandle.Offset(1, rtvDescriptorSize);
        device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&commandAllocators[n]));
    }

    // 6. root signeature
    // param0: Scene CBV, param1: Object CBV
    CD3DX12_ROOT_PARAMETER rootParameters[2];
    rootParameters[0].InitAsConstantBufferView(0); // b0
    rootParameters[1].InitAsConstantBufferView(1); // b1

    CD3DX12_ROOT_SIGNATURE_DESC rootSigDesc;
    rootSigDesc.Init(2, rootParameters, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT);
    
    ComPtr<ID3DBlob> signature;
    D3D12SerializeRootSignature(&rootSigDesc, D3D_ROOT_SIGNATURE_VERSION_1, &signature, nullptr);
    device->CreateRootSignature(0, signature->GetBufferPointer(), signature->GetBufferSize(), IID_PPV_ARGS(&rootSignature));

    // 7. compile Shader and PSO
    ComPtr<ID3DBlob> vs, ps;
    D3DCompile(shaderCode, strlen(shaderCode), nullptr, nullptr, nullptr, "VSMain", "vs_5_1", 0, 0, &vs, nullptr);
    D3DCompile(shaderCode, strlen(shaderCode), nullptr, nullptr, nullptr, "PSMain", "ps_5_1", 0, 0, &ps, nullptr);

    D3D12_INPUT_ELEMENT_DESC inputElementDescs[] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 }
    };

    D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc = {};
    psoDesc.InputLayout = { inputElementDescs, _countof(inputElementDescs) };
    psoDesc.pRootSignature = rootSignature.Get();
    psoDesc.VS = CD3DX12_SHADER_BYTECODE(vs.Get());
    psoDesc.PS = CD3DX12_SHADER_BYTECODE(ps.Get());
    psoDesc.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
    psoDesc.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
    psoDesc.DepthStencilState.DepthEnable = FALSE; // 2D
    psoDesc.DepthStencilState.StencilEnable = FALSE;
    psoDesc.SampleMask = UINT_MAX;
    psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    psoDesc.NumRenderTargets = 1;
    psoDesc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    psoDesc.SampleDesc.Count = 1;

    // opaque PSO
    device->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&psoOpaque));

    // blend PSO
    D3D12_RENDER_TARGET_BLEND_DESC transparencyBlend = {
        TRUE, FALSE,
        D3D12_BLEND_SRC_ALPHA, D3D12_BLEND_INV_SRC_ALPHA, D3D12_BLEND_OP_ADD,
        D3D12_BLEND_ONE, D3D12_BLEND_ZERO, D3D12_BLEND_OP_ADD,
        D3D12_LOGIC_OP_NOOP, D3D12_COLOR_WRITE_ENABLE_ALL
    };
    psoDesc.BlendState.RenderTarget[0] = transparencyBlend;
    device->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&psoAlpha));

    // 8. vertex buffer (Quad)
    Vertex vertices[] = {
        { {-0.5f,  0.5f, 0.0f}, {0, 0} }, { { 0.5f,  0.5f, 0.0f}, {1, 0} }, { {-0.5f, -0.5f, 0.0f}, {0, 1} },
        { { 0.5f,  0.5f, 0.0f}, {1, 0} }, { { 0.5f, -0.5f, 0.0f}, {1, 1} }, { {-0.5f, -0.5f, 0.0f}, {0, 1} }
    };
    UINT vBufferSize = sizeof(vertices);
    device->CreateCommittedResource(&CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD), D3D12_HEAP_FLAG_NONE, 
        &CD3DX12_RESOURCE_DESC::Buffer(vBufferSize), D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&vertexBuffer));
    UINT8* pVertexDataBegin;
    vertexBuffer->Map(0, nullptr, reinterpret_cast<void**>(&pVertexDataBegin));
    memcpy(pVertexDataBegin, vertices, sizeof(vertices));
    vertexBuffer->Unmap(0, nullptr);
    vertexBufferView.BufferLocation = vertexBuffer->GetGPUVirtualAddress();
    vertexBufferView.StrideInBytes = sizeof(Vertex);
    vertexBufferView.SizeInBytes = vBufferSize;

    // 9. const buffer resource (aligne 256 bytes)
    UINT sceneCBSize = (sizeof(SceneCB) + 255) & ~255;
    UINT objectCBSize = (sizeof(ObjectCB) + 255) & ~255;
    UINT totalCBSize = (sceneCBSize + objectCBSize * ObjectCount) * FrameCount;

    device->CreateCommittedResource(&CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD), D3D12_HEAP_FLAG_NONE,
        &CD3DX12_RESOURCE_DESC::Buffer(totalCBSize), D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&constantBuffer));
    constantBuffer->Map(0, nullptr, reinterpret_cast<void**>(&cbvDataBegin));

    // 10. init random objects
    for (int i = 0; i < ObjectCount; i++) {
        GameObject obj;
        obj.pos = { (float)(rand() % 20 - 10), (float)(rand() % 20 - 10), (float)(rand() % 10) };
        obj.color = { (float)rand() / RAND_MAX, (float)rand() / RAND_MAX, (float)rand() / RAND_MAX, (rand() % 2 == 0) ? 1.0f : 0.4f };
        obj.type = rand() % 2;
        obj.transparent = obj.color.w < 1.0f;
        objects.push_back(obj);
    }

    device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, commandAllocators[frameIndex].Get(), psoOpaque.Get(), IID_PPV_ARGS(&commandList));
    commandList->Close();

    device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence));
    fenceValues[frameIndex]++;
    fenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);

    return true;
}

void Update() {
    // camera moving
    float speed = 0.1f;
    if (GetKeyState('W') & 0x8000) camPos += XMVectorSet(0, 0, speed, 0);
    if (GetKeyState('S') & 0x8000) camPos += XMVectorSet(0, 0, -speed, 0);
    if (GetKeyState('A') & 0x8000) camPos += XMVectorSet(-speed, 0, 0, 0);
    if (GetKeyState('D') & 0x8000) camPos += XMVectorSet(speed, 0, 0, 0);

    XMMATRIX view = XMMatrixLookAtLH(camPos, camPos + XMVectorSet(0, 0, 1, 0), XMVectorSet(0, 1, 0, 0));
    XMMATRIX proj = XMMatrixPerspectiveFovLH(XM_PIDIV4, 1280.0f / 720.0f, 0.1f, 100.0f);
    
    UINT sceneCBSize = (sizeof(SceneCB) + 255) & ~255;
    UINT objectCBSize = (sizeof(ObjectCB) + 255) & ~255;
    UINT frameOffset = frameIndex * (sceneCBSize + objectCBSize * ObjectCount);

    // update global paramter
    SceneCB sceneData = { XMMatrixTranspose(view * proj) };
    memcpy(cbvDataBegin + frameOffset, &sceneData, sizeof(SceneCB));

    // update object constant
    for (int i = 0; i < ObjectCount; i++) {
        ObjectCB objData;
        objData.world = XMMatrixTranspose(XMMatrixTranslation(objects[i].pos.x, objects[i].pos.y, objects[i].pos.z));
        objData.color = objects[i].color;
        objData.type = objects[i].type;
        memcpy(cbvDataBegin + frameOffset + sceneCBSize + i * objectCBSize, &objData, sizeof(ObjectCB));
    }
}

void Render() {
    commandAllocators[frameIndex]->Reset();
    commandList->Reset(commandAllocators[frameIndex].Get(), psoOpaque.Get());

    commandList->SetGraphicsRootSignature(rootSignature.Get());
    ID3D12DescriptorHeap* heaps[] = { cbvHeap.Get() };
    commandList->SetDescriptorHeaps(_countof(heaps), heaps);

    commandList->RSSetViewports(1, &CD3DX12_VIEWPORT(0.0f, 0.0f, 1280.0f, 720.0f));
    commandList->RSSetScissorRects(1, &CD3DX12_RECT(0, 0, 1280, 720));

    commandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(renderTargets[frameIndex].Get(), D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET));

    CD3DX12_CPU_DESCRIPTOR_HANDLE rtvHandle(rtvHeap->GetCPUDescriptorHandleForHeapStart(), frameIndex, rtvDescriptorSize);
    commandList->OMSetRenderTargets(1, &rtvHandle, FALSE, nullptr);
    const float clearColor[] = { 0.1f, 0.2f, 0.3f, 1.0f };
    commandList->ClearRenderTargetView(rtvHandle, clearColor, 0, nullptr);

    commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    commandList->IASetVertexBuffers(0, 1, &vertexBufferView);

    UINT sceneCBSize = (sizeof(SceneCB) + 255) & ~255;
    UINT objectCBSize = (sizeof(ObjectCB) + 255) & ~255;
    UINT frameOffset = frameIndex * (sceneCBSize + objectCBSize * ObjectCount);
    D3D12_GPU_VIRTUAL_ADDRESS cbGpuAddr = constantBuffer->GetGPUVirtualAddress() + frameOffset;

    // bind Scene CBV
    commandList->SetGraphicsRootConstantBufferView(0, cbGpuAddr);

    // draw object (opaque to blend)
    for (int pass = 0; pass < 2; pass++) {
        for (int i = 0; i < ObjectCount; i++) {
            if ((pass == 0 && objects[i].transparent) || (pass == 1 && !objects[i].transparent)) continue;

            commandList->SetPipelineState(objects[i].transparent ? psoAlpha.Get() : psoOpaque.Get());
            commandList->SetGraphicsRootConstantBufferView(1, cbGpuAddr + sceneCBSize + i * objectCBSize);
            commandList->DrawInstanced(6, 1, 0, 0);
        }
    }

    commandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(renderTargets[frameIndex].Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT));
    commandList->Close();

    ID3D12CommandList* ppCommandLists[] = { commandList.Get() };
    commandQueue->ExecuteCommandLists(_countof(ppCommandLists), ppCommandLists);

    swapChain->Present(1, 0);

    // wait last frame (sample sync)
    const UINT64 currentFenceValue = fenceValues[frameIndex];
    commandQueue->Signal(fence.Get(), currentFenceValue);
    frameIndex = swapChain->GetCurrentBackBufferIndex();
    if (fence->GetCompletedValue() < fenceValues[frameIndex]) {
        fence->SetEventOnCompletion(fenceValues[frameIndex], fenceEvent);
        WaitForSingleObjectEx(fenceEvent, INFINITE, FALSE);
    }
    fenceValues[frameIndex] = currentFenceValue + 1;
}

void Cleanup() {
    WaitForSingleObjectEx(fenceEvent, INFINITE, FALSE);
    CloseHandle(fenceEvent);
}

LRESULT CALLBACK WindowProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
    case WM_DESTROY: PostQuitMessage(0); return 0;
    }
    return DefWindowProc(hWnd, message, wParam, lParam);
}
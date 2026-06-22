struct VSInput {
    uint vertexID : SV_VertexID;
};

struct PSInput {
    float4 position : SV_POSITION;
    float2 uv : TEXCOORD;
};

cbuffer BindlessIndices : register(b0)
{
    uint g_vertexBufferIdx;
    uint g_textureIdx;
};

// Built-in sampler (or retrieved bindless sampler from SamplerDescriptorHeap)
SamplerState g_Sampler : register(s0);

PSInput VSMain(VSInput input) {
    PSInput result;
    
    uint offset = input.vertexID * 20;
    
    // SM 6.6 Direct Heap Indexing:
    // We retrieve the ByteAddressBuffer directly from the global resource heap using the index.
    ByteAddressBuffer vBuffer = ResourceDescriptorHeap[g_vertexBufferIdx];
    
    float3 pos = asfloat(vBuffer.Load3(offset));
    float2 uv = asfloat(vBuffer.Load2(offset + 12));
    
    result.position = float4(pos, 1.0f);
    result.uv = uv;
    return result;
}

float4 PSMain(PSInput input) : SV_TARGET {
    // SM 6.6 Direct Heap Indexing:
    // We retrieve the Texture2D directly from the global resource heap using the index.
    // Note: Since space1 started at 1000, texture index is offset accordingly.
    Texture2D tex = ResourceDescriptorHeap[g_textureIdx + 1000];
    
    return tex.Sample(g_Sampler, input.uv);
}
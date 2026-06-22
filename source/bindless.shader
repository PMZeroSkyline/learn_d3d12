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

// Declaring unbounded arrays instead of SM 6.6 direct heap indexing
ByteAddressBuffer g_ByteAddressBuffers[] : register(t0, space0);
Texture2D         g_Textures2D[]         : register(t0, space1);
SamplerState      g_Sampler              : register(s0);

PSInput VSMain(VSInput input) {
    PSInput result;
    
    uint offset = input.vertexID * 20;
    
    // Dynamic array indexing
    float3 pos = asfloat(g_ByteAddressBuffers[g_vertexBufferIdx].Load3(offset));
    float2 uv = asfloat(g_ByteAddressBuffers[g_vertexBufferIdx].Load2(offset + 12));
    
    result.position = float4(pos, 1.0f);
    result.uv = uv;
    return result;
}

float4 PSMain(PSInput input) : SV_TARGET {
    return g_Textures2D[g_textureIdx].Sample(g_Sampler, input.uv);
}
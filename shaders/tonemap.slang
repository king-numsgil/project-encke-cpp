// HDR -> display. A full-screen triangle reads the accumulation buffer,
// applies exposure and a filmic curve, and writes linear display values into
// the _SRGB swapchain, which encodes on write.
//
// The curve is Narkowicz's ACES fit: a placeholder chosen for being short and
// well known, not for accuracy. Swap it for AgX or PBR Neutral when colour
// starts to matter.

import lib.bindless;
import lib.gpu_types;

struct VertexOut
{
    float4 position : SV_Position;
};

[shader("vertex")]
VertexOut vertex_main(uint index : SV_VertexID)
{
    // One triangle covering the screen; the overhang is clipped.
    float2 corner = float2(float((index << 1u) & 2u), float(index & 2u));

    VertexOut output;
    output.position = float4(corner * 2.0f - 1.0f, 0.0f, 1.0f);
    return output;
}

float3 aces(float3 x)
{
    return saturate((x * (2.51f * x + 0.03f)) / (x * (2.43f * x + 0.59f) + 0.14f));
}

[shader("fragment")]
float4 fragment_main(VertexOut input) : SV_Target
{
    // SV_Position.xy is the pixel centre in framebuffer coordinates, which
    // are top-left like the HDR target's -- no conversion.
    int3   texel = int3(int2(input.position.xy), 0);
    float3 hdr   = g_textures[NonUniformResourceIndex(push.hdr_sampled)].Load(texel).rgb;

    return float4(aces(hdr * push.exposure), 1.0f);
}

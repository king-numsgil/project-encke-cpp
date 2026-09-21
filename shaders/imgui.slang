// Dear ImGui draw lists, for ui/imgui_vulkan. Replaces the GLSL pair embedded as
// SPIR-V words in upstream imgui_impl_vulkan.cpp.
//
// Colour conventions, which are the point of having our own shader:
//
//   - Vertex colours are ImGui's, authored in sRGB. They are decoded here.
//   - Textures are LINEAR. ImGui's own (the font atlas) are created _SRGB so
//     the sampler decodes them; renderer targets shown in a window already are.
//   - The product is linear, and is written one of two ways. To an _SRGB target
//     it goes out as-is and the hardware encodes it, so blending happens in
//     linear space. To a UNORM target, push.ui_encode_srgb is set and it is
//     encoded here, so blending happens in sRGB space -- which is what ImGui's
//     styles and anti-aliasing were tuned against.
//
// Either way the colours land where ImGui meant them; only the space the
// blend runs in differs.

import lib.bindless;
import lib.colour;

struct VertexIn
{
    float2 position : POSITION;   // pixels, top-left origin, +Y down
    float2 uv       : TEXCOORD;
    float4 colour   : COLOR;      // R8G8B8A8_UNORM, sRGB-encoded
};

struct VertexOut
{
    float4 position : SV_Position;
    float2 uv       : TEXCOORD;
    float4 colour   : COLOR;
};

[shader("vertex")]
VertexOut vertex_main(VertexIn input)
{
    // ImGui's space is +Y down like the framebuffer, so the pipeline uses an
    // unflipped viewport and this maps straight to NDC.
    VertexOut output;
    output.position = float4(input.position * push.ui_transform.xy + push.ui_transform.zw, 0.0f, 1.0f);
    output.uv       = input.uv;
    output.colour   = input.colour;
    return output;
}

[shader("fragment")]
float4 fragment_main(VertexOut input) : SV_Target
{
    // Not `texture` / `sampler`: both are HLSL type keywords.
    Texture2D<float4> source = g_textures[NonUniformResourceIndex(push.ui_texture)];
    SamplerState      filter = g_samplers[NonUniformResourceIndex(push.ui_sampler)];
    float4            texel  = source.Sample(filter, input.uv);

    // Alpha is coverage, never colour-encoded; only rgb is decoded.
    float3 rgb   = srgb_to_linear(input.colour.rgb) * texel.rgb;
    float  alpha = input.colour.a * texel.a;

    if (push.ui_encode_srgb != 0u)
    {
        rgb = linear_to_srgb(rgb);
    }

    return float4(rgb, alpha);
}

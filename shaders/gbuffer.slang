// Geometry pass. Writes the G-buffer, plus emissive straight into the HDR
// accumulation target so the lighting pass only ever adds to it.
//
//   0  RGBA8_SRGB    albedo.rgb | ao
//   1  RG16_UNORM    view-space normal, octahedral
//   2  RG8_UNORM     roughness | metallic
//   3  RG16_SFLOAT   motion: uv_previous - uv_current
//   4  RGBA16_SFLOAT HDR accumulation, seeded with emissive
//
// The layout is mirrored by the attachment formats in render/renderer.cpp.

import lib.bindless;
import lib.gpu_types;
import lib.normal;
import lib.screen;

struct VertexIn
{
    float3 position : POSITION;
    float3 normal   : NORMAL;
    float3 colour   : COLOR;
};

struct VertexOut
{
    float4 position  : SV_Position;
    float3 normal    : NORMAL;
    float3 colour    : COLOR;
    float4 current   : TEXCOORD0;
    float4 previous  : TEXCOORD1;
};

struct GBuffer
{
    float4 albedo_ao : SV_Target0;
    float2 normal    : SV_Target1;
    float2 material  : SV_Target2;
    float2 motion    : SV_Target3;
    float4 emissive  : SV_Target4;
};

[shader("vertex")]
VertexOut vertex_main(VertexIn input)
{
    GpuObject object = load_object(push.object_index);
    float4    local  = float4(input.position, 1.0f);

    VertexOut output;
    output.position = transform(object.mvp, local);
    output.current  = output.position;
    output.previous = transform(object.prev_mvp, local);
    output.normal   = transform(object.normal_view, float4(input.normal, 0.0f)).xyz;
    output.colour   = input.colour;
    return output;
}

[shader("fragment")]
GBuffer fragment_main(VertexOut input)
{
    GpuObject object = load_object(push.object_index);

    // Motion in UV space, so a later reprojection is uv + motion. Both NDC
    // positions go through the same ndc_to_uv, which carries the Y flip.
    float2 current_uv  = ndc_to_uv(input.current.xy / input.current.w);
    float2 previous_uv = ndc_to_uv(input.previous.xy / input.previous.w);

    GBuffer output;
    output.albedo_ao = float4(object.albedo_roughness.rgb * input.colour, 1.0f);
    output.normal    = encode_normal(normalize(input.normal));
    output.material  = float2(object.albedo_roughness.a, object.emissive_metallic.a);
    output.motion    = previous_uv - current_uv;
    output.emissive  = float4(object.emissive_metallic.rgb, 1.0f);
    return output;
}

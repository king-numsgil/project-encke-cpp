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
//
// A textured object samples albedo, packed occlusion-roughness-metalness and,
// where it has them, a tangent-space normal and an emission map, and
// multiplies them by its factors, as glTF does. An untextured one is its
// factors alone, exactly as before textures.

import lib.bindless;
import lib.gpu_types;
import lib.morph;
import lib.normal;
import lib.screen;

struct VertexIn
{
    float3 position : POSITION;
    float3 normal   : NORMAL;
    float4 tangent  : TANGENT;    // xyz along +u, w bitangent sign
    float2 uv       : TEXCOORD;   // metres at unit scale
};

struct VertexOut
{
    float4 position  : SV_Position;
    float3 normal    : NORMAL;
    float4 tangent   : TANGENT;
    float2 uv        : TEXCOORD2;   // tile repeats
    float4 current   : TEXCOORD0;
    float4 previous  : TEXCOORD1;

    // The object index, which the draw's firstInstance carries. Flat, and
    // the same for every vertex of a draw.
    nointerpolation uint object : OBJECT;
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
VertexOut vertex_main(VertexIn input, uint instance : SV_VulkanInstanceID, uint vertex : SV_VulkanVertexID)
{
    // SV_VulkanInstanceID includes firstInstance; SV_InstanceID would not.
    GpuObject object  = load_object(instance);
    Morphed   morphed = morph_vertex(object, vertex, input.position, input.normal);
    float4    local   = float4(morphed.position, 1.0f);

    // Mesh UVs are metres at unit scale. Scaling by how much the object's
    // scale stretches the tangent and bitangent turns them into metres on
    // the object, and texture_scale already divides by the tile size.
    float3 bitangent = cross(input.normal, input.tangent.xyz) * input.tangent.w;
    float3 scale     = object.texture_scale.xyz;
    float2 stretch   = float2(length(scale * input.tangent.xyz),
                              length(scale * bitangent) * object.texture_scale.w);

    // Rasterised through the jittered projection; motion is measured
    // without the jitter, which is the camera's, not the scene's, so the
    // current position has it taken back out. Last frame's mvp never had it.
    float2 jitter = load_frame().taa.xy;

    VertexOut output;
    output.position   = mul(object.mvp, local);
    output.current    = output.position;
    output.current.xy -= jitter * output.current.w;
    output.previous = mul(object.prev_mvp, local);
    output.normal   = mul(object.normal_view, float4(morphed.normal, 0.0f)).xyz;
    output.tangent  = float4(mul(object.model_view, float4(input.tangent.xyz, 0.0f)).xyz,
                             input.tangent.w);
    output.uv       = input.uv * stretch;
    output.object   = instance;
    return output;
}

[shader("fragment")]
GBuffer fragment_main(VertexOut input)
{
    GpuObject object = load_object(input.object);

    // Motion in UV space, so a later reprojection is uv + motion. Both NDC
    // positions go through the same ndc_to_uv, which carries the Y flip.
    float2 current_uv  = ndc_to_uv(input.current.xy / input.current.w);
    float2 previous_uv = ndc_to_uv(input.previous.xy / input.previous.w);

    float3 albedo    = object.albedo_roughness.rgb;
    float  roughness = object.albedo_roughness.a;
    float  metallic  = object.emissive_metallic.a;
    float3 emissive  = object.emissive_metallic.rgb;
    float  ao        = 1.0f;
    float3 normal    = normalize(input.normal);

    // Uniform across one draw of the indirect batch, but not across the
    // batch: hence NonUniformResourceIndex on every handle.
    if (object.textures.x != kNoTexture)
    {
        SamplerState filter = g_samplers[NonUniformResourceIndex(push.material_sampler)];

        float4 colour = g_textures[NonUniformResourceIndex(object.textures.x)].Sample(filter, input.uv);
        float3 orm    = g_textures[NonUniformResourceIndex(object.textures.z)].Sample(filter, input.uv).rgb;

        albedo    *= colour.rgb;
        ao         = orm.r;
        roughness *= orm.g;
        metallic  *= orm.b;

        if (object.textures.y != kNoTexture)
        {
            float3 bumped =
                g_textures[NonUniformResourceIndex(object.textures.y)].Sample(filter, input.uv).xyz;

            // Tangent frame in view space, re-orthogonalised against the
            // interpolated normal. The bitangent is the top of the image,
            // which is where an OpenGL-convention normal map's green points.
            float3 tangent   = normalize(input.tangent.xyz - normal * dot(normal, input.tangent.xyz));
            float3 bitangent = cross(normal, tangent) * input.tangent.w;

            // Mips average normals to shorter vectors; normalising after
            // the decode undoes that.
            float3 perturbed = bumped * 2.0f - 1.0f;
            // Normal strength, glTF's normalTexture.scale. Fixed at 2 for
            // every material by eye; at 1 the maps read too flat under the
            // low sun.
            perturbed.xy *= 2.0f;
            normal = normalize(tangent * perturbed.x + bitangent * perturbed.y + normal * perturbed.z);
        }

        if (object.textures.w != kNoTexture)
        {
            emissive *= g_textures[NonUniformResourceIndex(object.textures.w)].Sample(filter, input.uv).rgb;
        }
    }

    GBuffer output;
    output.albedo_ao = float4(albedo, ao);
    output.normal    = encode_normal(normal);
    output.material  = float2(roughness, metallic);
    output.motion    = previous_uv - current_uv;
    output.emissive  = float4(emissive, 1.0f);
    return output;
}

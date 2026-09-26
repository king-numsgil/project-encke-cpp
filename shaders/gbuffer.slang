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

    // Terrain only: the ground's weights, rock, grass, snow, sand; gravel
    // is what they leave.
    float4 materials : TEXCOORD3;

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
    GpuObject object = load_object(instance);

    Morphed morphed;
    morphed.position = input.position;
    morphed.normal   = input.normal;
    float4 materials = 0.0f;
    if (is_terrain(object))
    {
        GpuTerrainVertex terrain = load_terrain_vertex(object, vertex);
        morphed   = morph_vertex(object, terrain, input.position, input.normal);
        materials = float4(float(terrain.materials & 0xFFu), float((terrain.materials >> 8u) & 0xFFu),
                           float((terrain.materials >> 16u) & 0xFFu), float(terrain.materials >> 24u)) /
                    255.0f;
    }
    float4 local = float4(morphed.position, 1.0f);

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
    output.uv        = input.uv * stretch;
    output.materials = materials;
    output.object    = instance;
    return output;
}

// A tangent-space normal map's sample applied to the interpolated normal.
// The tangent frame is re-orthogonalised against the normal; the bitangent
// is the top of the image, which is where an OpenGL-convention normal map's
// green points.
float3 bump(float3 normal, float4 tangent_in, float3 bumped)
{
    float3 tangent   = normalize(tangent_in.xyz - normal * dot(normal, tangent_in.xyz));
    float3 bitangent = cross(normal, tangent) * tangent_in.w;

    // Mips average normals to shorter vectors; normalising after the decode
    // undoes that.
    float3 perturbed = bumped * 2.0f - 1.0f;
    // Normal strength, glTF's normalTexture.scale. Fixed at 2 for every
    // material by eye; at 1 the maps read too flat under the low sun.
    perturbed.xy *= 2.0f;
    return normalize(tangent * perturbed.x + bitangent * perturbed.y + normal * perturbed.z);
}

struct Ground
{
    float3 albedo;
    float3 normal;
    float3 orm;
};

// One of the terrain palette's materials at `uv`, metres: its own tile, or
// its flat colour until its maps have landed. Which material a pixel
// samples varies across a quad, so implicit derivatives would be undefined:
// the caller takes the UV's once, in uniform control flow, and they are
// scaled to this material's tile.
Ground sample_ground(uint palette, uint index, float2 uv, float2 uv_dx, float2 uv_dy, float3 normal, float4 tangent,
                     SamplerState filter)
{
    GpuTerrainMaterial material = g_buffers[NonUniformResourceIndex(palette)]
                                      .Load<GpuTerrainMaterial>(index * kTerrainMaterialStride);
    Ground ground;
    ground.albedo = material.params.yzw;
    ground.normal = normal;
    ground.orm    = float3(1.0f, 1.0f, 0.0f);
    if (material.textures.x == kNoTexture)
    {
        return ground;
    }

    float  scale  = material.params.x;
    float2 at     = uv * scale;
    float2 dx     = uv_dx * scale;
    float2 dy     = uv_dy * scale;
    ground.albedo = g_textures[NonUniformResourceIndex(material.textures.x)].SampleGrad(filter, at, dx, dy).rgb;
    ground.orm    = g_textures[NonUniformResourceIndex(material.textures.z)].SampleGrad(filter, at, dx, dy).rgb;
    if (material.textures.y != kNoTexture)
    {
        ground.normal =
            bump(normal, tangent,
                 g_textures[NonUniformResourceIndex(material.textures.y)].SampleGrad(filter, at, dx, dy).xyz);
    }
    return ground;
}

// The terrain palette blended by the vertex's weights. Only the two
// heaviest materials are sampled, which is where nearly all the weight
// is: sampling all five would be fifteen texture reads a pixel.
Ground terrain_ground(float4 materials, float2 uv, float3 normal, float4 tangent, SamplerState filter)
{
    float weights[kTerrainMaterialCount] = {
        saturate(1.0f - materials.x - materials.y - materials.z - materials.w),
        materials.x, materials.y, materials.z, materials.w,
    };

    uint  first = 0u, second = 1u;
    if (weights[1] > weights[0])
    {
        first  = 1u;
        second = 0u;
    }
    for (uint index = 2u; index < kTerrainMaterialCount; ++index)
    {
        if (weights[index] > weights[first])
        {
            second = first;
            first  = index;
        }
        else if (weights[index] > weights[second])
        {
            second = index;
        }
    }

    float2 uv_dx   = ddx(uv);
    float2 uv_dy   = ddy(uv);
    uint   palette = load_frame().terrain.x;
    Ground a       = sample_ground(palette, first, uv, uv_dx, uv_dy, normal, tangent, filter);
    float  share   = weights[second] / max(weights[first] + weights[second], 1e-4f);
    if (share <= 0.0f)
    {
        return a;
    }
    Ground b = sample_ground(palette, second, uv, uv_dx, uv_dy, normal, tangent, filter);

    Ground blended;
    blended.albedo = lerp(a.albedo, b.albedo, share);
    blended.orm    = lerp(a.orm, b.orm, share);
    blended.normal = normalize(lerp(a.normal, b.normal, share));
    return blended;
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
    if ((object.morph_masks.z & 2u) != 0u)
    {
        // Terrain: the palette's materials, their maps as they are. The
        // chunk's own factors are all 1.
        SamplerState filter = g_samplers[NonUniformResourceIndex(push.material_sampler)];
        Ground       ground = terrain_ground(input.materials, input.uv, normal, input.tangent, filter);

        albedo    *= ground.albedo;
        ao         = ground.orm.r;
        roughness *= ground.orm.g;
        metallic  *= ground.orm.b;
        normal     = ground.normal;
    }
    else if (object.textures.x != kNoTexture)
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
            normal = bump(normal, input.tangent,
                          g_textures[NonUniformResourceIndex(object.textures.y)].Sample(filter, input.uv).xyz);
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

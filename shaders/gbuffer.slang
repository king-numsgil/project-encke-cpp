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

    // Terrain only: the morphed position in mesh space plus the chunk's
    // period offset, and the mesh-space normal, for the triplanar rock.
    float3 mesh_position : TEXCOORD4;
    float3 mesh_normal   : TEXCOORD5;

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

    // Palette terrain takes its cube-projected UVs from the morphed position,
    // as to_vertices lays them out, so a vertex pulled toward its target
    // does not drag its texture along. They differ from the mesh's own by
    // whole periods.
    if ((object.morph_masks.z & 2u) != 0u)
    {
        float3 p    = morphed.position + object.period_offset.xyz;
        uint   face = uint(object.period_offset.w);
        output.uv   = face == 0u ? p.zy : (face == 1u ? p.xz : p.xy);
    }
    output.materials     = materials;
    output.mesh_position = morphed.position + object.period_offset.xyz;
    output.mesh_normal   = morphed.normal;
    output.object        = instance;
    return output;
}

// A tangent-space normal map's sample applied to the interpolated normal.
// The tangent frame is re-orthogonalised against the normal; the bitangent
// is the top of the image, which is where an OpenGL-convention normal map's
// green points.
// `perturbed` is decoded, with the strength applied.
float3 bump_decoded(float3 normal, float4 tangent_in, float3 perturbed)
{
    float3 tangent   = normalize(tangent_in.xyz - normal * dot(normal, tangent_in.xyz));
    float3 bitangent = cross(normal, tangent) * tangent_in.w;

    // Mips average normals to shorter vectors; normalising undoes that.
    return normalize(tangent * perturbed.x + bitangent * perturbed.y + normal * perturbed.z);
}

float3 bump(float3 normal, float4 tangent_in, float3 bumped)
{
    float3 perturbed = bumped * 2.0f - 1.0f;
    // Normal strength, glTF's normalTexture.scale. Fixed at 2 for every
    // material by eye; at 1 the maps read too flat under the low sun.
    perturbed.xy *= 2.0f;
    return bump_decoded(normal, tangent_in, perturbed);
}

struct Ground
{
    float3 albedo;
    float3 normal;
    float3 orm;
};

// The palette's rock, which is projected along the mesh axes instead of
// the cube projection's UVs: it is what covers cliffs and overhangs, where
// a projection from the body's centre stretches.
static const uint kRock = 1u;

// The surface in mesh space, whose axes are the body's, for the triplanar
// projection. Derivatives are taken once, in uniform control flow.
struct MeshSurface
{
    float3   position;      // plus the chunk's period offset, metres
    float3   position_dx;
    float3   position_dy;
    float3   normal;        // unit
    float4x4 normal_view;   // mesh -> view, for normals
};

// One projection of a material: albedo, ORM, and the normal map's decoded
// tangent-space normal at bump()'s strength of 2, with its length before
// the strength, for Toksvig.
struct Projection
{
    float3 albedo;
    float3 orm;
    float3 normal;
    float  length;
};

Projection sample_projection(GpuTerrainMaterial material, float2 at, float2 dx, float2 dy, SamplerState filter)
{
    Projection projection;
    projection.albedo = g_textures[NonUniformResourceIndex(material.textures.x)].SampleGrad(filter, at, dx, dy).rgb;
    projection.orm    = g_textures[NonUniformResourceIndex(material.textures.z)].SampleGrad(filter, at, dx, dy).rgb;
    projection.normal = float3(0.0f, 0.0f, 1.0f);
    projection.length = 1.0f;
    if (material.textures.y != kNoTexture)
    {
        float3 decoded =
            g_textures[NonUniformResourceIndex(material.textures.y)].SampleGrad(filter, at, dx, dy).xyz * 2.0f -
            1.0f;
        projection.length = max(length(decoded), 1e-3f);
        projection.normal = float3(decoded.xy * 2.0f, decoded.z);
    }
    return projection;
}

// Rock's maps along the three mesh axes, blended by the normal's sharpened
// weights, with whiteout-blended normals after Ben Golus ("Normal Mapping
// for a Triplanar Shader", 2017). Each projection's u is mirrored on the
// negative side so the map's x keeps its sense; v is the image's down, so
// the map's green, toward the top, is negated.
Projection sample_triplanar(GpuTerrainMaterial material, MeshSurface surface, float scale, SamplerState filter)
{
    float3 n     = surface.normal;
    float3 sides = float3(n.x < 0.0f ? -1.0f : 1.0f, n.y < 0.0f ? -1.0f : 1.0f, n.z < 0.0f ? -1.0f : 1.0f);

    // Projections carrying little weight are skipped, the rest share it.
    float3 blend = pow(abs(n), 4.0f);
    blend       /= dot(blend, 1.0f);
    blend        = max(blend - 0.05f, 0.0f);
    blend       /= dot(blend, 1.0f);

    float3 p  = surface.position * scale;
    float3 dx = surface.position_dx * scale;
    float3 dy = surface.position_dy * scale;

    Projection result;
    result.albedo = 0.0f;
    result.orm    = 0.0f;
    result.normal = 0.0f;
    result.length = 0.0f;
    if (blend.x > 0.0f)
    {
        float2     flip = float2(sides.x, 1.0f);
        Projection x    = sample_projection(material, p.zy * flip, dx.zy * flip, dy.zy * flip, filter);
        float2     t    = x.normal.xy * float2(sides.x, -1.0f);
        result.albedo  += x.albedo * blend.x;
        result.orm     += x.orm * blend.x;
        result.normal  += float3(t + n.zy, abs(x.normal.z) * n.x).zyx * blend.x;
        result.length  += x.length * blend.x;
    }
    if (blend.y > 0.0f)
    {
        float2     flip = float2(sides.y, 1.0f);
        Projection y    = sample_projection(material, p.xz * flip, dx.xz * flip, dy.xz * flip, filter);
        float2     t    = y.normal.xy * float2(sides.y, -1.0f);
        result.albedo  += y.albedo * blend.y;
        result.orm     += y.orm * blend.y;
        result.normal  += float3(t + n.xz, abs(y.normal.z) * n.y).xzy * blend.y;
        result.length  += y.length * blend.y;
    }
    if (blend.z > 0.0f)
    {
        float2     flip = float2(-sides.z, 1.0f);
        Projection z    = sample_projection(material, p.xy * flip, dx.xy * flip, dy.xy * flip, filter);
        float2     t    = z.normal.xy * float2(-sides.z, -1.0f);
        result.albedo  += z.albedo * blend.z;
        result.orm     += z.orm * blend.z;
        result.normal  += float3(t + n.xy, abs(z.normal.z) * n.z) * blend.z;
        result.length  += z.length * blend.z;
    }
    result.normal = normalize(mul(surface.normal_view, float4(result.normal, 0.0f)).xyz);
    return result;
}

// One of the terrain palette's materials: its own tile, or its flat colour
// until its maps have landed. Rock is projected along the mesh axes, the
// rest at `uv`, metres. Which material a pixel samples varies across a
// quad, so implicit derivatives would be undefined: the caller takes them
// once, in uniform control flow, and they are scaled to this material's
// tile.
Ground sample_ground(uint palette, uint index, float2 uv, float2 uv_dx, float2 uv_dy, float3 normal, float4 tangent,
                     MeshSurface surface, SamplerState filter)
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

    float scale       = material.params.x;
    float bump_length = 1.0f;
    if (index == kRock)
    {
        Projection rock = sample_triplanar(material, surface, scale, filter);
        ground.albedo   = rock.albedo;
        ground.orm      = rock.orm;
        ground.normal   = rock.normal;
        bump_length     = rock.length;
    }
    else
    {
        Projection planar = sample_projection(material, uv * scale, uv_dx * scale, uv_dy * scale, filter);
        ground.albedo     = planar.albedo;
        ground.orm        = planar.orm;
        bump_length       = planar.length;
        if (material.textures.y != kNoTexture)
        {
            ground.normal = bump_decoded(normal, tangent, planar.normal);
        }
    }

    // The map's roughness into [floor, 1]: its variation kept, its gloss not.
    float floor     = material.surface.x;
    float roughness = floor + (1.0f - floor) * ground.orm.g;

    // Toksvig: a mip averages the map's normals to a shorter vector, and
    // how much shorter is how much bump the distance has blurred away. Put
    // back as roughness, so far ground does not turn glossy as its relief
    // flattens.
    float variance = saturate((1.0f - bump_length) / bump_length);
    float alpha    = roughness * roughness;
    ground.orm.g   = min(sqrt(sqrt(alpha * alpha + variance)), 1.0f);
    return ground;
}

// The terrain palette blended by the vertex's weights. Only the two
// heaviest materials are sampled, which is where nearly all the weight
// is: sampling all five would be fifteen texture reads a pixel. The second
// is dropped where its share is small, remapped so the blend stays
// continuous.
Ground terrain_ground(float4 materials, float2 uv, float3 normal, float4 tangent, MeshSurface surface,
                      SamplerState filter)
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
    Ground a       = sample_ground(palette, first, uv, uv_dx, uv_dy, normal, tangent, surface, filter);

    // Between 0 and a half; below kMinShare the second is not sampled.
    static const float kMinShare = 0.05f;
    float share = weights[second] / max(weights[first] + weights[second], 1e-4f);
    share       = saturate((share - kMinShare) / (0.5f - kMinShare)) * 0.5f;
    if (share <= 0.0f)
    {
        return a;
    }
    Ground b = sample_ground(palette, second, uv, uv_dx, uv_dy, normal, tangent, surface, filter);

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
        MeshSurface  surface;
        surface.position    = input.mesh_position;
        surface.position_dx = ddx(input.mesh_position);
        surface.position_dy = ddy(input.mesh_position);
        surface.normal      = normalize(input.mesh_normal);
        surface.normal_view = object.normal_view;
        Ground ground = terrain_ground(input.materials, input.uv, normal, input.tangent, surface, filter);

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

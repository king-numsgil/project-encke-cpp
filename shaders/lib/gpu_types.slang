// Mirrors src/render/gpu_types.hpp member for member. Change one side, change
// the other.
//
// Matrices are float4x4 read from GLM's column-major memory, which
// -matrix-layout-column-major makes correct for ByteAddressBuffer Load<T> too.
// Two traps: M * v is mul(m, v), since * is component-wise; and indexing is
// row-first, so GLM's m[c][r] is m[r][c] here.
//
// Everything here is view space.

module gpu_types;

public struct GpuFrame
{
    public float4x4 projection;
    public float4   screen;          // xy size in px, zw 1/size
    public float4   cluster_depth;   // x near, y far, z log scale, w log bias
    public uint4    cluster_grid;    // xyz dims, w max lights per cluster
    public float4   sun_direction;   // view space, toward the sun
    public float4   sun_radiance;
    public float4   env_up;          // view space, unit: away from the ground
    public float4   env_sky;         // linear rgb radiance above the horizon
    public float4   env_ground;      // linear rgb radiance below it
    public uint4    counts;          // x light count, y cascade count, z max objects
    public float4   shadow;          // x shadow distance, y fade start, z normal offset (texels)
    public float4   exposure_range;  // x log2 min luminance, y log2 range, z low, w high percentile
    public float4   exposure_adapt;  // x frame seconds, y speed brighter, z darker, w compensation EV
    public float4   exposure_limits; // x min EV100, y max EV100, z 1 = jump to target, w EV100 to start from
    public uint4    atmosphere;      // x GpuAtmosphere buffer or kNoAtmosphere, y transmittance LUT,
                                     // z multiple-scattering LUT (sampled), w their sampler
    public float4   taa;             // xy jitter in NDC (in projection too), z 1 = reset history,
                                     // w exposure before anything was metered
    public uint4    taa_images;      // x history to read (sampled), y to write (storage),
                                     // z EV100 image (sampled) or ~0, w linear clamped sampler
    public float4x4 sky_reprojection; // view direction now -> last frame's clip, rotation only
    public float4   post;            // x CAS sharpness in tonemap, 0 skips it
    public uint4    terrain;         // x GpuTerrainMaterial buffer or ~0
};

// Gravel, rock, grass, snow, sand. See TerrainMaterial in render/gpu_types.hpp.
public struct GpuTerrainMaterial
{
    public uint4  textures;          // x albedo, y normal, z ORM; x kNoTexture until all have landed
    public float4 params;            // x 1 / tile width (m), yzw linear albedo when untextured
    public float4 surface;           // x roughness floor: the map's is remapped into [x, 1]
};

public static const uint kTerrainMaterialCount  = 5;
public static const uint kTerrainMaterialStride = 48;

public static const uint kNoAtmosphere = 0xFFFFFFFFu;

// Kilometres. See Atmosphere in render/gpu_types.hpp.
public struct GpuAtmosphere
{
    public float4 planet;            // xyz body centre in view space, w radius
    public float4 shell;             // x top radius, y camera altitude, z cos sun radius, w sun solid angle
    public float4 rayleigh;          // rgb scattering per km, w scale height
    public float4 mie;               // rgb scattering per km, w scale height
    public float4 mie_absorption;    // rgb per km, w phase g
    public float4 ozone;             // rgb absorption per km, w tent apex altitude
    public float4 ground;            // rgb body albedo, w tent half-width
    public float4 surroundings;      // rgb nearest body's albedo, w its sky fill
    public uint4  handles;           // x transmittance, y multiple scattering (storage), z ambient buffer,
                                     // w sky-view LUT (storage)
    public uint4  sky_view;          // x sky-view LUT (sampled), y 1 when built this frame
};

// A point light is a spot whose cone never cuts off: cos_outer below -1.
public struct GpuLight
{
    public float4 position_radius;     // view-space xyz, w radius (m)
    public float4 colour_intensity;    // linear rgb, w intensity (cd)
    public float4 direction_cos_outer; // view-space unit axis, w cos(outer half-angle)
    public float  cos_inner;
    public uint   shadow;              // shadow view index, or kNoShadow
    public uint   pad0;
    public uint   pad1;
};

public static const uint kNoShadow = 0xFFFFFFFFu;

public struct GpuShadowView
{
    public float4x4 view_to_clip;
    public float4   params;   // x cascade far, y texel metres (per metre for spots), z strength, w perspective
    public uint4    image;    // x bindless sampled image
};

// Material values are glTF-style factors: with textures they multiply what is
// sampled, without they are the material.
public struct GpuObject
{
    public float4x4 mvp;
    public float4x4 prev_mvp;
    public float4x4 model_view;        // 3x3 used, for tangents
    public float4x4 normal_view;
    public float4   albedo_roughness;
    public float4   emissive_metallic;
    public uint4    textures;          // x albedo, y normal, z ORM, w emission; x kNoTexture if
                                       // untextured, y and w kNoTexture when that map is absent
    public float4   texture_scale;     // xyz scale / tile width (m), w tile width / height
    public float4   morph;             // x start distance, y 1 / (end - start), z extent, w voxel (m)
    public uint4    morph_masks;       // terrain: x coarser neighbours, y finer, z flags (1 morph,
                                       // 2 terrain palette, 0 not terrain), w TerrainVertex buffer
    public float4   period_offset;     // terrain: xyz added to mesh positions for triplanar
                                       // and cube-projected UVs, w the cube face (0 x, 1 y, 2 z)
};

// See TerrainVertex in render/mesh.hpp.
public struct GpuTerrainVertex
{
    public float3 morph_position;
    public uint   morph_normal;        // octahedral, 16 bits a component, x low
    public uint   materials;           // bytes from the lowest: rock, grass, snow, sand
};

public static const uint kNoTexture = 0xFFFFFFFFu;

public struct Push
{
    public uint frame;
    public uint objects;
    public uint lights;
    public uint cluster_counts;

    public uint cluster_lights;
    public uint gbuffer_albedo;
    public uint gbuffer_normal;
    public uint gbuffer_material;

    public uint gbuffer_depth;
    public uint hdr_storage;
    public uint hdr_sampled;
    public uint pad0;              // keeps ui_transform 16-byte aligned

    public float exposure;
    public uint  debug_view;
    public uint  gbuffer_motion;
    public uint  debug_target;     // storage image a debug_views dispatch writes

    // UI overlay. Pixel -> NDC is xy * scale + translate.
    public float4 ui_transform;    // xy scale, zw translate
    public uint   ui_texture;      // bindless sampled image
    public uint   ui_sampler;      // bindless sampler
    public uint   ui_encode_srgb;  // nonzero: target is UNORM, encode in-shader
    public float  debug_gain;      // motion view magnification

    public uint shadow_views;      // GpuShadowView per map, cascades first
    public uint shadow_matrices;   // float4x4 per (shadow view, object), at
                                   // view * max objects + object: the shadow pass's instance
    public uint shadow_sampler;    // comparison sampler
    public uint exposure_image;    // 1x1 R32F adapted EV100: storage handle in the exposure
                                   // pass, sampled in tonemap, where ~0 means use push.exposure

    // Only the exposure passes read these.
    public uint exposure_histogram; // u32 per bin, writable

    public uint material_sampler;   // trilinear anisotropic repeat, every material texture

    public uint tonemap;            // curve, as Tonemap in render/renderer.hpp
};

// Byte strides must match sizeof() on the C++ side.
public static const uint kFrameStride      = 416;
public static const uint kLightStride      = 64;
public static const uint kShadowViewStride = 96;
public static const uint kObjectStride     = 368;
public static const uint kTerrainVertexStride = 20;
public static const uint kMatrixStride     = 64;

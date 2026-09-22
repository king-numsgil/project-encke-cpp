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
    public float4   ambient;
    public uint4    counts;          // x light count, y cascade count
    public float4   shadow;          // x shadow distance, y fade start, z normal offset (texels)
    public float4   exposure_range;  // x log2 min luminance, y log2 range, z low, w high percentile
    public float4   exposure_adapt;  // x frame seconds, y speed brighter, z darker, w compensation EV
    public float4   exposure_limits; // x min EV100, y max EV100, z 1 = jump to target, w EV100 to start from
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

public struct GpuObject
{
    public float4x4 mvp;
    public float4x4 prev_mvp;
    public float4x4 normal_view;
    public float4   albedo_roughness;
    public float4   emissive_metallic;
};

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
    public uint object_index;

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
    public uint shadow_matrices;   // float4x4 per (shadow view, object); the shadow
                                   // pass indexes it with object_index directly
    public uint shadow_sampler;    // comparison sampler
    public uint exposure_image;    // 1x1 R32F adapted EV100: storage handle in the exposure
                                   // pass, sampled in tonemap, where ~0 means use push.exposure

    // Only the exposure passes read these.
    public uint exposure_histogram; // u32 per bin, writable
};

// Byte strides must match sizeof() on the C++ side.
public static const uint kFrameStride      = 240;
public static const uint kLightStride      = 64;
public static const uint kShadowViewStride = 96;
public static const uint kObjectStride     = 224;
public static const uint kMatrixStride     = 64;

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
    public uint4    counts;          // x light count
};

public struct GpuLight
{
    public float4 position_radius;   // view-space xyz, w radius (m)
    public float4 colour_intensity;  // linear rgb, w intensity (cd)
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
};

// Byte strides must match sizeof() on the C++ side.
public static const uint kFrameStride  = 176;
public static const uint kLightStride  = 32;
public static const uint kObjectStride = 224;

// Mirrors src/render/gpu_types.hpp member for member. Change one side, change
// the other.
//
// Matrices arrive as four float4 COLUMNS, matching GLM's column-major memory
// order, and are applied with transform() below rather than a float4x4. That
// keeps them immune to whichever matrix-layout convention ByteAddressBuffer
// Load<T> would otherwise apply.
//
// Everything here is view space.

module gpu_types;

public struct Mat4
{
    public float4 c0;
    public float4 c1;
    public float4 c2;
    public float4 c3;
};

// Column-major M * v, spelled out.
public float4 transform(Mat4 m, float4 v)
{
    return m.c0 * v.x + m.c1 * v.y + m.c2 * v.z + m.c3 * v.w;
}

public struct GpuFrame
{
    public Mat4   projection;
    public float4 screen;          // xy size in px, zw 1/size
    public float4 cluster_depth;   // x near, y far, z log scale, w log bias
    public uint4  cluster_grid;    // xyz dims, w max lights per cluster
    public float4 sun_direction;   // view space, toward the sun
    public float4 sun_radiance;
    public float4 ambient;
    public uint4  counts;          // x light count
};

public struct GpuLight
{
    public float4 position_radius;   // view-space xyz, w radius (m)
    public float4 colour_intensity;  // linear rgb, w intensity (cd)
};

public struct GpuObject
{
    public Mat4   mvp;
    public Mat4   prev_mvp;
    public Mat4   normal_view;
    public float4 albedo_roughness;
    public float4 emissive_metallic;
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

#pragma once

// Mirrored member-for-member in shaders/lib/gpu_types.slang. Change one side,
// change the other; the static_asserts pin sizes, not meaning.
//
// Every member is 16 bytes or a multiple of it, so C++ packing, std430, scalar
// layout and Slang's ByteAddressBuffer Load<T> all agree with no padding rules
// in play. Matrices are written as GLM's column-major f32mat4, which in memory
// is exactly four consecutive columns -- the shader side reads them as four
// float4 columns and multiplies by hand, so no matrix-layout flag can
// transpose them.
//
// Everything is in VIEW space. World positions are f64 on the CPU and are
// composed through the view matrix in f64 before being narrowed, so the GPU
// never sees a large coordinate. See render/camera.hpp.

namespace encke::gpu
{
    struct Frame
    {
        f32mat4 projection;          // view -> clip, infinite reversed-Z
        f32vec4 screen;              // xy size in px, zw 1/size
        f32vec4 cluster_depth;       // x near, y far, z log scale, w log bias
        u32vec4 cluster_grid;        // xyz grid dims, w max lights per cluster
        f32vec4 sun_direction;       // view space, pointing toward the sun
        f32vec4 sun_radiance;        // linear rgb
        f32vec4 ambient;             // linear rgb
        u32vec4 counts;              // x light count
    };

    struct Light
    {
        f32vec4 position_radius;     // view-space xyz, w influence radius (m)
        f32vec4 colour_intensity;    // linear rgb, w luminous intensity (cd)
    };

    struct Object
    {
        f32mat4 mvp;
        f32mat4 prev_mvp;            // for motion vectors
        f32mat4 normal_view;         // inverse-transpose(view * model); 3x3 used
        f32vec4 albedo_roughness;    // linear rgb, a roughness
        f32vec4 emissive_metallic;   // linear HDR rgb, a metallic
    };

    // One struct for every pass; each pass reads the fields it needs. All
    // pipelines share one push range, so this is pushed once per pass and the
    // object index patched per draw.
    struct Push
    {
        u32 frame;
        u32 objects;
        u32 lights;
        u32 cluster_counts;

        u32 cluster_lights;
        u32 gbuffer_albedo;
        u32 gbuffer_normal;
        u32 gbuffer_material;

        u32 gbuffer_depth;
        u32 hdr_storage;
        u32 hdr_sampled;
        u32 object_index;

        f32 exposure;
        u32 debug_view;
        u32 gbuffer_motion;
        u32 pad0;
    };

    static_assert(sizeof(Frame) == 176);
    static_assert(sizeof(Light) == 32);
    static_assert(sizeof(Object) == 224);
    static_assert(sizeof(Push) == 64, "must fit the 128-byte guaranteed minimum");
}

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
        f32vec4 sun_radiance;        // linear rgb, illuminance (lux) folded in
        // The environment, standing in for everything but the lights: sky
        // above the horizon, ground below. Filled and reflected by lighting.
        f32vec4 env_up;              // view space, unit: away from the ground
        f32vec4 env_sky;             // linear rgb radiance above the horizon
        f32vec4 env_ground;          // linear rgb radiance below it
        u32vec4 counts;             // x light count, y cascade count, z max objects
        f32vec4 shadow;              // x shadow distance, y fade start, z normal offset (texels)

        // Auto-exposure; see render/config.hpp for what each one means.
        f32vec4 exposure_range;      // x log2 min luminance, y log2 range, z low, w high percentile
        f32vec4 exposure_adapt;      // x frame seconds, y speed brighter, z darker, w compensation EV
        f32vec4 exposure_limits;     // x min EV100, y max EV100, z 1 = jump to target,
                                     // w EV100 to start from when jumping

        // x the Atmosphere buffer, or kNoAtmosphere when the camera sees
        // none; y the transmittance LUT, z the multiple-scattering LUT, both
        // sampled; w their sampler, linear and clamped.
        u32vec4 atmosphere;
    };

    inline constexpr u32 kNoAtmosphere = ~0u;

    // The atmosphere the camera sees (encke::Atmosphere), in kilometres,
    // which keeps squared planet radii inside f32's range with digits to
    // spare. The camera's altitude comes from f64: its distance from the
    // centre, a large number, differenced in f32 would be off by metres.
    struct Atmosphere
    {
        f32vec4 planet;              // xyz the body's centre in view space, w its radius
        f32vec4 shell;               // x top radius, y camera altitude above the radius,
                                     // z cos of the sun's angular radius, w its disk's solid angle (sr)
        f32vec4 rayleigh;            // rgb scattering per km, w scale height
        f32vec4 mie;                 // rgb scattering per km, w scale height
        f32vec4 mie_absorption;      // rgb per km, w phase asymmetry g
        f32vec4 ozone;               // rgb absorption per km, w altitude of the tent's apex
        f32vec4 ground;              // rgb the body's albedo, w half the ozone tent's base
        f32vec4 surroundings;        // rgb the nearest body's albedo, w its sky fill
        u32vec4 handles;             // x transmittance, y multiple scattering, storage images;
                                     // z the ambient buffer, writable: sky, then ground radiance;
                                     // w the sky-view LUT, storage
        u32vec4 sky_view;            // x the sky-view LUT, sampled; y 1 when the camera is in
                                     // the air and the LUT was built this frame
    };

    // Point and spot lights share one struct and one clustered path. A point
    // light is a spot whose cone never cuts off: cos_outer below -1.
    struct Light
    {
        f32vec4 position_radius;     // view-space xyz, w influence radius (m)
        f32vec4 colour_intensity;    // linear rgb, w luminous intensity (cd)
        f32vec4 direction_cos_outer; // view-space unit axis, w cos(outer half-angle)
        f32     cos_inner;           // cos(inner half-angle); full intensity inside
        u32     shadow;              // index into the shadow views, or kNoShadow
        u32     pad0;
        u32     pad1;
    };

    inline constexpr u32 kNoShadow = ~0u;

    // One shadow map: a sun cascade (orthographic) or a spot (perspective).
    // The lighting pass takes a view-space position straight to the map's clip
    // space with view_to_clip; the world never appears.
    struct ShadowView
    {
        f32mat4 view_to_clip;
        // x: cascade far view distance (unused for spots)
        // y: texel footprint in metres; for a spot, per metre from the light
        // z: strength, 0 to 1, for fading out at the range limit
        // w: 1 for perspective (spot), 0 for orthographic (cascade)
        f32vec4 params;
        u32vec4 image;               // x bindless sampled image
    };

    // Material values are glTF-style factors: with textures they multiply
    // what is sampled, without they are the material.
    struct Object
    {
        f32mat4 mvp;
        f32mat4 prev_mvp;            // for motion vectors
        f32mat4 model_view;          // view * model; 3x3 used, for tangents
        f32mat4 normal_view;         // inverse-transpose(view * model); 3x3 used
        f32vec4 albedo_roughness;    // linear rgb, a roughness
        f32vec4 emissive_metallic;   // linear HDR rgb, a metallic

        // Bindless sampled images: x albedo, y normal, z occlusion-
        // roughness-metalness, w emission. x is kNoTexture on an untextured
        // object, and then none of the others is read. On a textured one, y
        // and w may still be kNoTexture, and are then skipped.
        u32vec4 textures;

        // xyz the object's scale divided by the tile's width in metres; w the
        // tile's width over its height. Stretches mesh UVs, which are metres
        // at unit scale, into tile repeats.
        f32vec4 texture_scale;

        // Geomorph, as encke::Geomorph: x start distance, y 1 / (end -
        // start), z chunk extent, w voxel, all metres.
        f32vec4 morph;
        // x coarser neighbours, y finer, z nonzero when the object morphs,
        // w the bindless MorphTarget buffer.
        u32vec4 morph_masks;
    };

    inline constexpr u32 kNoTexture = ~0u;

    // One struct for every pass; each pass reads the fields it needs. All
    // pipelines share one push range, so this is pushed once per pass. Draws
    // carry their object index as firstInstance, not in here.
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
        u32 pad0;                    // keeps ui_transform 16-byte aligned

        f32 exposure;
        u32 debug_view;
        u32 gbuffer_motion;
        u32 debug_target;            // storage image a debug_views dispatch writes

        // UI overlay (ui/imgui_vulkan). Pixel -> NDC is xy * scale + translate.
        f32vec4 ui_transform;        // xy scale, zw translate
        u32     ui_texture;          // bindless sampled image
        u32     ui_sampler;          // bindless sampler
        u32     ui_encode_srgb;      // nonzero: target is UNORM, encode in-shader
        f32     debug_gain;          // motion view magnification

        u32 shadow_views;            // ShadowView per map, cascades first
        u32 shadow_matrices;         // f32mat4 per (shadow view, object): object -> map clip,
                                     // at view * kMaxObjects + object, which the shadow
                                     // pass's firstInstance carries.
        u32 shadow_sampler;          // comparison sampler

        // 1x1 R32F holding the adapted EV100. Patched per pass like
        // debug_target: its storage handle for the exposure pass, its sampled
        // handle for tonemap. kInvalid in tonemap means use `exposure` above,
        // the scene's fixed value.
        u32 exposure_image;

        // Only the exposure passes read this.
        u32 exposure_histogram;      // u32 per bin, writable

        // Trilinear anisotropic repeat, for every material texture.
        u32 material_sampler;

        // Tonemap curve, as encke::Tonemap. Only tonemap reads it.
        u32 tonemap;
    };

    static_assert(sizeof(Frame) == 288);
    static_assert(sizeof(Atmosphere) == 160);
    static_assert(sizeof(Light) == 64);
    static_assert(sizeof(ShadowView) == 96);
    static_assert(sizeof(Object) == 352);
    static_assert(sizeof(Push) == 124,"must fit the 128-byte guaranteed minimum");
}

// Deferred lighting. One thread per pixel: rebuild the view-space position
// from depth, find the pixel's cluster, shade against that cluster's lights,
// and add the result onto the emissive already in the HDR target.
//
// debug_view:
//   0  clustered lighting
//   1  brute force -- every light, no clusters. Must match 0; any difference
//      is a cluster assignment bug.
//   2  lights per cluster, as a heat map
//   3  view-space normals
//   4  motion vectors, magnified

import lib.bindless;
import lib.cluster;
import lib.colour;
import lib.gpu_types;
import lib.normal;
import lib.pbr;
import lib.screen;

float3 shade_light(Surface surface, float3 position, GpuLight light)
{
    float3 to_light = light.position_radius.xyz - position;
    float  distance = length(to_light);
    float  radius   = light.position_radius.w;
    if (distance >= radius)
    {
        return 0.0f;
    }

    float3 radiance = light.colour_intensity.rgb * light.colour_intensity.w *
                      point_attenuation(distance, radius);
    return shade(surface, to_light / distance, radiance);
}

float3 heat(float t)
{
    // Black -> blue -> green -> yellow -> red.
    t = saturate(t);
    return saturate(float3(1.5f * t - 0.25f, 1.5f - abs(2.0f * t - 1.0f) * 1.5f,
                           1.0f - 2.0f * t));
}

[shader("compute")]
[numthreads(8, 8, 1)]
void compute_main(uint3 id : SV_DispatchThreadID)
{
    GpuFrame frame = load_frame();

    uint2 pixel = id.xy;
    if (any(float2(pixel) >= frame.screen.xy))
    {
        return;
    }

    RWTexture2D<float4> hdr   = g_images[NonUniformResourceIndex(push.hdr_storage)];
    int3                texel = int3(pixel, 0);

    float depth = g_textures[NonUniformResourceIndex(push.gbuffer_depth)].Load(texel).r;

    // Reversed-Z clears to 0.0, which with an infinite far plane is exactly
    // "nothing was drawn here". Leave the HDR value, which is the clear colour.
    if (depth <= 0.0f)
    {
        return;
    }

    // Infinite reversed-Z maps view distance d to depth = near / d.
    float  distance = frame.cluster_depth.x / depth;
    float2 ndc      = fragment_to_ndc(float2(pixel) + 0.5f, frame.screen.xy);
    float3 position = view_at(ndc, distance, frame);

    float4 albedo_ao = g_textures[NonUniformResourceIndex(push.gbuffer_albedo)].Load(texel);
    float2 encoded   = g_textures[NonUniformResourceIndex(push.gbuffer_normal)].Load(texel).rg;
    float2 material  = g_textures[NonUniformResourceIndex(push.gbuffer_material)].Load(texel).rg;

    Surface surface;
    surface.albedo    = albedo_ao.rgb;
    surface.ao        = albedo_ao.a;
    surface.normal    = decode_normal(encoded);
    surface.to_eye    = normalize(-position);
    surface.roughness = material.r;
    surface.metallic  = material.g;

    uint cluster = cluster_of(pixel, distance, frame);

    if (push.debug_view == 2u)
    {
        uint count = g_rw_buffers[NonUniformResourceIndex(push.cluster_counts)].Load(cluster * 4u);
        hdr[pixel] = float4(heat(float(count) / 16.0f), 1.0f);
        return;
    }
    if (push.debug_view == 3u)
    {
        hdr[pixel] = float4(surface.normal * 0.5f + 0.5f, 1.0f);
        return;
    }
    if (push.debug_view == 4u)
    {
        // Signed, centred on grey: static geometry under a static camera is
        // exactly 0.5 grey.
        //
        // Motion is per-FRAME UV displacement, so the gain that makes it
        // legible depends entirely on the frame rate. Uncapped at a couple of
        // thousand frames a second it is around 5e-5 UV -- a twentieth of a
        // pixel. This gain suits that; drop it by the same factor once there
        // is a frame limiter.
        float2 motion = g_textures[NonUniformResourceIndex(push.gbuffer_motion)].Load(texel).rg;
        hdr[pixel] = float4(saturate(motion * 5000.0f + 0.5f), 0.5f, 1.0f);
        return;
    }

    float3 lit = shade(surface, frame.sun_direction.xyz, frame.sun_radiance.rgb);
    lit += frame.ambient.rgb * surface.albedo * surface.ao;

    if (push.debug_view == 1u)
    {
        for (uint light = 0; light < frame.counts.x; ++light)
        {
            lit += shade_light(surface, position, load_light(light));
        }
    }
    else
    {
        uint max_lights = frame.cluster_grid.w;
        uint count = g_rw_buffers[NonUniformResourceIndex(push.cluster_counts)].Load(cluster * 4u);
        RWByteAddressBuffer lists = g_rw_buffers[NonUniformResourceIndex(push.cluster_lights)];

        for (uint i = 0; i < count; ++i)
        {
            uint light = lists.Load((cluster * max_lights + i) * 4u);
            lit += shade_light(surface, position, load_light(light));
        }
    }

    hdr[pixel] = float4(hdr[pixel].rgb + lit, 1.0f);
}

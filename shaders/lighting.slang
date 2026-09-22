// Deferred lighting. One thread per pixel: rebuild the view-space position
// from depth, find the pixel's cluster, shade against that cluster's lights,
// and add the result onto the emissive already in the HDR target. The sun is
// shadowed through its cascades, and a spot through its own map when the CPU
// picked it as one of the shadowed few.
//
// debug_view:
//   0  clustered lighting
//   1  brute force -- every light, no clusters. Must match 0; any difference
//      is a cluster assignment bug.
//
// The visualisations (cluster heat, normals, motion) are not modes of this
// pass; they are drawn into their own images by debug_views.slang.

import lib.bindless;
import lib.cluster;
import lib.gpu_types;
import lib.normal;
import lib.pbr;
import lib.screen;
import lib.shadow;

float3 shade_light(Surface surface, float3 position, GpuLight light, GpuFrame frame)
{
    float3 to_light = light.position_radius.xyz - position;
    float  distance = length(to_light);
    float  radius   = light.position_radius.w;
    if (distance >= radius)
    {
        return 0.0f;
    }

    float3 direction = to_light / distance;
    float  cone      = spot_cone(-direction, light.direction_cos_outer.xyz,
                                 light.direction_cos_outer.w, light.cos_inner);
    if (cone <= 0.0f || dot(surface.normal, direction) <= 0.0f)
    {
        return 0.0f;
    }

    float shadow = light.shadow == kNoShadow
                       ? 1.0f
                       : spot_visibility(light.shadow, position, surface.normal, frame);

    float3 radiance = light.colour_intensity.rgb * light.colour_intensity.w *
                      point_attenuation(distance, radius) * cone * shadow;
    return shade(surface, direction, radiance);
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

    float  distance = distance_from_depth(depth, frame);
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

    float3 lit = frame.ambient.rgb * surface.albedo * surface.ao;

    float3 sun = frame.sun_direction.xyz;
    if (any(frame.sun_radiance.rgb > 0.0f) && dot(surface.normal, sun) > 0.0f)
    {
        lit += shade(surface, sun, frame.sun_radiance.rgb) *
               sun_visibility(position, surface.normal, frame);
    }

    if (push.debug_view == 1u)
    {
        for (uint light = 0; light < frame.counts.x; ++light)
        {
            lit += shade_light(surface, position, load_light(light), frame);
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
            lit += shade_light(surface, position, load_light(light), frame);
        }
    }

    hdr[pixel] = float4(hdr[pixel].rgb + lit, 1.0f);
}

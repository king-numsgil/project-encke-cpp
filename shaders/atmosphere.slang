// The atmosphere's passes; the model is lib/atmosphere.slang.
//
//   transmittance_main  LUT: transmittance to the top by altitude and zenith
//   multiscatter_main   LUT: every order of scattering above the first
//   sky_view_main       LUT: the sky around the camera, while it is in the air
//   ambient_main        one group: the sky and ground radiance the lighting
//                       pass's environment uses, around the camera
//   apply_main          per pixel, after lighting: the sky where nothing was
//                       drawn, and aerial perspective over what was
//
// The first two LUTs depend on the atmosphere alone and are rebuilt only
// when it changes; the rest run every frame.

import lib.atmosphere;
import lib.bindless;
import lib.cluster;
import lib.gpu_types;
import lib.screen;

[shader("compute")]
[numthreads(8, 8, 1)]
void transmittance_main(uint3 id : SV_DispatchThreadID)
{
    GpuFrame            frame = load_frame();
    GpuAtmosphere       a     = load_atmosphere(frame);
    RWTexture2D<float4> lut   = g_images[NonUniformResourceIndex(a.handles.x)];

    uint width, height;
    lut.GetDimensions(width, height);
    if (id.x >= width || id.y >= height)
    {
        return;
    }

    float altitude, mu;
    transmittance_parameters(a, (float2(id.xy) + 0.5f) / float2(width, height), altitude, mu);
    float r = bottom_radius(a) + altitude;

    float2 shell = sphere_hits(r * mu, squared_excess(altitude - thickness(a), top_radius(a)));
    float  dt    = max(shell.y, 0.0f) / 40.0f;
    float3 depth = 0.0f;
    for (uint step = 0; step < 40u; ++step)
    {
        float t     = (float(step) + 0.5f) * dt;
        float above = altitude + (t * t + 2.0f * r * mu * t) / (sqrt(r * r + t * t + 2.0f * r * mu * t) + r);
        depth += medium_at(a, above).extinction * dt;
    }
    lut[id.xy] = float4(exp(-depth), 1.0f);
}

// Hillaire's section 5.5: from each texel's altitude, light arriving from
// every direction once scattered (isotropically, under unit illuminance,
// including off the ground), and the fraction f_ms of it that scatters
// again; every further order is a geometric series, 1 / (1 - f_ms).
[shader("compute")]
[numthreads(8, 8, 1)]
void multiscatter_main(uint3 id : SV_DispatchThreadID)
{
    GpuFrame            frame = load_frame();
    GpuAtmosphere       a     = load_atmosphere(frame);
    RWTexture2D<float4> lut   = g_images[NonUniformResourceIndex(a.handles.y)];

    uint width, height;
    lut.GetDimensions(width, height);
    if (id.x >= width || id.y >= height)
    {
        return;
    }

    float2 uv       = (float2(id.xy) + 0.5f) / float2(width, height);
    float  mu_sun   = uv.x * 2.0f - 1.0f;
    float  altitude = max(uv.y * thickness(a), 0.01f);
    float  r        = bottom_radius(a) + altitude;
    float3 position = float3(0.0f, r, 0.0f);
    float3 sun      = float3(0.0f, mu_sun, sqrt(max(1.0f - mu_sun * mu_sun, 0.0f)));

    static const uint  kSide      = 8u;
    static const uint  kSteps     = 20u;
    static const float kIsotropic = 1.0f / (4.0f * kPi);

    float3 luminance = 0.0f;
    float3 again     = 0.0f;
    for (uint j = 0; j < kSide; ++j)
    {
        for (uint i = 0; i < kSide; ++i)
        {
            float  phi       = 2.0f * kPi * (float(i) + 0.5f) / float(kSide);
            float  cos_theta = 1.0f - 2.0f * (float(j) + 0.5f) / float(kSide);
            float  sin_theta = sqrt(max(1.0f - cos_theta * cos_theta, 0.0f));
            float3 direction = float3(sin_theta * cos(phi), cos_theta, sin_theta * sin(phi));

            float  b      = dot(position, direction);
            float2 shell  = sphere_hits(b, squared_excess(altitude - thickness(a), top_radius(a)));
            float2 ground = sphere_hits(b, squared_excess(altitude, bottom_radius(a)));
            bool   hits   = ground.x > 0.0f;
            float  end    = hits ? ground.x : max(shell.y, 0.0f);
            float  dt     = end / float(kSteps);

            float3 through_all = 1.0f;
            for (uint step = 0; step < kSteps; ++step)
            {
                float  t     = (float(step) + 0.3f) * dt;
                float3 p     = position + t * direction;
                float  above = altitude + (2.0f * b * t + t * t) / (length(p) + r);

                Medium medium     = medium_at(a, above);
                float3 extinction = max(medium.extinction, 1e-7f);
                float3 sunlit     = transmittance_toward(frame, a, p, above, sun);
                float3 emitted    = sunlit * medium.scattering * kIsotropic;
                float3 through    = exp(-extinction * dt);

                luminance   += through_all * (emitted - emitted * through) / extinction;
                again       += through_all * (medium.scattering - medium.scattering * through) / extinction;
                through_all *= through;
            }

            if (hits)
            {
                float3 at    = position + end * direction;
                float  up    = dot(normalize(at), sun);
                luminance += through_all * transmittance_lut(frame, a, 0.0f, up) * saturate(up) *
                             a.ground.rgb / kPi;
            }
        }
    }

    float  count = float(kSide * kSide);
    float3 f_ms  = again / count;
    lut[id.xy]   = float4(luminance / count / max(1.0f - f_ms, 1e-3f), 1.0f);
}

// The sky from the camera, while it is in the air: one march per texel,
// where marching per sky pixel was most of the atmosphere's cost.
[shader("compute")]
[numthreads(8, 8, 1)]
void sky_view_main(uint3 id : SV_DispatchThreadID)
{
    GpuFrame            frame = load_frame();
    GpuAtmosphere       a     = load_atmosphere(frame);
    RWTexture2D<float4> lut   = g_images[NonUniformResourceIndex(a.handles.w)];

    uint width, height;
    lut.GetDimensions(width, height);
    if (id.x >= width || id.y >= height)
    {
        return;
    }

    float2 size = float2(width, height);
    float  view_zenith_cos, light_view_cos;
    sky_view_parameters(a, (float2(id.xy) + 0.5f) / size, size, view_zenith_cos, light_view_cos);

    float3 up, toward_sun;
    sky_frame(frame, a, up, toward_sun);
    float3 side      = cross(up, toward_sun);
    float  sin_view  = sqrt(max(1.0f - view_zenith_cos * view_zenith_cos, 0.0f));
    float  sin_light = sqrt(max(1.0f - light_view_cos * light_view_cos, 0.0f));
    float3 direction = normalize(toward_sun * sin_view * light_view_cos + side * sin_view * sin_light +
                                 up * view_zenith_cos);

    float3  luminance = 0.0f;
    Segment segment   = view_segment(a, direction, 1e30f);
    if (segment.end > segment.start)
    {
        luminance = march(frame, a, direction, segment.start, segment.end, 32u).luminance;
    }
    lut[id.xy] = float4(luminance, 1.0f);
}

// The environment the lighting pass fills and reflects, around the camera:
// the sky's radiance averaged cosine-weighted over the hemisphere above the
// nearest body's horizon, so it gives an upward surface the right
// irradiance, and the ground below, lit by the sun through the air and by
// that sky. The sky fill stands in for bounce light as without air.
groupshared float3 g_sky[64];

[shader("compute")]
[numthreads(64, 1, 1)]
void ambient_main(uint3 thread : SV_GroupThreadID)
{
    GpuFrame      frame = load_frame();
    GpuAtmosphere a     = load_atmosphere(frame);

    float3 up      = frame.env_up.xyz;
    float3 side    = normalize(abs(up.y) < 0.9f ? cross(up, float3(0.0f, 1.0f, 0.0f))
                                                : cross(up, float3(1.0f, 0.0f, 0.0f)));
    float3 forward = cross(side, up);

    uint   i         = thread.x;
    float  u         = (float(i % 8u) + 0.5f) / 8.0f;
    float  v         = (float(i / 8u) + 0.5f) / 8.0f;
    float  radius    = sqrt(u);
    float  phi       = 2.0f * kPi * v;
    float3 direction = normalize(side * radius * cos(phi) + forward * radius * sin(phi) +
                                 up * sqrt(max(1.0f - u, 0.0f)));

    Segment segment = view_segment(a, direction, 1e30f);
    float3  sky     = 0.0f;
    if (segment.end > segment.start)
    {
        sky = march(frame, a, direction, segment.start, segment.end, 16u).luminance;
    }
    g_sky[i] = sky;

    GroupMemoryBarrierWithGroupSync();
    if (i != 0u)
    {
        return;
    }

    // In order, one thread, so every run sums the same way.
    float3 total = 0.0f;
    for (uint k = 0; k < 64u; ++k)
    {
        total += g_sky[k];
    }
    float3 sky_radiance = total / 64.0f;

    float3 sun    = frame.sun_direction.xyz;
    float3 direct = frame.sun_radiance.rgb * sun_transmittance(frame, a, float3(0.0f)) * saturate(dot(up, sun));
    float3 ground = a.surroundings.rgb / kPi * (direct + kPi * sky_radiance);

    RWByteAddressBuffer ambient = g_rw_buffers[NonUniformResourceIndex(a.handles.z)];
    ambient.Store4(0u, asuint(float4(sky_radiance + ground * a.surroundings.w, 0.0f)));
    ambient.Store4(16u, asuint(float4(ground, 0.0f)));
}

// Over the lit HDR target. Where nothing was drawn, the sky: in-scattered
// light along the whole ray through the air, and the sun's disk seen
// through it. Where something was, aerial perspective: its colour dimmed by
// the air between and that air's own in-scattering added. From space the
// same march runs over the stretch of the ray inside the shell.
[shader("compute")]
[numthreads(8, 8, 1)]
void apply_main(uint3 id : SV_DispatchThreadID)
{
    GpuFrame frame = load_frame();
    uint2    pixel = id.xy;
    if (any(float2(pixel) >= frame.screen.xy))
    {
        return;
    }

    GpuAtmosphere       a     = load_atmosphere(frame);
    RWTexture2D<float4> hdr   = g_images[NonUniformResourceIndex(push.hdr_storage)];
    float               depth = g_textures[NonUniformResourceIndex(push.gbuffer_depth)].Load(int3(pixel, 0)).r;

    float2 ndc       = fragment_to_ndc(float2(pixel) + 0.5f, frame.screen.xy);
    float3 direction = normalize(view_at(ndc, 1.0f, frame));
    bool   sky       = depth <= 0.0f;
    float  limit     = sky ? 1e30f : length(view_at(ndc, distance_from_depth(depth, frame), frame)) * 0.001f;

    float3  colour  = sky ? float3(0.0f) : hdr[pixel].rgb;
    float3  behind  = 1.0f;
    Segment segment = view_segment(a, direction, limit);
    if (sky && a.sky_view.y != 0u)
    {
        // In the air: the sky from its LUT, and the disk's transmittance
        // from the camera straight up the ray.
        float3 up, toward_sun;
        sky_frame(frame, a, up, toward_sun);
        float  zenith_cos = dot(direction, up);
        float3 level      = direction - up * zenith_cos;
        float  light_cos  = dot(level, level) > 1e-8f ? dot(normalize(level), toward_sun) : 1.0f;

        Texture2D<float4> lut = g_textures[NonUniformResourceIndex(a.sky_view.x)];
        uint              width, height;
        lut.GetDimensions(width, height);
        float2 uv = sky_view_uv(a, float2(width, height), zenith_cos, light_cos, segment.ground);
        colour    = lut.SampleLevel(g_samplers[NonUniformResourceIndex(frame.atmosphere.w)], uv, 0.0f).rgb;
        behind = transmittance_toward(frame, a, camera_position(a), camera_altitude(a), direction);
    }
    else if (segment.end > segment.start)
    {
        Scattered air = march(frame, a, direction, segment.start, segment.end,
                              steps_for(segment.end - segment.start));
        colour = colour * air.transmittance + air.luminance;
        behind = air.transmittance;
    }

    // The disk's luminance is its illuminance over its solid angle, far past
    // what RGBA16F holds; scaled down whole, not clamped per channel, so it
    // keeps the colour the air gave it.
    if (sky && !segment.ground && dot(direction, frame.sun_direction.xyz) >= a.shell.z)
    {
        float3 disk   = frame.sun_radiance.rgb / a.shell.w * behind;
        float  peak   = max(max(disk.r, disk.g), max(disk.b, 1e-6f));
        colour       += disk * min(1.0f, 60000.0f / peak);
    }

    hdr[pixel] = float4(colour, 1.0f);
}

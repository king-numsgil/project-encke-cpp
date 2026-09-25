// Temporal antialiasing: this frame's HDR, drawn through a projection
// jittered by a sub-pixel offset that cycles each frame, blended into the
// history of frames before, reprojected by the motion vectors. After the
// atmosphere, before exposure: the result goes to the history image, which
// exposure and tonemap then read in HDR's place.
//
// After Karis, "High Quality Temporal Supersampling" (2014), and Playdead's
// INSIDE talk (Pedersen, 2016):
//   - motion from the nearest surface in the 3x3 neighbourhood, so an edge
//     reprojects with whatever is in front
//   - history sampled Catmull-Rom, in five bilinear taps, so it stays sharp
//   - history clipped toward the neighbourhood's mean, within its variance in
//     YCoCg, which is what rejects stale colour where things moved or came
//     into view
//   - clipping and blending on colour compressed as c / (1 + luma), after
//     last frame's exposure, so a bright sample -- the sun, a specular glint
//     -- weighs no more than a dull one and leaves no firefly behind
// The history is stored linear and unexposed, so an exposure that adapts
// between frames does not fight it.

import lib.bindless;
import lib.cluster;
import lib.colour;
import lib.gpu_types;
import lib.screen;

// The new frame's share once the history is trusted.
static const float kCurrentWeight = 0.1f;

// How many standard deviations of the neighbourhood the history may stray.
static const float kClipGamma = 1.0f;

float3 to_ycocg(float3 c)
{
    return float3(0.25f * c.r + 0.5f * c.g + 0.25f * c.b,
                  0.5f * c.r - 0.5f * c.b,
                  -0.25f * c.r + 0.5f * c.g - 0.25f * c.b);
}

float3 from_ycocg(float3 c)
{
    float t = c.x - c.z;
    return float3(t + c.y, c.x + c.z, t - c.y);
}

float3 compress(float3 c)
{
    return c / (1.0f + luminance(c));
}

float3 expand(float3 c)
{
    return c / max(1.0f - luminance(c), 1e-4f);
}

// Clips q toward the box's centre until it lies inside, rather than
// clamping each channel, which would shift its hue.
float3 clip_to_box(float3 low, float3 high, float3 q)
{
    float3 centre = 0.5f * (high + low);
    float3 extent = 0.5f * (high - low) + 1e-6f;
    float3 offset = q - centre;
    float3 units  = abs(offset / extent);
    float  most   = max(units.x, max(units.y, units.z));
    return most > 1.0f ? centre + offset / most : q;
}

// Catmull-Rom through bilinear taps: the four middle weights folded into one
// sample between texels, and the corners dropped, which they barely touch.
// After Jimenez, "Filmic SMAA" (2016).
float3 sample_history(Texture2D<float4> history, SamplerState filter, float2 uv, float2 size)
{
    float2 position = uv * size;
    float2 centre   = floor(position - 0.5f) + 0.5f;
    float2 f        = position - centre;
    float2 f2       = f * f;
    float2 f3       = f2 * f;

    float2 w0  = -0.5f * f3 + f2 - 0.5f * f;
    float2 w1  = 1.5f * f3 - 2.5f * f2 + 1.0f;
    float2 w2  = -1.5f * f3 + 2.0f * f2 + 0.5f * f;
    float2 w3  = 0.5f * f3 - 0.5f * f2;
    float2 w12 = w1 + w2;

    float2 at0  = (centre - 1.0f) / size;
    float2 at12 = (centre + w2 / w12) / size;
    float2 at3  = (centre + 2.0f) / size;

    float3 sum = history.SampleLevel(filter, float2(at12.x, at0.y), 0.0f).rgb * (w12.x * w0.y) +
                 history.SampleLevel(filter, float2(at0.x, at12.y), 0.0f).rgb * (w0.x * w12.y) +
                 history.SampleLevel(filter, at12, 0.0f).rgb * (w12.x * w12.y) +
                 history.SampleLevel(filter, float2(at3.x, at12.y), 0.0f).rgb * (w3.x * w12.y) +
                 history.SampleLevel(filter, float2(at12.x, at3.y), 0.0f).rgb * (w12.x * w3.y);
    float weight = w12.x * w0.y + w0.x * w12.y + w12.x * w12.y + w3.x * w12.y + w12.x * w3.y;

    // The negative lobes can undershoot next to a bright texel.
    return max(sum / weight, 0.0f);
}

// The group's 8x8 pixels and a one-pixel border, each read, exposed,
// compressed and converted once instead of by all nine of its neighbours.
static const int kGroup = 8;
static const int kTile  = kGroup + 2;

groupshared float3 g_ycocg[kTile * kTile];
groupshared float  g_depth[kTile * kTile];

[shader("compute")]
[numthreads(8, 8, 1)]
void compute_main(uint3 id : SV_DispatchThreadID, uint3 local : SV_GroupThreadID, uint3 group : SV_GroupID)
{
    GpuFrame frame = load_frame();
    int2     pixel = int2(id.xy);
    int2     size  = int2(frame.screen.xy);

    RWTexture2D<float4> current = g_images[NonUniformResourceIndex(push.hdr_storage)];
    RWTexture2D<float4> output  = g_images[NonUniformResourceIndex(frame.taa_images.y)];
    Texture2D<float4>   depths  = g_textures[NonUniformResourceIndex(push.gbuffer_depth)];

    // Last frame's adapted exposure: this frame's adapt pass runs after.
    float exposure = frame.taa.w;
    if (frame.taa_images.z != 0xFFFFFFFFu)
    {
        float ev100 = g_textures[NonUniformResourceIndex(frame.taa_images.z)].Load(int3(0, 0, 0)).r;
        exposure    = 1.0f / (1.2f * exp2(ev100));
    }

    int2 tile_origin = int2(group.xy) * kGroup - 1;
    for (int i = int(local.y) * kGroup + int(local.x); i < kTile * kTile; i += kGroup * kGroup)
    {
        int2 at    = clamp(tile_origin + int2(i % kTile, i / kTile), int2(0), size - 1);
        g_ycocg[i] = to_ycocg(compress(current[at].rgb * exposure));
        g_depth[i] = depths.Load(int3(at, 0)).r;
    }
    GroupMemoryBarrierWithGroupSync();

    if (any(pixel >= size))
    {
        return;
    }

    // The neighbourhood's moments, and its nearest surface: largest depth,
    // reversed-Z.
    float3 sum         = 0.0f;
    float3 sum_squares = 0.0f;
    float  nearest     = -1.0f;
    int2   nearest_at  = pixel;
    for (int dy = -1; dy <= 1; ++dy)
    {
        for (int dx = -1; dx <= 1; ++dx)
        {
            int    slot  = (int(local.y) + 1 + dy) * kTile + int(local.x) + 1 + dx;
            float3 ycocg = g_ycocg[slot];
            sum         += ycocg;
            sum_squares += ycocg * ycocg;

            if (g_depth[slot] > nearest)
            {
                nearest    = g_depth[slot];
                nearest_at = clamp(pixel + int2(dx, dy), int2(0), size - 1);
            }
        }
    }
    float3 centre = from_ycocg(g_ycocg[(int(local.y) + 1) * kTile + int(local.x) + 1]);
    float3 mean   = sum / 9.0f;
    float3 sigma  = sqrt(max(sum_squares / 9.0f - mean * mean, 0.0f));

    // Where this pixel was last frame. The sky has no motion vector: it
    // moves with the camera's rotation alone, reprojected from the
    // unjittered ray through the pixel centre.
    float2 uv = (float2(pixel) + 0.5f) / frame.screen.xy;
    float2 previous_uv;
    if (nearest > 0.0f)
    {
        previous_uv = uv + g_textures[NonUniformResourceIndex(push.gbuffer_motion)].Load(int3(nearest_at, 0)).rg;
    }
    else
    {
        float2 ndc       = fragment_to_ndc(float2(pixel) + 0.5f, frame.screen.xy);
        float3 direction = float3(ndc.x / frame.projection[0][0], ndc.y / frame.projection[1][1], -1.0f);
        float4 clip      = mul(frame.sky_reprojection, float4(direction, 0.0f));
        previous_uv      = ndc_to_uv(clip.xy / clip.w);
    }

    // With no history the frame passes through untouched, not round-tripped
    // through last frame's exposure: that exposure was metered from the
    // history being discarded, and would carry it into the next frames.
    bool usable = frame.taa.z < 0.5f && all(previous_uv >= 0.0f) && all(previous_uv <= 1.0f);
    if (!usable)
    {
        output[pixel] = float4(current[pixel].rgb, 1.0f);
        return;
    }

    Texture2D<float4> history = g_textures[NonUniformResourceIndex(frame.taa_images.x)];
    SamplerState      filter  = g_samplers[NonUniformResourceIndex(frame.taa_images.w)];

    float3 previous = compress(sample_history(history, filter, previous_uv, frame.screen.xy) * exposure);
    float3 clipped  = from_ycocg(clip_to_box(mean - kClipGamma * sigma, mean + kClipGamma * sigma,
                                             to_ycocg(previous)));
    float3 resolved = lerp(clipped, centre, kCurrentWeight);

    output[pixel] = float4(expand(resolved) / exposure, 1.0f);
}

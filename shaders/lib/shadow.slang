// Shadow-map lookups, shared by lighting and the cascade debug view.
//
// Every map is reversed-Z like the main depth buffer: nearest the light is
// 1.0. The comparison sampler is GREATER_OR_EQUAL, so a tap returns 1 (lit)
// when the receiver is at least as near the light as the stored occluder.
//
// Maps are rendered through the same negative-height viewport as the frame, so
// clip -> UV goes through ndc_to_uv exactly like a screen-space lookup.

module shadow;

import lib.bindless;
import lib.gpu_types;
import lib.screen;

// 3x3 bilinear comparison taps: a 4x4-texel footprint, smoothly weighted.
float pcf(uint image, float2 uv, float reference)
{
    Texture2D<float4>      map     = g_textures[NonUniformResourceIndex(image)];
    SamplerComparisonState compare = g_compare_samplers[NonUniformResourceIndex(push.shadow_sampler)];

    uint width;
    uint height;
    map.GetDimensions(width, height);
    float2 texel = 1.0f / float2(width, height);

    float lit = 0.0f;
    for (int y = -1; y <= 1; ++y)
    {
        for (int x = -1; x <= 1; ++x)
        {
            lit += map.SampleCmpLevelZero(compare, uv + float2(x, y) * texel, reference);
        }
    }
    return lit / 9.0f;
}

// 1 lit, 0 shadowed, for a view-space position on a surface with a view-space
// normal. The receiver is pushed along its normal by `offset_texels` of the
// map's footprint at that point, which removes acne on surfaces facing away
// from the light without the peter-panning a large depth bias would cause.
float visibility(GpuShadowView view, float3 position, float3 normal, float offset_texels)
{
    float4 clip = mul(view.view_to_clip, float4(position, 1.0f));

    // A spot's texels grow with distance from the light, which clip.w is.
    float footprint = view.params.w > 0.5f ? view.params.y * clip.w : view.params.y;
    clip = mul(view.view_to_clip, float4(position + normal * footprint * offset_texels, 1.0f));

    if (clip.w <= 0.0f)
    {
        return 1.0f;
    }

    float3 ndc = clip.xyz / clip.w;
    float2 uv  = ndc_to_uv(ndc.xy);
    if (any(uv < 0.0f) || any(uv > 1.0f))
    {
        return 1.0f;
    }

    return pcf(view.image.x, uv, ndc.z);
}

// Which cascade covers a view distance, or the cascade count past the last.
public uint cascade_of(float distance, GpuFrame frame)
{
    uint count = frame.counts.y;
    for (uint cascade = 0; cascade < count; ++cascade)
    {
        if (distance <= load_shadow_view(cascade).params.x)
        {
            return cascade;
        }
    }
    return count;
}

public float sun_visibility(float3 position, float3 normal, GpuFrame frame)
{
    float distance = -position.z;
    uint  cascade  = cascade_of(distance, frame);
    if (cascade >= frame.counts.y)
    {
        return 1.0f;
    }

    float lit  = visibility(load_shadow_view(cascade), position, normal, frame.shadow.z);
    float fade = saturate((distance - frame.shadow.y) / max(frame.shadow.x - frame.shadow.y, 1e-3f));
    return lerp(lit, 1.0f, fade);
}

public float spot_visibility(uint index, float3 position, float3 normal, GpuFrame frame)
{
    GpuShadowView view = load_shadow_view(index);
    float         lit  = visibility(view, position, normal, frame.shadow.z);
    return lerp(1.0f, lit, view.params.z);
}

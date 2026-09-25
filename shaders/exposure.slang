// Auto-exposure, two compute entry points run back to back after lighting:
//
//   histogram_main  one thread per pixel: bin the HDR target's log2 luminance
//   adapt_main      one group of 256: average the histogram between two
//                   percentiles, meter an EV100 from it, move the stored EV100
//                   toward that over time, and clear the histogram for the
//                   next frame
//
// Bin 0 holds everything darker than the histogram's range, and every pixel
// where nothing was drawn, and is left out of the average: exposure follows
// what is drawn, and the sky, bright air or black space, is backdrop. Metered
// with it, the sky seen from high in the atmosphere, dim but not empty, took
// half the frame and burned the planet below to white. A frame with nothing
// drawn counts nothing and holds its exposure.
//
// The result is EV100 in a 1x1 R32F image, which tonemap samples. Metering is
// the saturation-based ISO 100 convention (K = 12.5), so EV100 is
// log2(L * 100 / 12.5) for average luminance L, and tonemap's exposure is
// 1 / (1.2 * 2^EV100). A scene lit by the real Sun meters around EV100 14.

import lib.bindless;
import lib.colour;
import lib.gpu_types;

static const uint kBins      = 256;
static const uint kGroupSide = 16;

groupshared uint g_bins[kBins];

uint bin_of(float luminance, GpuFrame frame)
{
    float stops = log2(max(luminance, 1e-30f)) - frame.exposure_range.x;
    if (stops < 0.0f)
    {
        return 0u;
    }
    return 1u + uint(saturate(stops / frame.exposure_range.y) * float(kBins - 2u));
}

float log_luminance_of(uint bin, GpuFrame frame)
{
    return frame.exposure_range.x +
           (float(bin) - 0.5f) / float(kBins - 2u) * frame.exposure_range.y;
}

[shader("compute")]
[numthreads(16, 16, 1)]
void histogram_main(uint3 id : SV_DispatchThreadID, uint index : SV_GroupIndex)
{
    GpuFrame frame = load_frame();

    g_bins[index] = 0u;
    GroupMemoryBarrierWithGroupSync();

    if (all(float2(id.xy) < frame.screen.xy))
    {
        RWTexture2D<float4> hdr   = g_images[NonUniformResourceIndex(push.hdr_storage)];
        float               depth = g_textures[NonUniformResourceIndex(push.gbuffer_depth)].Load(int3(id.xy, 0)).r;
        uint                bin   = depth > 0.0f ? bin_of(luminance(hdr[id.xy].rgb), frame) : 0u;
        InterlockedAdd(g_bins[bin], 1u);
    }
    GroupMemoryBarrierWithGroupSync();

    // One global add per bin per group, instead of one per pixel.
    uint count = g_bins[index];
    if (count > 0u)
    {
        g_rw_buffers[NonUniformResourceIndex(push.exposure_histogram)]
            .InterlockedAdd(index * 4u, count);
    }
}

[shader("compute")]
[numthreads(256, 1, 1)]
void adapt_main(uint index : SV_GroupIndex)
{
    GpuFrame frame = load_frame();

    RWByteAddressBuffer histogram = g_rw_buffers[NonUniformResourceIndex(push.exposure_histogram)];
    g_bins[index] = histogram.Load(index * 4u);
    histogram.Store(index * 4u, 0u);
    GroupMemoryBarrierWithGroupSync();

    // Serial on one thread: 255 bins, and a fixed order keeps the result
    // identical between runs, which the byte comparisons rely on.
    if (index != 0u)
    {
        return;
    }

    // Jumping straight to the target is also how the first frame starts:
    // the image holds nothing yet, so the scene's own EV100 stands in until
    // something has been metered.
    bool                jump    = frame.exposure_limits.z > 0.5f;
    RWTexture2D<float4> state   = g_images[NonUniformResourceIndex(push.exposure_image)];
    float               current = jump ? frame.exposure_limits.w : state[uint2(0, 0)].r;

    uint counted = 0u;
    for (uint bin = 1u; bin < kBins; ++bin)
    {
        counted += g_bins[bin];
    }

    float target = current;
    if (counted > 0u)
    {
        // Mean log luminance of the pixels between the two percentiles; a
        // bin straddling a cut contributes only its share inside.
        float low  = frame.exposure_range.z * float(counted);
        float high = frame.exposure_range.w * float(counted);

        float below   = 0.0f;
        float weight  = 0.0f;
        float log_sum = 0.0f;
        for (uint bin = 1u; bin < kBins; ++bin)
        {
            float count = float(g_bins[bin]);
            float start = max(below, low);
            float end   = min(below + count, high);
            if (end > start)
            {
                weight  += end - start;
                log_sum += (end - start) * log_luminance_of(bin, frame);
            }
            below += count;
        }

        if (weight > 0.0f)
        {
            // log2(L * 100 / 12.5) = log2(L) + 3.
            target = log_sum / weight + 3.0f - frame.exposure_adapt.w;
            target = clamp(target, frame.exposure_limits.x, frame.exposure_limits.y);
        }
    }

    float adapted = target;
    if (!jump)
    {
        float speed = target > current ? frame.exposure_adapt.y : frame.exposure_adapt.z;
        adapted     = current + (target - current) * (1.0f - exp(-frame.exposure_adapt.x * speed));
    }

    state[uint2(0, 0)] = float4(adapted, 0.0f, 0.0f, 0.0f);
}

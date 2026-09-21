// Visualisations of renderer state, each drawn into its own image for a UI
// window rather than over the lit frame. One dispatch per open window, after
// lighting, so the cluster lists and G-buffer are this frame's.
//
// debug_view selects what to draw into push.debug_target:
//   2  lights per cluster, as a heat map
//   3  view-space normals
//   4  motion vectors, magnified by push.debug_gain
//
// Output is linear, like every texture the UI samples. Every pixel is
// written, so the image needs no clear.

import lib.bindless;
import lib.cluster;
import lib.gpu_types;
import lib.normal;

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

    RWTexture2D<float4> target = g_images[NonUniformResourceIndex(push.debug_target)];
    int3                texel  = int3(pixel, 0);

    if (push.debug_view == 4u)
    {
        // Signed, centred on grey: static geometry under a static camera, and
        // empty space, are exactly 0.5 grey.
        //
        // Motion is per-FRAME UV displacement, so the gain that makes it
        // legible depends on the frame rate -- which is why it is a slider
        // in the window rather than a constant here.
        float2 motion = g_textures[NonUniformResourceIndex(push.gbuffer_motion)].Load(texel).rg;
        target[pixel] = float4(saturate(motion * push.debug_gain + 0.5f), 0.5f, 1.0f);
        return;
    }

    float depth = g_textures[NonUniformResourceIndex(push.gbuffer_depth)].Load(texel).r;

    // Reversed-Z clears to 0.0: nothing was drawn here.
    if (depth <= 0.0f)
    {
        target[pixel] = float4(0.0f, 0.0f, 0.0f, 1.0f);
        return;
    }

    if (push.debug_view == 2u)
    {
        // Infinite reversed-Z maps view distance d to depth = near / d.
        float distance = frame.cluster_depth.x / depth;
        uint  cluster  = cluster_of(pixel, distance, frame);
        uint  count = g_rw_buffers[NonUniformResourceIndex(push.cluster_counts)].Load(cluster * 4u);
        target[pixel] = float4(heat(float(count) / 16.0f), 1.0f);
        return;
    }

    float2 encoded = g_textures[NonUniformResourceIndex(push.gbuffer_normal)].Load(texel).rg;
    target[pixel]  = float4(decode_normal(encoded) * 0.5f + 0.5f, 1.0f);
}

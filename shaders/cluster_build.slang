// Assigns lights to clusters. One thread per cluster, each testing every light
// against its view-space AABB -- brute force, and correct, which is the point
// of a first draft. Lists are fixed-size per cluster; a cluster that overflows
// keeps the first `max` lights and drops the rest.

import lib.bindless;
import lib.cluster;
import lib.gpu_types;
import lib.screen;

struct Aabb
{
    float3 lo;
    float3 hi;
};

Aabb cluster_bounds(uint3 cluster, GpuFrame frame)
{
    float2 grid = float2(frame.cluster_grid.xy);

    // Tile corners in UV (top-left origin), taken to NDC through the shared
    // helper so the Y flip matches what lighting sees per pixel.
    float2 uv_lo  = float2(cluster.xy) / grid;
    float2 uv_hi  = float2(cluster.xy + 1u) / grid;
    float2 ndc_a  = uv_to_ndc(uv_lo);
    float2 ndc_b  = uv_to_ndc(uv_hi);

    float near_d = slice_distance(cluster.z, frame);
    float far_d  = slice_distance(cluster.z + 1u, frame);

    // A frustum slice's extremes are at its corners; the AABB of all eight
    // encloses it.
    float3 corners[8] = {
        view_at(float2(ndc_a.x, ndc_a.y), near_d, frame),
        view_at(float2(ndc_b.x, ndc_a.y), near_d, frame),
        view_at(float2(ndc_a.x, ndc_b.y), near_d, frame),
        view_at(float2(ndc_b.x, ndc_b.y), near_d, frame),
        view_at(float2(ndc_a.x, ndc_a.y), far_d, frame),
        view_at(float2(ndc_b.x, ndc_a.y), far_d, frame),
        view_at(float2(ndc_a.x, ndc_b.y), far_d, frame),
        view_at(float2(ndc_b.x, ndc_b.y), far_d, frame),
    };

    Aabb box;
    box.lo = corners[0];
    box.hi = corners[0];
    for (uint i = 1; i < 8; ++i)
    {
        box.lo = min(box.lo, corners[i]);
        box.hi = max(box.hi, corners[i]);
    }
    return box;
}

bool sphere_touches(float3 centre, float radius, Aabb box)
{
    float3 nearest = clamp(centre, box.lo, box.hi);
    float3 delta   = centre - nearest;
    return dot(delta, delta) <= radius * radius;
}

[shader("compute")]
[numthreads(64, 1, 1)]
void compute_main(uint3 id : SV_DispatchThreadID)
{
    GpuFrame frame = load_frame();

    uint3 grid  = frame.cluster_grid.xyz;
    uint  total = grid.x * grid.y * grid.z;
    uint  index = id.x;
    if (index >= total)
    {
        return;
    }

    uint max_lights = frame.cluster_grid.w;
    Aabb box        = cluster_bounds(unflatten(index, frame), frame);

    RWByteAddressBuffer lists = g_rw_buffers[NonUniformResourceIndex(push.cluster_lights)];

    uint count = 0;
    for (uint light = 0; light < frame.counts.x; ++light)
    {
        GpuLight l = load_light(light);
        if (sphere_touches(l.position_radius.xyz, l.position_radius.w, box))
        {
            if (count < max_lights)
            {
                lists.Store((index * max_lights + count) * 4u, light);
            }
            ++count;
        }
    }

    g_rw_buffers[NonUniformResourceIndex(push.cluster_counts)].Store(index * 4u,
                                                                    min(count, max_lights));
}

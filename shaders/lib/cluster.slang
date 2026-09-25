// Cluster grid addressing, shared by cluster_build and lighting.
//
// Both passes MUST agree on which cluster a point falls in, or a light gets
// assigned to one cluster and looked up in another -- which does not fail, it
// just loses lighting at cluster boundaries. Everything that maps a position
// to a cluster lives here and nowhere else.
//
// Tiles are addressed from the top-left, matching pixel coordinates. Depth
// slices are logarithmic in view distance: slice = log(d) * scale + bias,
// which spends resolution close to the camera where ship interiors live.

module cluster;

import lib.gpu_types;

// Distance along the view axis. View space looks down -Z.
public float view_distance(float3 view_position)
{
    return -view_position.z;
}

public uint depth_slice(float distance, GpuFrame frame)
{
    uint  slices = frame.cluster_grid.z;
    float slice  = log(max(distance, frame.cluster_depth.x)) * frame.cluster_depth.z +
                   frame.cluster_depth.w;
    return min(uint(max(slice, 0.0f)), slices - 1u);
}

// Depth at the near edge of a slice; slice == slices gives the far edge.
public float slice_distance(uint slice, GpuFrame frame)
{
    float near   = frame.cluster_depth.x;
    float far    = frame.cluster_depth.y;
    float slices = float(frame.cluster_grid.z);
    return near * pow(far / near, float(slice) / slices);
}

public uint flatten(uint3 cluster, GpuFrame frame)
{
    uint3 grid = frame.cluster_grid.xyz;
    return cluster.x + grid.x * (cluster.y + grid.y * cluster.z);
}

public uint3 unflatten(uint index, GpuFrame frame)
{
    uint3 grid = frame.cluster_grid.xyz;
    uint  x    = index % grid.x;
    uint  y    = (index / grid.x) % grid.y;
    uint  z    = index / (grid.x * grid.y);
    return uint3(x, y, z);
}

public uint cluster_of(uint2 pixel, float distance, GpuFrame frame)
{
    uint3 grid = frame.cluster_grid.xyz;
    uint2 tile = min(uint2(float2(pixel) * float2(grid.xy) * frame.screen.zw), grid.xy - 1u);
    return flatten(uint3(tile, depth_slice(distance, frame)), frame);
}

// View distance from a reversed-Z depth sample. The infinite projection maps
// distance d to depth = near / d, where near is the CAMERA's near plane, held
// in the projection itself (GLM m[3][2], row-first [2][3] here). Not the
// cluster grid's near distance, which is a separate and larger number: using
// that put every reconstructed position at twice its real distance.
public float distance_from_depth(float depth, GpuFrame frame)
{
    return frame.projection[2][3] / depth;
}

// View-space point at a given distance along the ray through an NDC position.
// Specific to the infinite reversed-Z projection: only its two focal terms
// matter, [0][0] = f / aspect and [1][1] = f, and TAA's jitter, the
// negated NDC offset in [0][2] and [1][2] (GLM's m[2][0] and m[2][1]),
// which every reconstruction must take back out or it lands a sub-pixel
// off the rasterised surface.
public float3 view_at(float2 ndc, float distance, GpuFrame frame)
{
    return float3((ndc.x + frame.projection[0][2]) * distance / frame.projection[0][0],
                  (ndc.y + frame.projection[1][2]) * distance / frame.projection[1][1],
                  -distance);
}

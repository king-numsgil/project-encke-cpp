// Geomorphing, for meshes that carry morph targets: terrain chunks. A vertex
// is drawn part way to its target, the vertex the next coarser LOD places
// in its parent cell, by how far it is from the camera; near a neighbour
// drawn one LOD coarser it is pulled all the way, and near a finer one held
// at its own position. See Geomorph in render/components.hpp.
//
// The G-buffer and shadow passes both run this, so a chunk casts the shadow
// of the surface it draws.

module morph;

import lib.bindless;
import lib.gpu_types;
import lib.normal;

// A vertex within this many voxels of a chunk face may be shared with the
// chunk across it, so near a coarser or finer neighbour it is pulled or held
// completely. Over the next kRampVoxels the pull fades out.
static const float kSharedVoxels = 1.25f;
static const float kRampVoxels   = 4.0f;

public struct Morphed
{
    public float3 position;
    public float3 normal;
};

// 1 within kSharedVoxels of a face, 0 from kRampVoxels further in.
float face_weight(float metres, float voxel)
{
    return saturate(1.0f - (metres / voxel - kSharedVoxels) / kRampVoxels);
}

// The strongest pull toward the neighbours in `mask`: for each, the
// product of how near the vertex is to every face between it and that
// neighbour. A chunk is far wider than the ramp, so a vertex is near at most
// one face on each axis, and only the seven neighbours toward those faces
// can pull on it: those are the only ones tested, unrolled. Decoding every
// set bit's offset in a loop, with divisions by 3 and 9, cost the G-buffer
// pass more than the rest of the morph together.
float neighbour_weight(uint mask, float3 low, float3 high)
{
    if (mask == 0u)
    {
        return 0.0f;
    }

    int3   side   = int3(low > 0.0f) * -1 + int3(high > 0.0f);
    float3 near   = max(low, high);
    float  weight = 0.0f;
    [unroll]
    for (uint corner = 1u; corner < 8u; ++corner)
    {
        uint3 along  = uint3(corner, corner >> 1u, corner >> 2u) & 1u;
        int3  offset = int3(along) * side;
        float w      = (along.x != 0u ? near.x : 1.0f) * (along.y != 0u ? near.y : 1.0f) *
                       (along.z != 0u ? near.z : 1.0f);
        uint  bit    = uint((offset.x + 1) + 3 * (offset.y + 1) + 9 * (offset.z + 1));
        weight       = max(weight, ((mask >> bit) & 1u) != 0u ? w : 0.0f);
    }
    return weight;
}

// Whether the object is a terrain chunk, which carries a TerrainVertex per
// vertex.
public bool is_terrain(GpuObject object)
{
    return object.morph_masks.z != 0u;
}

// The pool's TerrainVertex stream, whose handle the object carries.
// `vertex` is SV_VulkanVertexID, gl_VertexIndex, which includes the draw's
// vertexOffset, so it indexes the stream directly. Not SV_VertexID: Slang
// lowers that to gl_VertexIndex - gl_BaseVertex, as it does SV_InstanceID,
// and every mesh would read the first mesh's.
public GpuTerrainVertex load_terrain_vertex(GpuObject object, uint vertex)
{
    return g_buffers[NonUniformResourceIndex(object.morph_masks.w)]
        .Load<GpuTerrainVertex>(vertex * kTerrainVertexStride);
}

public Morphed morph_vertex(GpuObject object, GpuTerrainVertex target, float3 position, float3 normal)
{
    Morphed result;
    result.position = position;
    result.normal   = normal;
    if ((object.morph_masks.z & 1u) == 0u)
    {
        return result;
    }

    // From the camera, which view space puts at the origin.
    float distance = length(mul(object.model_view, float4(position, 1.0f)).xyz);
    float amount   = saturate((distance - object.morph.x) * object.morph.y);

    float  extent = object.morph.z;
    float  voxel  = object.morph.w;
    float3 low    = float3(face_weight(position.x, voxel), face_weight(position.y, voxel),
                           face_weight(position.z, voxel));
    float3 high   = float3(face_weight(extent - position.x, voxel), face_weight(extent - position.y, voxel),
                           face_weight(extent - position.z, voxel));

    // Held near finer neighbours, then pulled near coarser ones: where both
    // meet, at a corner, the coarser wins.
    amount = min(amount, 1.0f - neighbour_weight(object.morph_masks.y, low, high));
    amount = max(amount, neighbour_weight(object.morph_masks.x, low, high));

    float2 packed = float2(float(target.morph_normal & 0xFFFFu), float(target.morph_normal >> 16u)) / 65535.0f;

    result.position = lerp(position, target.morph_position, amount);
    result.normal   = normalize(lerp(normal, decode_normal(packed), amount));
    return result;
}

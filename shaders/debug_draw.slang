// Debug drawing over the finished frame, into the swapchain after tonemap:
// the scene's wireframe, and lines the CPU hands over (collision shapes, the
// octree's chunks). Both are depth-tested against the G-buffer's depth and
// write none, and both are drawn through the unjittered projection: they
// come after TAA's resolve, so jittered they would crawl by half a pixel a
// frame.
//
// Colours are display colours, not radiance: authored in sRGB, written
// linear into the _SRGB swapchain, untouched by exposure or the air.

import lib.bindless;
import lib.colour;
import lib.gpu_types;
import lib.morph;

// Pulled toward the camera by this fraction of their distance, so a wire
// wins the depth test against the surface it lies on. Reversed-Z: a larger
// depth is nearer.
static const float kDepthPull = 1.004f;

// A colour per LOD, cycling, by the chunk's voxel size: 2^n metres takes
// entry n mod 8, so it needs no base voxel size. Mirrors kLodColours in
// render/debug_lines.cpp, which colours the octree's boxes.
static const float3 kLodColours[8] = {
    float3(1.00f, 0.35f, 0.30f), float3(1.00f, 0.65f, 0.20f), float3(0.95f, 0.95f, 0.30f),
    float3(0.40f, 0.95f, 0.35f), float3(0.30f, 0.90f, 0.90f), float3(0.35f, 0.55f, 1.00f),
    float3(0.70f, 0.45f, 1.00f), float3(1.00f, 0.45f, 0.85f),
};

// What isn't terrain.
static const float3 kObjectWire = float3(0.85f, 0.90f, 0.95f);

float4 unjittered(float4 clip, GpuFrame frame)
{
    clip.xy -= frame.taa.xy * clip.w;
    clip.z  *= kDepthPull;
    return clip;
}

struct VertexIn
{
    float3 position : POSITION;
};

struct WireOut
{
    float4 position : SV_Position;
    nointerpolation float3 colour : COLOR;
};

[shader("vertex")]
WireOut wire_vertex(VertexIn input, uint instance : SV_VulkanInstanceID, uint vertex : SV_VulkanVertexID)
{
    GpuObject object = load_object(instance);

    // Morphed as the G-buffer draws it, so the wires lie on what is shown.
    float3 position = input.position;
    float3 colour   = srgb_to_linear(kObjectWire);
    if (is_terrain(object))
    {
        GpuTerrainVertex terrain = load_terrain_vertex(object, vertex);
        position = morph_vertex(object, terrain, input.position, float3(0.0f, 1.0f, 0.0f)).position;

        int lod = int(round(log2(object.morph.w)));
        colour  = srgb_to_linear(kLodColours[uint(lod + 64) % 8u]);
    }

    WireOut output;
    output.position = unjittered(mul(object.mvp, float4(position, 1.0f)), load_frame());
    output.colour   = colour;
    return output;
}

[shader("fragment")]
float4 wire_fragment(WireOut input) : SV_Target0
{
    return float4(input.colour, 1.0f);
}

[shader("vertex")]
WireOut line_vertex(uint vertex : SV_VulkanVertexID)
{
    GpuFrame       frame = load_frame();
    GpuDebugVertex end   = g_buffers[NonUniformResourceIndex(frame.debug.x)]
                             .Load<GpuDebugVertex>(vertex * kDebugVertexStride);

    float3 srgb = float3(float(end.colour & 0xFFu), float((end.colour >> 8u) & 0xFFu),
                         float((end.colour >> 16u) & 0xFFu)) / 255.0f;

    WireOut output;
    output.position = unjittered(mul(frame.projection, float4(end.position, 1.0f)), frame);
    output.colour   = srgb_to_linear(srgb);
    return output;
}

[shader("fragment")]
float4 line_fragment(WireOut input) : SV_Target0
{
    return float4(input.colour, 1.0f);
}

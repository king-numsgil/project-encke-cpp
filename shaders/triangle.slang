// No vertex buffer: positions come from SV_VertexID. The colours are picked so
// a vertical flip is obvious on screen -- red must appear at the apex, pointing
// up. If it points down, the negative-height viewport is not being applied.

import lib.colour;

struct VertexOut
{
    float4 position : SV_Position;
    float3 colour   : COLOR;
};

// Wound counter-clockwise in NDC with +Y up, which is the glTF convention and
// what frontFace = COUNTER_CLOCKWISE expects given the negative-height
// viewport. Reversing these two entries culls the triangle.
static const float2 kPositions[3] = {
    float2( 0.0,  0.6),   // apex
    float2(-0.6, -0.5),   // bottom left
    float2( 0.6, -0.5),   // bottom right
};

// Authored in sRGB and converted in the shader, so the intent stays readable.
// Writing these straight out would render them washed out -- the swapchain is
// an _SRGB format and encodes on write.
static const float3 kColoursSrgb[3] = {
    float3(0.90, 0.20, 0.22),   // red at the apex
    float3(0.20, 0.45, 0.90),   // blue bottom left
    float3(0.20, 0.75, 0.35),   // green bottom right
};

[shader("vertex")]
VertexOut vertex_main(uint index : SV_VertexID)
{
    VertexOut output;
    output.position = float4(kPositions[index], 0.0, 1.0);
    output.colour   = srgb_to_linear(kColoursSrgb[index]);
    return output;
}

[shader("fragment")]
float4 fragment_main(VertexOut input) : SV_Target
{
    return float4(input.colour, 1.0);
}

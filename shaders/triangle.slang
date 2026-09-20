// No vertex buffer: positions come from SV_VertexID. The colours are picked so
// a vertical flip is obvious on screen -- red must appear at the apex, pointing
// up. If it points down, the negative-height viewport is not being applied.

struct VertexOut
{
    float4 position : SV_Position;
    float3 colour   : COLOR;
};

static const float2 kPositions[3] = {
    float2( 0.0,  0.6),   // apex
    float2( 0.6, -0.5),   // bottom right
    float2(-0.6, -0.5),   // bottom left
};

// Shader output is LINEAR. The swapchain is _SRGB, so the hardware encodes on
// write and anything written here appears considerably lighter than the number
// suggests. These are the linear values for the sRGB colours actually wanted:
// (0.90, 0.20, 0.22), (0.20, 0.75, 0.35) and (0.20, 0.45, 0.90).
static const float3 kColours[3] = {
    float3(0.787, 0.033, 0.040),   // red at the apex
    float3(0.033, 0.523, 0.100),   // green bottom right
    float3(0.033, 0.171, 0.787),   // blue bottom left
};

[shader("vertex")]
VertexOut vertex_main(uint index : SV_VertexID)
{
    VertexOut output;
    output.position = float4(kPositions[index], 0.0, 1.0);
    output.colour   = kColours[index];
    return output;
}

[shader("fragment")]
float4 fragment_main(VertexOut input) : SV_Target
{
    return float4(input.colour, 1.0);
}

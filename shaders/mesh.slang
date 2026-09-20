// Vertex-buffer driven mesh with a single push-constant matrix.
//
// Depth is REVERSED: the projection maps near to 1.0 and far to 0.0, the
// attachment clears to 0.0 and the pipeline compares with GREATER_OR_EQUAL.
// Nothing here has to know that -- it falls out of the matrix -- but a
// projection built the usual way around will make everything vanish.

struct Push
{
    float4x4 mvp;
    float4x4 model;
};

[[vk::push_constant]]
Push push;

struct VertexIn
{
    float3 position : POSITION;
    float3 normal   : NORMAL;
    float3 colour   : COLOR;
};

struct VertexOut
{
    float4 position : SV_Position;
    float3 normal   : NORMAL;
    float3 colour   : COLOR;
};

[shader("vertex")]
VertexOut vertex_main(VertexIn input)
{
    VertexOut output;

    output.position = mul(push.mvp, float4(input.position, 1.0f));

    // Uniform scale only, so the model matrix rotates normals correctly
    // without an inverse transpose. Revisit when non-uniform scale appears.
    output.normal = normalize(mul((float3x3)push.model, input.normal));
    output.colour = input.colour;

    return output;
}

[shader("fragment")]
float4 fragment_main(VertexOut input) : SV_Target
{
    // Placeholder lambert against a fixed direction, purely so the faces read
    // as a solid object rather than flat colour. Real lighting comes with the
    // deferred pass.
    const float3 to_light = normalize(float3(0.4f, 0.8f, 0.45f));

    float  ndotl   = saturate(dot(normalize(input.normal), to_light));
    float3 ambient = input.colour * 0.18f;

    return float4(ambient + input.colour * ndotl, 1.0f);
}

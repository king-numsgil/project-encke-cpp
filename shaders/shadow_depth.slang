// Depth-only pass into one shadow map: a sun cascade or a spot light. No
// fragment stage; the rasteriser writes depth by itself.
//
// The instance index is (shadow view * max objects + object), pre-flattened
// on the CPU into each indirect draw's firstInstance, and addresses the
// object -> map clip matrix directly. That matrix was composed in f64 from
// the light's view and the object's model, so the large world translations
// cancelled before anything was narrowed.
//
// SV_VulkanInstanceID, not SV_InstanceID: Slang lowers SV_InstanceID to
// InstanceIndex - BaseInstance to match D3D, which would drop firstInstance.

import lib.bindless;
import lib.gpu_types;

struct VertexIn
{
    float3 position : POSITION;
};

[shader("vertex")]
float4 vertex_main(VertexIn input, uint instance : SV_VulkanInstanceID) : SV_Position
{
    return mul(load_shadow_matrix(instance), float4(input.position, 1.0f));
}

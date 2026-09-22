// The global descriptor set. Mirrors vulkan/bindless.hpp: binding numbers must
// agree, and nothing checks it except validation at draw time.
//
// Always index through NonUniformResourceIndex unless the handle is provably
// uniform across the wave. Handles here come from push constants, which are
// uniform, but the habit is cheap and the failure mode -- silently sampling the
// wrong resource on some lanes -- is not.

module bindless;

import lib.gpu_types;

// Read-only and writable buffers live in separate bindings on purpose. A
// writable storage buffer in the vertex or fragment stage needs
// vertexPipelineStoresAndAtomics / fragmentStoresAndAtomics, and those stages
// only ever read -- so they get the read-only view and the features stay off.
// Each buffer is registered in exactly one of the two.
[[vk::binding(0, 0)]] public Texture2D<float4>   g_textures[];
[[vk::binding(1, 0)]] public RWTexture2D<float4> g_images[];
[[vk::binding(2, 0)]] public ByteAddressBuffer   g_buffers[];
[[vk::binding(3, 0)]] public SamplerState        g_samplers[];
[[vk::binding(4, 0)]] public RWByteAddressBuffer g_rw_buffers[];

// The same sampler binding seen as comparison samplers. Comparison is a
// property of the VkSampler, not of the descriptor type, so one binding holds
// both kinds; a handle must only be read through the view matching how its
// sampler was created.
[[vk::binding(3, 0)]] public SamplerComparisonState g_compare_samplers[];

[[vk::push_constant]] public Push push;

public GpuFrame load_frame()
{
    return g_buffers[NonUniformResourceIndex(push.frame)].Load<GpuFrame>(0);
}

public GpuLight load_light(uint index)
{
    return g_buffers[NonUniformResourceIndex(push.lights)].Load<GpuLight>(index * kLightStride);
}

public GpuObject load_object(uint index)
{
    return g_buffers[NonUniformResourceIndex(push.objects)].Load<GpuObject>(index * kObjectStride);
}

public GpuShadowView load_shadow_view(uint index)
{
    return g_buffers[NonUniformResourceIndex(push.shadow_views)]
        .Load<GpuShadowView>(index * kShadowViewStride);
}

public float4x4 load_shadow_matrix(uint index)
{
    return g_buffers[NonUniformResourceIndex(push.shadow_matrices)]
        .Load<float4x4>(index * kMatrixStride);
}

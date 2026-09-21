#pragma once

namespace encke
{
    class VulkanDevice;

    // Both pipeline kinds share one layout shape: the global bindless set at
    // set 0 plus a single push-constant range visible to every stage. Handles
    // into the bindless set travel in the push constants.

    class GraphicsPipeline
    {
    public:
        struct Config
        {
            // Resolved against `shaders/` beside the executable.
            char const* spirv_name     = nullptr;
            char const* vertex_entry   = "vertex_main";
            char const* fragment_entry = "fragment_main";

            // One entry per colour attachment, in location order. Dynamic
            // rendering bakes these in, so they must match the pass exactly.
            span<VkFormat const> colour_formats;

            // VK_FORMAT_UNDEFINED disables depth entirely. When enabled the
            // test is reversed-Z: GREATER_OR_EQUAL against a 0.0 clear.
            VkFormat depth_format = VK_FORMAT_UNDEFINED;
            bool     depth_write  = true;

            // Empty means no vertex buffers.
            span<VkVertexInputBindingDescription const>   bindings;
            span<VkVertexInputAttributeDescription const> attributes;

            VkCullModeFlags cull_mode = VK_CULL_MODE_BACK_BIT;

            // Straight (non-premultiplied) alpha over every colour attachment:
            // rgb = src * a + dst * (1 - a), alpha accumulates coverage.
            bool alpha_blend = false;

            VkDescriptorSetLayout set_layout         = VK_NULL_HANDLE;
            u32                   push_constant_size = 0;
        };

        GraphicsPipeline() = default;
        ~GraphicsPipeline();

        GraphicsPipeline(GraphicsPipeline const&)            = delete;
        GraphicsPipeline& operator=(GraphicsPipeline const&) = delete;
        GraphicsPipeline(GraphicsPipeline&&)                 = delete;
        GraphicsPipeline& operator=(GraphicsPipeline&&)      = delete;

        bool init(VulkanDevice const& device, Config const& config);
        void shutdown();

        VkPipeline       handle() const { return pipeline_; }
        VkPipelineLayout layout() const { return layout_; }

    private:
        VulkanDevice const* device_   = nullptr;
        VkPipelineLayout    layout_   = VK_NULL_HANDLE;
        VkPipeline          pipeline_ = VK_NULL_HANDLE;
    };

    class ComputePipeline
    {
    public:
        struct Config
        {
            char const* spirv_name = nullptr;
            char const* entry      = "compute_main";

            VkDescriptorSetLayout set_layout         = VK_NULL_HANDLE;
            u32                   push_constant_size = 0;
        };

        ComputePipeline() = default;
        ~ComputePipeline();

        ComputePipeline(ComputePipeline const&)            = delete;
        ComputePipeline& operator=(ComputePipeline const&) = delete;
        ComputePipeline(ComputePipeline&&)                 = delete;
        ComputePipeline& operator=(ComputePipeline&&)      = delete;

        bool init(VulkanDevice const& device, Config const& config);
        void shutdown();

        VkPipeline       handle() const { return pipeline_; }
        VkPipelineLayout layout() const { return layout_; }

    private:
        VulkanDevice const* device_   = nullptr;
        VkPipelineLayout    layout_   = VK_NULL_HANDLE;
        VkPipeline          pipeline_ = VK_NULL_HANDLE;
    };
}

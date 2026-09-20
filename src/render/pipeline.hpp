#pragma once

namespace encke
{
    class VulkanDevice;

    // A graphics pipeline built from a single SPIR-V module holding both entry
    // points. Viewport and scissor are dynamic, so a resize does not invalidate
    // the pipeline -- but the colour format does, since dynamic rendering bakes
    // it in. Rebuild if the swapchain ever changes format.
    class GraphicsPipeline
    {
    public:
        GraphicsPipeline() = default;
        ~GraphicsPipeline();

        GraphicsPipeline(GraphicsPipeline const&)            = delete;
        GraphicsPipeline& operator=(GraphicsPipeline const&) = delete;
        GraphicsPipeline(GraphicsPipeline&&)                 = delete;
        GraphicsPipeline& operator=(GraphicsPipeline&&)      = delete;

        struct Config
        {
            // Resolved against `shaders/` beside the executable.
            char const* spirv_name     = nullptr;
            char const* vertex_entry   = "vertex_main";
            char const* fragment_entry = "fragment_main";

            VkFormat colour_format = VK_FORMAT_UNDEFINED;

            // VK_FORMAT_UNDEFINED disables depth entirely.
            VkFormat depth_format = VK_FORMAT_UNDEFINED;

            // Empty means no vertex buffers; the shader builds its own
            // positions from SV_VertexID.
            span<VkVertexInputBindingDescription const>   bindings;
            span<VkVertexInputAttributeDescription const> attributes;

            // Zero means no push constants.
            u32 push_constant_size = 0;
        };

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

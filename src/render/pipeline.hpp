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

        // spirv_name is resolved against `shaders/` beside the executable.
        bool init(VulkanDevice const& device, char const* spirv_name,
                  char const* vertex_entry, char const* fragment_entry,
                  VkFormat colour_format);

        void shutdown();

        VkPipeline handle() const { return pipeline_; }

    private:
        VulkanDevice const* device_   = nullptr;
        VkPipelineLayout    layout_   = VK_NULL_HANDLE;
        VkPipeline          pipeline_ = VK_NULL_HANDLE;
    };
}

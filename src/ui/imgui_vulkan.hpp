// Dear ImGui renderer backend for encke's Vulkan stack.
//
// Forked from backends/imgui_impl_vulkan.{h,cpp} of Dear ImGui v1.92.9b-docking
// (https://github.com/ocornut/imgui), and version-locked there: upgrading ImGui
// means re-reading upstream's changes to that file and porting what applies.
// Changes from upstream:
//
//   - Memory comes from VMA (vulkan/buffer, vulkan/image), never
//     vkAllocateMemory.
//   - Textures live in the global bindless set. ImTextureID is a bindless
//     sampled-image handle plus one (see texture_id()), so any renderer target
//     can be shown in an ImGui window without a descriptor set of its own.
//   - Texture uploads are recorded into the frame's command buffer in
//     prepare(), instead of a separate submit followed by vkQueueWaitIdle.
//     Staging buffers and destroyed textures are freed once their frame slot
//     comes round again.
//   - Shaders are Slang (shaders/imgui.slang), sharing lib/colour, so the UI is
//     colour-correct on both sRGB and UNORM targets.
//   - The pipeline is built by render/pipeline with the shared layout: the
//     bindless set plus gpu::Push.
//   - Multi-viewport support and the ImGui_ImplVulkanH_* window helpers are
//     removed. Secondary OS windows would need them ported onto
//     VulkanSwapchain; docking alone needs nothing from the renderer.
//
// ---------------------------------------------------------------------------
// The MIT License (MIT)
//
// Copyright (c) 2014-2026 Omar Cornut
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in all
// copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.
// ---------------------------------------------------------------------------

#pragma once

#include "render/gpu_types.hpp"
#include "render/pipeline.hpp"
#include "vulkan/buffer.hpp"

#include <imgui.h>

namespace encke
{
    class BindlessSet;
    class VulkanAllocator;
    class VulkanDevice;

    // Handed to draw callbacks through ImGui::GetPlatformIO().Renderer_RenderState
    // while record() runs; upstream's ImGui_ImplVulkan_RenderState. A callback
    // that changes `push` must push it again itself.
    struct ImGuiVulkanRenderState
    {
        VkCommandBuffer  command = VK_NULL_HANDLE;
        VkPipelineLayout layout  = VK_NULL_HANDLE;
        gpu::Push*       push    = nullptr;
    };

    class ImGuiVulkan
    {
    public:
        // ImGui reserves 0 as "no texture", and 0 is a live bindless slot, so
        // handles travel offset by one.
        static ImTextureID texture_id(u32 bindless_handle)
        {
            return static_cast<ImTextureID>(bindless_handle) + 1;
        }

        ImGuiVulkan() = default;
        ~ImGuiVulkan();

        ImGuiVulkan(ImGuiVulkan const&)            = delete;
        ImGuiVulkan& operator=(ImGuiVulkan const&) = delete;
        ImGuiVulkan(ImGuiVulkan&&)                 = delete;
        ImGuiVulkan& operator=(ImGuiVulkan&&)      = delete;

        // Requires a current ImGui context. `target_format` is the colour
        // attachment record() will draw into; sRGB or not decides whether the
        // shader encodes. `frames_in_flight` sizes the per-slot buffers.
        bool init(VulkanAllocator const& allocator, VulkanDevice const& device,
                  BindlessSet& bindless, VkFormat target_format, u32 frames_in_flight);

        // The device must be idle.
        void shutdown();

        // Outside any rendering scope. Uploads texture changes and this
        // frame's vertices into `slot`, whose previous frame has retired.
        void prepare(ImDrawData* draw_data, VkCommandBuffer command, u32 slot);

        // Inside the rendering scope. Draws what the last prepare() uploaded.
        void record(ImDrawData const* draw_data, VkCommandBuffer command);

    private:
        struct Texture;

        // Everything whose lifetime is tied to one frame in flight.
        struct Slot
        {
            Buffer vertices;
            Buffer indices;

            // Freed at the next prepare() of this slot, when the frame that
            // used them has retired.
            vector<std::unique_ptr<Buffer>> staging;
            vector<Texture*>                retired;
        };

        void update_texture(ImTextureData& data, VkCommandBuffer command, Slot& slot);
        void destroy_texture(Texture* texture);
        void free_retired(Slot& slot);

        void setup_render_state(ImDrawData const& draw_data, VkCommandBuffer command,
                                VkExtent2D framebuffer);
        void push_constants(VkCommandBuffer command) const;

        // ImGui identifies the special callbacks by function address.
        static void callback_reset_render_state(ImDrawList const* list, ImDrawCmd const* cmd);
        static void callback_sampler_linear(ImDrawList const* list, ImDrawCmd const* cmd);
        static void callback_sampler_nearest(ImDrawList const* list, ImDrawCmd const* cmd);

        VulkanAllocator const* allocator_ = nullptr;
        VulkanDevice const*    device_    = nullptr;
        BindlessSet*           bindless_  = nullptr;

        // Kept for the pipeline config span, which must outlive init.
        VkFormat         target_format_ = VK_FORMAT_UNDEFINED;
        GraphicsPipeline pipeline_;

        VkSampler linear_         = VK_NULL_HANDLE;
        VkSampler nearest_        = VK_NULL_HANDLE;
        u32       linear_handle_  = ~0u;
        u32       nearest_handle_ = ~0u;

        vector<std::unique_ptr<Slot>> slots_;
        u32                           current_slot_ = 0;

        // Only the ui_* fields are meaningful to the shader; the rest ride
        // along because every pipeline shares one push range.
        gpu::Push              push_{};
        ImGuiVulkanRenderState render_state_;

        bool initialised_ = false;
    };
}

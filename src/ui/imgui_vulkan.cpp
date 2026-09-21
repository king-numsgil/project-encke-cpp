// Dear ImGui renderer backend for encke's Vulkan stack. Forked from
// backends/imgui_impl_vulkan.cpp of Dear ImGui v1.92.9b-docking; see
// ui/imgui_vulkan.hpp for what changed and why.
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

#include "core/pch.hpp"

#include "ui/imgui_vulkan.hpp"

#include "core/log.hpp"
#include "core/memory.hpp"
#include "vulkan/allocator.hpp"
#include "vulkan/bindless.hpp"
#include "vulkan/device.hpp"
#include "vulkan/image.hpp"

#include <algorithm>
#include <cstring>

namespace encke
{
    struct ImGuiVulkan::Texture
    {
        Image image;
        u32   handle = BindlessSet::kInvalid;
    };

    namespace
    {
        constexpr VkImageLayout kSampledLayout = VK_IMAGE_LAYOUT_READ_ONLY_OPTIMAL;

        // ImGui's textures hold colour authored in sRGB. An _SRGB format makes
        // the sampler decode it, so the shader sees linear values the same as
        // it does from a renderer target. Alpha is never decoded.
        constexpr VkFormat kTextureFormat = VK_FORMAT_R8G8B8A8_SRGB;

        constexpr VkVertexInputBindingDescription kBindings[]{
            {.binding = 0, .stride = sizeof(ImDrawVert), .inputRate = VK_VERTEX_INPUT_RATE_VERTEX},
        };

        constexpr VkVertexInputAttributeDescription kAttributes[]{
            {.location = 0, .binding = 0, .format = VK_FORMAT_R32G32_SFLOAT,
             .offset = offsetof(ImDrawVert, pos)},
            {.location = 1, .binding = 0, .format = VK_FORMAT_R32G32_SFLOAT,
             .offset = offsetof(ImDrawVert, uv)},
            {.location = 2, .binding = 0, .format = VK_FORMAT_R8G8B8A8_UNORM,
             .offset = offsetof(ImDrawVert, col)},
        };

        constexpr VkIndexType kIndexType =
            sizeof(ImDrawIdx) == 2 ? VK_INDEX_TYPE_UINT16 : VK_INDEX_TYPE_UINT32;

        bool is_srgb(VkFormat format)
        {
            switch (format)
            {
            case VK_FORMAT_B8G8R8A8_SRGB:
            case VK_FORMAT_R8G8B8A8_SRGB:
            case VK_FORMAT_A8B8G8R8_SRGB_PACK32:
                return true;
            default:
                return false;
            }
        }

        ImGuiVulkan* backend()
        {
            return static_cast<ImGuiVulkan*>(ImGui::GetIO().BackendRendererUserData);
        }

        VkSampler create_sampler(VkDevice device, VkFilter filter, VkSamplerMipmapMode mip)
        {
            VkSamplerCreateInfo const info{
                .sType                   = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
                .pNext                   = nullptr,
                .flags                   = 0,
                .magFilter               = filter,
                .minFilter               = filter,
                .mipmapMode              = mip,
                .addressModeU            = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                .addressModeV            = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                .addressModeW            = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                .mipLodBias              = 0.0f,
                .anisotropyEnable        = VK_FALSE,
                .maxAnisotropy           = 1.0f,
                .compareEnable           = VK_FALSE,
                .compareOp               = VK_COMPARE_OP_ALWAYS,
                .minLod                  = -1000.0f,
                .maxLod                  = 1000.0f,
                .borderColor             = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK,
                .unnormalizedCoordinates = VK_FALSE,
            };

            VkSampler      sampler = VK_NULL_HANDLE;
            VkResult const result =
                vkCreateSampler(device, &info, memory::vulkan_callbacks(), &sampler);
            if (result != VK_SUCCESS)
            {
                log::vk_error("vkCreateSampler (imgui)", result);
                return VK_NULL_HANDLE;
            }
            return sampler;
        }

        void image_barrier(VkCommandBuffer command, VkImage image, VkImageLayout from,
                           VkImageLayout to, VkPipelineStageFlags2 src_stage,
                           VkAccessFlags2 src_access, VkPipelineStageFlags2 dst_stage,
                           VkAccessFlags2 dst_access)
        {
            VkImageMemoryBarrier2 const barrier{
                .sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
                .pNext               = nullptr,
                .srcStageMask        = src_stage,
                .srcAccessMask       = src_access,
                .dstStageMask        = dst_stage,
                .dstAccessMask       = dst_access,
                .oldLayout           = from,
                .newLayout           = to,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .image               = image,
                .subresourceRange    = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
            };

            VkDependencyInfo const dependency{
                .sType                    = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
                .pNext                    = nullptr,
                .dependencyFlags          = 0,
                .memoryBarrierCount       = 0,
                .pMemoryBarriers          = nullptr,
                .bufferMemoryBarrierCount = 0,
                .pBufferMemoryBarriers    = nullptr,
                .imageMemoryBarrierCount  = 1,
                .pImageMemoryBarriers     = &barrier,
            };

            vkCmdPipelineBarrier2(command, &dependency);
        }

        // Grows a per-slot mapped buffer to hold `bytes`, with headroom so a
        // UI that grows a little each frame does not reallocate every frame.
        bool reserve(Buffer& buffer, VulkanAllocator const& allocator, VkDeviceSize bytes,
                     VkBufferUsageFlags usage)
        {
            if (buffer.handle() != VK_NULL_HANDLE && buffer.size() >= bytes)
            {
                return true;
            }

            buffer.shutdown();
            return buffer.init_mapped(allocator, std::max<VkDeviceSize>(bytes + bytes / 2, 64 * 1024),
                                      usage);
        }
    }

    ImGuiVulkan::~ImGuiVulkan()
    {
        shutdown();
    }

    bool ImGuiVulkan::init(VulkanAllocator const& allocator, VulkanDevice const& device,
                           BindlessSet& bindless, VkFormat target_format, u32 frames_in_flight)
    {
        allocator_     = &allocator;
        device_        = &device;
        bindless_      = &bindless;
        target_format_ = target_format;

        ImGuiIO& io = ImGui::GetIO();
        if (io.BackendRendererUserData != nullptr)
        {
            log::error("an ImGui renderer backend is already initialised");
            return false;
        }

        io.BackendRendererUserData = this;
        io.BackendRendererName     = "encke_vulkan";
        io.BackendFlags |= ImGuiBackendFlags_RendererHasVtxOffset;
        io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;
        initialised_ = true;

        ImGuiPlatformIO& platform_io              = ImGui::GetPlatformIO();
        platform_io.DrawCallback_ResetRenderState = callback_reset_render_state;
        platform_io.DrawCallback_SetSamplerLinear  = callback_sampler_linear;
        platform_io.DrawCallback_SetSamplerNearest = callback_sampler_nearest;

        int const max_dimension =
            static_cast<int>(device.properties().limits.maxImageDimension2D);
        platform_io.Renderer_TextureMaxWidth  = max_dimension;
        platform_io.Renderer_TextureMaxHeight = max_dimension;

        linear_  = create_sampler(device.handle(), VK_FILTER_LINEAR, VK_SAMPLER_MIPMAP_MODE_LINEAR);
        nearest_ = create_sampler(device.handle(), VK_FILTER_NEAREST, VK_SAMPLER_MIPMAP_MODE_NEAREST);
        if (linear_ == VK_NULL_HANDLE || nearest_ == VK_NULL_HANDLE)
        {
            return false;
        }
        linear_handle_  = bindless.add_sampler(linear_);
        nearest_handle_ = bindless.add_sampler(nearest_);

        GraphicsPipeline::Config const config{
            .spirv_name         = "imgui.spv",
            .colour_formats     = span<VkFormat const>{&target_format_, 1},
            .depth_format       = VK_FORMAT_UNDEFINED,
            .depth_write        = false,
            .bindings           = kBindings,
            .attributes         = kAttributes,
            .cull_mode          = VK_CULL_MODE_NONE,
            .alpha_blend        = true,
            .set_layout         = bindless.layout(),
            .push_constant_size = sizeof(gpu::Push),
        };

        if (!pipeline_.init(device, config))
        {
            return false;
        }

        slots_.clear();
        for (u32 index = 0; index < frames_in_flight; ++index)
        {
            slots_.push_back(std::make_unique<Slot>());
        }

        push_                = gpu::Push{};
        push_.ui_texture     = BindlessSet::kInvalid;
        push_.ui_sampler     = linear_handle_;
        push_.ui_encode_srgb = is_srgb(target_format) ? 0u : 1u;

        return true;
    }

    void ImGuiVulkan::prepare(ImDrawData* draw_data, VkCommandBuffer command, u32 slot_index)
    {
        current_slot_ = slot_index;
        Slot& slot    = *slots_[slot_index];

        // The fence for this slot has been waited on, so whatever the frame
        // before last left here is no longer read by the GPU.
        free_retired(slot);

        if (draw_data == nullptr)
        {
            return;
        }

        // Usually one texture, already OK. (Textures is the platform IO list,
        // carried on the draw data so it can be overridden.)
        if (draw_data->Textures != nullptr)
        {
            for (ImTextureData* const texture : *draw_data->Textures)
            {
                if (texture->Status != ImTextureStatus_OK)
                {
                    update_texture(*texture, command, slot);
                }
            }
        }

        if (draw_data->TotalVtxCount <= 0)
        {
            return;
        }

        VkDeviceSize const vertex_bytes =
            static_cast<VkDeviceSize>(draw_data->TotalVtxCount) * sizeof(ImDrawVert);
        VkDeviceSize const index_bytes =
            static_cast<VkDeviceSize>(draw_data->TotalIdxCount) * sizeof(ImDrawIdx);

        if (!reserve(slot.vertices, *allocator_, vertex_bytes, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT) ||
            !reserve(slot.indices, *allocator_, index_bytes, VK_BUFFER_USAGE_INDEX_BUFFER_BIT))
        {
            return;
        }

        // One contiguous copy per list; record() walks the same order and
        // keeps running offsets.
        auto* vertices = static_cast<ImDrawVert*>(slot.vertices.mapped());
        auto* indices  = static_cast<ImDrawIdx*>(slot.indices.mapped());
        for (ImDrawList const* const list : draw_data->CmdLists)
        {
            size_t const vertex_count = static_cast<size_t>(list->VtxBuffer.Size);
            size_t const index_count  = static_cast<size_t>(list->IdxBuffer.Size);

            std::memcpy(vertices, list->VtxBuffer.Data, vertex_count * sizeof(ImDrawVert));
            std::memcpy(indices, list->IdxBuffer.Data, index_count * sizeof(ImDrawIdx));
            vertices += vertex_count;
            indices += index_count;
        }
    }

    void ImGuiVulkan::setup_render_state(ImDrawData const& draw_data, VkCommandBuffer command,
                                         VkExtent2D framebuffer)
    {
        Slot const& slot = *slots_[current_slot_];

        vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_.handle());

        // Same layout shape as every other pipeline, so the renderer's binding
        // would survive; rebinding keeps this independent of that.
        VkDescriptorSet const set = bindless_->set();
        vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_.layout(), 0, 1,
                                &set, 0, nullptr);

        if (draw_data.TotalVtxCount > 0)
        {
            VkBuffer const     buffer = slot.vertices.handle();
            VkDeviceSize const offset = 0;
            vkCmdBindVertexBuffers(command, 0, 1, &buffer, &offset);
            vkCmdBindIndexBuffer(command, slot.indices.handle(), 0, kIndexType);
        }

        // Not flipped_viewport(): ImGui's space is already +Y down, matching
        // the framebuffer, and nothing here is culled by winding.
        VkViewport const viewport{
            .x        = 0.0f,
            .y        = 0.0f,
            .width    = static_cast<f32>(framebuffer.width),
            .height   = static_cast<f32>(framebuffer.height),
            .minDepth = 0.0f,
            .maxDepth = 1.0f,
        };
        vkCmdSetViewport(command, 0, 1, &viewport);

        // The visible ImGui space runs from DisplayPos (top left) to
        // DisplayPos + DisplaySize; (0,0) unless multi-viewport.
        f32 const scale_x = 2.0f / draw_data.DisplaySize.x;
        f32 const scale_y = 2.0f / draw_data.DisplaySize.y;
        push_.ui_transform = f32vec4{scale_x, scale_y, -1.0f - draw_data.DisplayPos.x * scale_x,
                                     -1.0f - draw_data.DisplayPos.y * scale_y};
        push_.ui_sampler = linear_handle_;

        // Forces the first draw to push, with its own texture.
        push_.ui_texture = BindlessSet::kInvalid;
    }

    void ImGuiVulkan::push_constants(VkCommandBuffer command) const
    {
        vkCmdPushConstants(command, pipeline_.layout(), VK_SHADER_STAGE_ALL, 0, sizeof(push_),
                           &push_);
    }

    void ImGuiVulkan::record(ImDrawData const* draw_data, VkCommandBuffer command)
    {
        if (draw_data == nullptr || draw_data->CmdListsCount == 0)
        {
            return;
        }

        // Retina-style displays scale ImGui's coordinates into pixels.
        ImVec2 const clip_offset = draw_data->DisplayPos;
        ImVec2 const clip_scale  = draw_data->FramebufferScale;

        int const width  = static_cast<int>(draw_data->DisplaySize.x * clip_scale.x);
        int const height = static_cast<int>(draw_data->DisplaySize.y * clip_scale.y);
        if (width <= 0 || height <= 0)
        {
            return;
        }

        f32 const  fb_width  = static_cast<f32>(width);
        f32 const  fb_height = static_cast<f32>(height);
        VkExtent2D framebuffer{static_cast<u32>(width), static_cast<u32>(height)};

        ImGuiPlatformIO& platform_io = ImGui::GetPlatformIO();
        render_state_                = ImGuiVulkanRenderState{
                           .command = command,
                           .layout  = pipeline_.layout(),
                           .push    = &push_,
        };
        platform_io.Renderer_RenderState = &render_state_;

        setup_render_state(*draw_data, command, framebuffer);

        // Everything was merged into one vertex and one index buffer, so
        // each list's commands are offset by what came before.
        u32 global_vertex = 0;
        u32 global_index  = 0;

        for (ImDrawList const* const list : draw_data->CmdLists)
        {
            for (ImDrawCmd const& cmd : list->CmdBuffer)
            {
                if (cmd.UserCallback != nullptr)
                {
                    if (cmd.UserCallback == callback_reset_render_state)
                    {
                        setup_render_state(*draw_data, command, framebuffer);
                    }
                    else
                    {
                        cmd.UserCallback(list, &cmd);
                    }
                    continue;
                }

                // Project the clip rectangle into framebuffer space and clamp
                // it: vkCmdSetScissor rejects anything off the edge.
                f32 const min_x = std::max((cmd.ClipRect.x - clip_offset.x) * clip_scale.x, 0.0f);
                f32 const min_y = std::max((cmd.ClipRect.y - clip_offset.y) * clip_scale.y, 0.0f);
                f32 const max_x = std::min((cmd.ClipRect.z - clip_offset.x) * clip_scale.x, fb_width);
                f32 const max_y = std::min((cmd.ClipRect.w - clip_offset.y) * clip_scale.y, fb_height);
                if (max_x <= min_x || max_y <= min_y)
                {
                    continue;
                }

                VkRect2D const scissor{
                    .offset = {static_cast<i32>(min_x), static_cast<i32>(min_y)},
                    .extent = {static_cast<u32>(max_x - min_x), static_cast<u32>(max_y - min_y)},
                };
                vkCmdSetScissor(command, 0, 1, &scissor);

                u32 const texture = static_cast<u32>(cmd.GetTexID() - 1);
                if (texture != push_.ui_texture)
                {
                    push_.ui_texture = texture;
                    push_constants(command);
                }

                vkCmdDrawIndexed(command, cmd.ElemCount, 1, cmd.IdxOffset + global_index,
                                 static_cast<i32>(cmd.VtxOffset + global_vertex), 0);
            }

            global_index += static_cast<u32>(list->IdxBuffer.Size);
            global_vertex += static_cast<u32>(list->VtxBuffer.Size);
        }

        platform_io.Renderer_RenderState = nullptr;

        // Scissor is dynamic state and leaks into whatever records next in
        // this scope; leave it covering everything, as upstream does.
        VkRect2D const full{.offset = {0, 0}, .extent = framebuffer};
        vkCmdSetScissor(command, 0, 1, &full);
    }

    void ImGuiVulkan::update_texture(ImTextureData& data, VkCommandBuffer command, Slot& slot)
    {
        if (data.Status == ImTextureStatus_WantDestroy)
        {
            // Upstream waits for UnusedFrames >= ImageCount before destroying.
            // Retiring into this slot is stricter: it is freed only when this
            // frame -- the last that could reference it -- has retired.
            if (auto* const texture = static_cast<Texture*>(data.BackendUserData))
            {
                slot.retired.push_back(texture);
            }
            data.SetTexID(ImTextureID_Invalid);
            data.BackendUserData = nullptr;
            data.SetStatus(ImTextureStatus_Destroyed);
            return;
        }

        bool const creating = data.Status == ImTextureStatus_WantCreate;

        if (creating)
        {
            if (data.Format != ImTextureFormat_RGBA32)
            {
                log::error("imgui texture format %d unsupported", static_cast<int>(data.Format));
                return;
            }

            auto texture = std::make_unique<Texture>();

            Image::Config const config{
                .format = kTextureFormat,
                .usage  = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                .aspect = VK_IMAGE_ASPECT_COLOR_BIT,
                .name   = "imgui texture",
            };
            VkExtent2D const extent{static_cast<u32>(data.Width), static_cast<u32>(data.Height)};

            if (!texture->image.init(*allocator_, *device_, config, extent))
            {
                return;
            }

            texture->handle = bindless_->add_sampled_image(texture->image.view(), kSampledLayout);
            if (texture->handle == BindlessSet::kInvalid)
            {
                return;
            }

            data.SetTexID(texture_id(texture->handle));
            data.BackendUserData = texture.release();
        }

        auto* const texture = static_cast<Texture*>(data.BackendUserData);
        if (texture == nullptr)
        {
            return;
        }

        // A new texture is uploaded whole, which also clears it; an update
        // uploads the bounding rectangle of the changes. ImGui only ever
        // writes regions no earlier frame has drawn from.
        u32 const x = creating ? 0u : data.UpdateRect.x;
        u32 const y = creating ? 0u : data.UpdateRect.y;
        u32 const w = creating ? static_cast<u32>(data.Width) : data.UpdateRect.w;
        u32 const h = creating ? static_cast<u32>(data.Height) : data.UpdateRect.h;

        size_t const       pitch = static_cast<size_t>(w) * static_cast<size_t>(data.BytesPerPixel);
        VkDeviceSize const bytes = static_cast<VkDeviceSize>(pitch) * h;

        auto staging = std::make_unique<Buffer>();
        if (!staging->init_mapped(*allocator_, bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT))
        {
            return;
        }

        auto* const destination = static_cast<std::byte*>(staging->mapped());
        for (u32 row = 0; row < h; ++row)
        {
            std::memcpy(destination + pitch * row,
                        data.GetPixelsAt(static_cast<int>(x), static_cast<int>(y + row)), pitch);
        }

        // An update overwrites texels earlier frames sampled: the barrier's
        // first scope reaches back through prior submissions to those reads.
        image_barrier(command, texture->image.handle(),
                      creating ? VK_IMAGE_LAYOUT_UNDEFINED : kSampledLayout,
                      VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                      creating ? VK_PIPELINE_STAGE_2_NONE : VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                      0, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);

        VkBufferImageCopy const region{
            .bufferOffset      = 0,
            .bufferRowLength   = 0,
            .bufferImageHeight = 0,
            .imageSubresource  = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
            .imageOffset       = {static_cast<i32>(x), static_cast<i32>(y), 0},
            .imageExtent       = {w, h, 1},
        };
        vkCmdCopyBufferToImage(command, staging->handle(), texture->image.handle(),
                               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

        image_barrier(command, texture->image.handle(), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                      kSampledLayout, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                      VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);

        slot.staging.push_back(std::move(staging));
        data.SetStatus(ImTextureStatus_OK);
    }

    void ImGuiVulkan::destroy_texture(Texture* texture)
    {
        if (texture == nullptr)
        {
            return;
        }

        bindless_->release_sampled_image(texture->handle);
        delete texture;
    }

    void ImGuiVulkan::free_retired(Slot& slot)
    {
        for (Texture* const texture : slot.retired)
        {
            destroy_texture(texture);
        }
        slot.retired.clear();
        slot.staging.clear();
    }

    void ImGuiVulkan::callback_reset_render_state(ImDrawList const* list, ImDrawCmd const* cmd)
    {
        // Never called: record() recognises it by address and does the reset
        // itself, as upstream does.
        static_cast<void>(list);
        static_cast<void>(cmd);
    }

    void ImGuiVulkan::callback_sampler_linear(ImDrawList const* list, ImDrawCmd const* cmd)
    {
        static_cast<void>(list);
        static_cast<void>(cmd);

        ImGuiVulkan* const self = backend();
        self->push_.ui_sampler  = self->linear_handle_;
        self->push_constants(self->render_state_.command);
    }

    void ImGuiVulkan::callback_sampler_nearest(ImDrawList const* list, ImDrawCmd const* cmd)
    {
        static_cast<void>(list);
        static_cast<void>(cmd);

        ImGuiVulkan* const self = backend();
        self->push_.ui_sampler  = self->nearest_handle_;
        self->push_constants(self->render_state_.command);
    }

    void ImGuiVulkan::shutdown()
    {
        if (!initialised_)
        {
            return;
        }

        // A texture shared with another context (RefCount > 1) is not ours to
        // destroy, matching upstream.
        for (ImTextureData* const data : ImGui::GetPlatformIO().Textures)
        {
            if (data->RefCount == 1)
            {
                destroy_texture(static_cast<Texture*>(data->BackendUserData));
                data->SetTexID(ImTextureID_Invalid);
                data->BackendUserData = nullptr;
                data->SetStatus(ImTextureStatus_Destroyed);
            }
        }

        for (std::unique_ptr<Slot>& slot : slots_)
        {
            free_retired(*slot);
        }
        slots_.clear();

        pipeline_.shutdown();

        VkDevice const device = device_->handle();
        for (VkSampler* const sampler : {&linear_, &nearest_})
        {
            if (*sampler != VK_NULL_HANDLE)
            {
                vkDestroySampler(device, *sampler, memory::vulkan_callbacks());
                *sampler = VK_NULL_HANDLE;
            }
        }

        ImGuiIO& io                = ImGui::GetIO();
        io.BackendRendererName     = nullptr;
        io.BackendRendererUserData = nullptr;
        io.BackendFlags &= ~(ImGuiBackendFlags_RendererHasVtxOffset |
                             ImGuiBackendFlags_RendererHasTextures);
        ImGui::GetPlatformIO().ClearRendererHandlers();

        initialised_ = false;
    }
}

#include "core/pch.hpp"

#include "render/renderer.hpp"

#include "core/log.hpp"
#include "vulkan/device.hpp"
#include "vulkan/swapchain.hpp"

namespace encke
{
    namespace
    {
        // Linear values. The swapchain is an _SRGB format, so the hardware
        // encodes these on write and they appear lighter than the numbers look.
        constexpr VkClearColorValue kClearColor{{0.03f, 0.12f, 0.18f, 1.0f}};

        constexpr u64 kNoTimeout = ~0ULL;

        VkImageMemoryBarrier2 layout_barrier(VkImage image, VkImageLayout from, VkImageLayout to,
                                             VkPipelineStageFlags2 src_stage,
                                             VkAccessFlags2 src_access,
                                             VkPipelineStageFlags2 dst_stage,
                                             VkAccessFlags2 dst_access)
        {
            return VkImageMemoryBarrier2{
                .sType         = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
                .pNext         = nullptr,
                .srcStageMask  = src_stage,
                .srcAccessMask = src_access,
                .dstStageMask  = dst_stage,
                .dstAccessMask = dst_access,
                .oldLayout     = from,
                .newLayout     = to,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .image               = image,
                .subresourceRange    = {
                    .aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT,
                    .baseMipLevel   = 0,
                    .levelCount     = 1,
                    .baseArrayLayer = 0,
                    .layerCount     = 1,
                },
            };
        }

        void submit_barrier(VkCommandBuffer command, VkImageMemoryBarrier2 const& barrier)
        {
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
    }

    Renderer::~Renderer()
    {
        shutdown();
    }

    bool Renderer::init(VulkanDevice const& device, VulkanSwapchain const& swapchain)
    {
        device_ = &device;

        VkCommandPoolCreateInfo const pool_info{
            .sType            = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
            .pNext            = nullptr,
            .flags            = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
            .queueFamilyIndex = *device.families().graphics,
        };

        VkResult result =
            vkCreateCommandPool(device.handle(), &pool_info, nullptr, &command_pool_);
        if (result != VK_SUCCESS)
        {
            log::vk_error("vkCreateCommandPool", result);
            return false;
        }

        VkCommandBufferAllocateInfo const alloc_info{
            .sType              = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
            .pNext              = nullptr,
            .commandPool        = command_pool_,
            .level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
            .commandBufferCount = kFramesInFlight,
        };

        result = vkAllocateCommandBuffers(device.handle(), &alloc_info, commands_.data());
        if (result != VK_SUCCESS)
        {
            log::vk_error("vkAllocateCommandBuffers", result);
            return false;
        }

        VkSemaphoreCreateInfo const semaphore_info{
            .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
            .pNext = nullptr,
            .flags = 0,
        };

        // Created signalled so the first wait on each frame slot returns at
        // once instead of deadlocking.
        VkFenceCreateInfo const fence_info{
            .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
            .pNext = nullptr,
            .flags = VK_FENCE_CREATE_SIGNALED_BIT,
        };

        for (u32 index = 0; index < kFramesInFlight; ++index)
        {
            result = vkCreateSemaphore(device.handle(), &semaphore_info, nullptr,
                                       &image_available_[index]);
            if (result != VK_SUCCESS)
            {
                log::vk_error("vkCreateSemaphore", result);
                return false;
            }

            result = vkCreateFence(device.handle(), &fence_info, nullptr, &in_flight_[index]);
            if (result != VK_SUCCESS)
            {
                log::vk_error("vkCreateFence", result);
                return false;
            }
        }

        return on_swapchain_changed(swapchain);
    }

    bool Renderer::on_swapchain_changed(VulkanSwapchain const& swapchain)
    {
        destroy_image_semaphores();

        VkSemaphoreCreateInfo const semaphore_info{
            .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
            .pNext = nullptr,
            .flags = 0,
        };

        render_finished_.resize(swapchain.image_count(), VK_NULL_HANDLE);
        for (VkSemaphore& semaphore : render_finished_)
        {
            VkResult const result =
                vkCreateSemaphore(device_->handle(), &semaphore_info, nullptr, &semaphore);
            if (result != VK_SUCCESS)
            {
                log::vk_error("vkCreateSemaphore", result);
                return false;
            }
        }

        return true;
    }

    bool Renderer::record(VkCommandBuffer command, VulkanSwapchain const& swapchain,
                          u32 image_index)
    {
        VkCommandBufferBeginInfo const begin{
            .sType            = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
            .pNext            = nullptr,
            .flags            = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
            .pInheritanceInfo = nullptr,
        };

        VkResult result = vkBeginCommandBuffer(command, &begin);
        if (result != VK_SUCCESS)
        {
            log::vk_error("vkBeginCommandBuffer", result);
            return false;
        }

        VkImage const image = swapchain.image(image_index);

        // The previous contents are irrelevant -- the render pass clears --
        // so discard from UNDEFINED rather than preserving PRESENT_SRC.
        submit_barrier(command,
                       layout_barrier(image,
                                      VK_IMAGE_LAYOUT_UNDEFINED,
                                      VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                      VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, 0,
                                      VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                                      VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT));

        VkRenderingAttachmentInfo const color{
            .sType              = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
            .pNext              = nullptr,
            .imageView          = swapchain.view(image_index),
            .imageLayout        = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            .resolveMode        = VK_RESOLVE_MODE_NONE,
            .resolveImageView   = VK_NULL_HANDLE,
            .resolveImageLayout = VK_IMAGE_LAYOUT_UNDEFINED,
            .loadOp             = VK_ATTACHMENT_LOAD_OP_CLEAR,
            .storeOp            = VK_ATTACHMENT_STORE_OP_STORE,
            .clearValue         = {.color = kClearColor},
        };

        VkRenderingInfo const rendering{
            .sType      = VK_STRUCTURE_TYPE_RENDERING_INFO,
            .pNext      = nullptr,
            .flags      = 0,
            .renderArea = {.offset = {0, 0}, .extent = swapchain.extent()},
            .layerCount = 1,
            .viewMask   = 0,
            .colorAttachmentCount = 1,
            .pColorAttachments    = &color,
            .pDepthAttachment     = nullptr,
            .pStencilAttachment   = nullptr,
        };

        // Dynamic rendering: no VkRenderPass, no VkFramebuffer.
        vkCmdBeginRendering(command, &rendering);
        vkCmdEndRendering(command);

        submit_barrier(command,
                       layout_barrier(image,
                                      VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                      VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                                      VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                                      VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                                      VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT, 0));

        result = vkEndCommandBuffer(command);
        if (result != VK_SUCCESS)
        {
            log::vk_error("vkEndCommandBuffer", result);
            return false;
        }

        return true;
    }

    FrameResult Renderer::draw(VulkanSwapchain const& swapchain)
    {
        VkDevice const device = device_->handle();

        VkResult result = vkWaitForFences(device, 1, &in_flight_[frame_], VK_TRUE, kNoTimeout);
        if (result != VK_SUCCESS)
        {
            log::vk_error("vkWaitForFences", result);
            return FrameResult::Error;
        }

        u32 image_index = 0;
        result = vkAcquireNextImageKHR(device, swapchain.handle(), kNoTimeout,
                                       image_available_[frame_], VK_NULL_HANDLE, &image_index);

        if (result == VK_ERROR_OUT_OF_DATE_KHR)
        {
            return FrameResult::OutOfDate;
        }
        if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR)
        {
            log::vk_error("vkAcquireNextImageKHR", result);
            return FrameResult::Error;
        }

        // Only reset once the frame is definitely going to be submitted --
        // returning early after this would leave the fence unsignalled forever.
        vkResetFences(device, 1, &in_flight_[frame_]);

        VkCommandBuffer const command = commands_[frame_];
        vkResetCommandBuffer(command, 0);

        if (!record(command, swapchain, image_index))
        {
            return FrameResult::Error;
        }

        VkSemaphoreSubmitInfo const wait{
            .sType       = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
            .pNext       = nullptr,
            .semaphore   = image_available_[frame_],
            .value       = 0,
            .stageMask   = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
            .deviceIndex = 0,
        };

        VkSemaphoreSubmitInfo const signal{
            .sType       = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
            .pNext       = nullptr,
            .semaphore   = render_finished_[image_index],
            .value       = 0,
            .stageMask   = VK_PIPELINE_STAGE_2_ALL_GRAPHICS_BIT,
            .deviceIndex = 0,
        };

        VkCommandBufferSubmitInfo const command_info{
            .sType         = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO,
            .pNext         = nullptr,
            .commandBuffer = command,
            .deviceMask    = 0,
        };

        VkSubmitInfo2 const submit{
            .sType                    = VK_STRUCTURE_TYPE_SUBMIT_INFO_2,
            .pNext                    = nullptr,
            .flags                    = 0,
            .waitSemaphoreInfoCount   = 1,
            .pWaitSemaphoreInfos      = &wait,
            .commandBufferInfoCount   = 1,
            .pCommandBufferInfos      = &command_info,
            .signalSemaphoreInfoCount = 1,
            .pSignalSemaphoreInfos    = &signal,
        };

        result = vkQueueSubmit2(device_->graphics_queue(), 1, &submit, in_flight_[frame_]);
        if (result != VK_SUCCESS)
        {
            log::vk_error("vkQueueSubmit2", result);
            return FrameResult::Error;
        }

        VkSwapchainKHR const chain = swapchain.handle();
        VkPresentInfoKHR const present{
            .sType              = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
            .pNext              = nullptr,
            .waitSemaphoreCount = 1,
            .pWaitSemaphores    = &render_finished_[image_index],
            .swapchainCount     = 1,
            .pSwapchains        = &chain,
            .pImageIndices      = &image_index,
            .pResults           = nullptr,
        };

        result = vkQueuePresentKHR(device_->present_queue(), &present);

        frame_ = (frame_ + 1) % kFramesInFlight;

        if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR)
        {
            return FrameResult::OutOfDate;
        }
        if (result != VK_SUCCESS)
        {
            log::vk_error("vkQueuePresentKHR", result);
            return FrameResult::Error;
        }

        return FrameResult::Ok;
    }

    void Renderer::destroy_image_semaphores()
    {
        if (device_ == nullptr)
        {
            return;
        }

        for (VkSemaphore& semaphore : render_finished_)
        {
            if (semaphore != VK_NULL_HANDLE)
            {
                vkDestroySemaphore(device_->handle(), semaphore, nullptr);
                semaphore = VK_NULL_HANDLE;
            }
        }

        render_finished_.clear();
    }

    void Renderer::shutdown()
    {
        if (device_ == nullptr)
        {
            return;
        }

        VkDevice const device = device_->handle();

        destroy_image_semaphores();

        for (u32 index = 0; index < kFramesInFlight; ++index)
        {
            if (image_available_[index] != VK_NULL_HANDLE)
            {
                vkDestroySemaphore(device, image_available_[index], nullptr);
                image_available_[index] = VK_NULL_HANDLE;
            }

            if (in_flight_[index] != VK_NULL_HANDLE)
            {
                vkDestroyFence(device, in_flight_[index], nullptr);
                in_flight_[index] = VK_NULL_HANDLE;
            }
        }

        if (command_pool_ != VK_NULL_HANDLE)
        {
            // Frees the command buffers with it.
            vkDestroyCommandPool(device, command_pool_, nullptr);
            command_pool_ = VK_NULL_HANDLE;
        }

        device_ = nullptr;
    }
}

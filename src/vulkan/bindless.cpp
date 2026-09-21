#include "core/pch.hpp"

#include "vulkan/bindless.hpp"

#include "core/log.hpp"
#include "core/memory.hpp"
#include "vulkan/device.hpp"

namespace encke
{
    namespace
    {
        // Far below any desktop limit (NVIDIA reports ~1M for update-after-bind
        // images) and far above what a first draft uses. Raise when a material
        // system starts registering textures.
        constexpr u32 kMaxSampledImages  = 4096;
        constexpr u32 kMaxStorageImages  = 256;
        constexpr u32 kMaxStorageBuffers = 1024;
        constexpr u32 kMaxSamplers       = 16;

        // Unused slots are legal, slots may change after the set is bound, and
        // a slot no pending command buffer touches may be rewritten.
        constexpr VkDescriptorBindingFlags kBindingFlags =
            VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT |
            VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT |
            VK_DESCRIPTOR_BINDING_UPDATE_UNUSED_WHILE_PENDING_BIT;
    }

    BindlessSet::~BindlessSet()
    {
        shutdown();
    }

    bool BindlessSet::init(VulkanDevice const& device)
    {
        device_ = &device;
        VkDevice const handle = device.handle();

        // Every stage sees every binding: G-buffer vertex shaders read object
        // buffers, compute reads images, fragment shaders will read materials.
        constexpr VkShaderStageFlags kStages = VK_SHADER_STAGE_ALL;

        VkDescriptorSetLayoutBinding const bindings[]{
            {kSampledImages,   VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,  kMaxSampledImages,  kStages, nullptr},
            {kStorageImages,   VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,  kMaxStorageImages,  kStages, nullptr},
            {kStorageBuffers,  VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, kMaxStorageBuffers, kStages, nullptr},
            {kSamplers,        VK_DESCRIPTOR_TYPE_SAMPLER,        kMaxSamplers,       kStages, nullptr},
            // Writable buffers are compute-only, but the layout can still
            // expose them everywhere; the shader declaration is what decides.
            {kWritableBuffers, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, kMaxStorageBuffers, kStages, nullptr},
        };

        VkDescriptorBindingFlags const flags[]{kBindingFlags, kBindingFlags, kBindingFlags,
                                               kBindingFlags, kBindingFlags};

        VkDescriptorSetLayoutBindingFlagsCreateInfo const flags_info{
            .sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO,
            .pNext         = nullptr,
            .bindingCount  = static_cast<u32>(std::size(flags)),
            .pBindingFlags = flags,
        };

        VkDescriptorSetLayoutCreateInfo const layout_info{
            .sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
            .pNext        = &flags_info,
            .flags        = VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT,
            .bindingCount = static_cast<u32>(std::size(bindings)),
            .pBindings    = bindings,
        };

        VkResult result = vkCreateDescriptorSetLayout(handle, &layout_info,
                                                      memory::vulkan_callbacks(), &layout_);
        if (result != VK_SUCCESS)
        {
            log::vk_error("vkCreateDescriptorSetLayout (bindless)", result);
            return false;
        }

        VkDescriptorPoolSize const sizes[]{
            {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,  kMaxSampledImages},
            {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,  kMaxStorageImages},
            {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, kMaxStorageBuffers * 2},
            {VK_DESCRIPTOR_TYPE_SAMPLER,        kMaxSamplers},
        };

        VkDescriptorPoolCreateInfo const pool_info{
            .sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
            .pNext         = nullptr,
            .flags         = VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT,
            .maxSets       = 1,
            .poolSizeCount = static_cast<u32>(std::size(sizes)),
            .pPoolSizes    = sizes,
        };

        result = vkCreateDescriptorPool(handle, &pool_info, memory::vulkan_callbacks(), &pool_);
        if (result != VK_SUCCESS)
        {
            log::vk_error("vkCreateDescriptorPool (bindless)", result);
            return false;
        }

        VkDescriptorSetAllocateInfo const alloc_info{
            .sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
            .pNext              = nullptr,
            .descriptorPool     = pool_,
            .descriptorSetCount = 1,
            .pSetLayouts        = &layout_,
        };

        result = vkAllocateDescriptorSets(handle, &alloc_info, &set_);
        if (result != VK_SUCCESS)
        {
            log::vk_error("vkAllocateDescriptorSets (bindless)", result);
            return false;
        }

        log::info("bindless set ready");
        return true;
    }

    void BindlessSet::write_image(Binding binding, u32 index, VkDescriptorType type,
                                  VkImageView view, VkImageLayout layout)
    {
        VkDescriptorImageInfo const image{
            .sampler     = VK_NULL_HANDLE,
            .imageView   = view,
            .imageLayout = layout,
        };

        VkWriteDescriptorSet const write{
            .sType            = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .pNext            = nullptr,
            .dstSet           = set_,
            .dstBinding       = binding,
            .dstArrayElement  = index,
            .descriptorCount  = 1,
            .descriptorType   = type,
            .pImageInfo       = &image,
            .pBufferInfo      = nullptr,
            .pTexelBufferView = nullptr,
        };

        vkUpdateDescriptorSets(device_->handle(), 1, &write, 0, nullptr);
    }

    u32 BindlessSet::add_sampled_image(VkImageView view, VkImageLayout layout)
    {
        if (!free_sampled_images_.empty())
        {
            u32 const handle = free_sampled_images_.back();
            free_sampled_images_.pop_back();
            update_sampled_image(handle, view, layout);
            return handle;
        }

        if (next_sampled_image_ >= kMaxSampledImages)
        {
            log::error("bindless sampled images exhausted (%u)", kMaxSampledImages);
            return kInvalid;
        }

        u32 const handle = next_sampled_image_++;
        update_sampled_image(handle, view, layout);
        return handle;
    }

    void BindlessSet::update_sampled_image(u32 handle, VkImageView view, VkImageLayout layout)
    {
        write_image(kSampledImages, handle, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, view, layout);
    }

    void BindlessSet::release_sampled_image(u32 handle)
    {
        if (handle != kInvalid)
        {
            free_sampled_images_.push_back(handle);
        }
    }

    u32 BindlessSet::add_storage_image(VkImageView view)
    {
        if (next_storage_image_ >= kMaxStorageImages)
        {
            log::error("bindless storage images exhausted (%u)", kMaxStorageImages);
            return kInvalid;
        }

        u32 const handle = next_storage_image_++;
        update_storage_image(handle, view);
        return handle;
    }

    void BindlessSet::update_storage_image(u32 handle, VkImageView view)
    {
        // Storage images are only ever accessed in GENERAL.
        write_image(kStorageImages, handle, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, view,
                    VK_IMAGE_LAYOUT_GENERAL);
    }

    u32 BindlessSet::add_buffer(Binding binding, u32& next, VkBuffer buffer, VkDeviceSize size)
    {
        if (next >= kMaxStorageBuffers)
        {
            log::error("bindless storage buffers exhausted (%u)", kMaxStorageBuffers);
            return kInvalid;
        }

        u32 const handle = next++;

        VkDescriptorBufferInfo const info{.buffer = buffer, .offset = 0, .range = size};

        VkWriteDescriptorSet const write{
            .sType            = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .pNext            = nullptr,
            .dstSet           = set_,
            .dstBinding       = binding,
            .dstArrayElement  = handle,
            .descriptorCount  = 1,
            .descriptorType   = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
            .pImageInfo       = nullptr,
            .pBufferInfo      = &info,
            .pTexelBufferView = nullptr,
        };

        vkUpdateDescriptorSets(device_->handle(), 1, &write, 0, nullptr);
        return handle;
    }

    u32 BindlessSet::add_storage_buffer(VkBuffer buffer, VkDeviceSize size)
    {
        return add_buffer(kStorageBuffers, next_storage_buffer_, buffer, size);
    }

    u32 BindlessSet::add_writable_buffer(VkBuffer buffer, VkDeviceSize size)
    {
        return add_buffer(kWritableBuffers, next_writable_buffer_, buffer, size);
    }

    u32 BindlessSet::add_sampler(VkSampler sampler)
    {
        if (next_sampler_ >= kMaxSamplers)
        {
            log::error("bindless samplers exhausted (%u)", kMaxSamplers);
            return kInvalid;
        }

        u32 const handle = next_sampler_++;

        VkDescriptorImageInfo const info{
            .sampler     = sampler,
            .imageView   = VK_NULL_HANDLE,
            .imageLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        };

        VkWriteDescriptorSet const write{
            .sType            = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .pNext            = nullptr,
            .dstSet           = set_,
            .dstBinding       = kSamplers,
            .dstArrayElement  = handle,
            .descriptorCount  = 1,
            .descriptorType   = VK_DESCRIPTOR_TYPE_SAMPLER,
            .pImageInfo       = &info,
            .pBufferInfo      = nullptr,
            .pTexelBufferView = nullptr,
        };

        vkUpdateDescriptorSets(device_->handle(), 1, &write, 0, nullptr);
        return handle;
    }

    void BindlessSet::shutdown()
    {
        if (device_ == nullptr)
        {
            return;
        }

        VkDevice const handle = device_->handle();

        // Destroying the pool frees the set with it.
        if (pool_ != VK_NULL_HANDLE)
        {
            vkDestroyDescriptorPool(handle, pool_, memory::vulkan_callbacks());
            pool_ = VK_NULL_HANDLE;
            set_  = VK_NULL_HANDLE;
        }

        if (layout_ != VK_NULL_HANDLE)
        {
            vkDestroyDescriptorSetLayout(handle, layout_, memory::vulkan_callbacks());
            layout_ = VK_NULL_HANDLE;
        }

        free_sampled_images_.clear();
        device_ = nullptr;
    }
}

#include "core/pch.hpp"

#include "vulkan/device.hpp"

#include "core/log.hpp"
#include "core/memory.hpp"
#include "vulkan/context.hpp"

#include <cstring>

namespace encke
{
    namespace
    {
        constexpr char const* kRequiredDeviceExtensions[] = {
            VK_KHR_SWAPCHAIN_EXTENSION_NAME,
        };

        // Lets the swapchain hand out a UNORM view of its sRGB images, so the
        // UI can blend in sRGB space as ImGui's styles assume. Optional:
        // without it the UI draws through the sRGB view, still colour-correct
        // because its shader decodes, but blending in linear space.
        constexpr char const* kMutableFormatExtension = VK_KHR_SWAPCHAIN_MUTABLE_FORMAT_EXTENSION_NAME;

        bool has_extension(VkPhysicalDevice device, char const* name)
        {
            u32 count = 0;
            if (vkEnumerateDeviceExtensionProperties(device, nullptr, &count, nullptr) != VK_SUCCESS)
            {
                return false;
            }

            vector<VkExtensionProperties> available(count);
            if (vkEnumerateDeviceExtensionProperties(device, nullptr, &count, available.data()) !=
                VK_SUCCESS)
            {
                return false;
            }

            for (VkExtensionProperties const& extension : available)
            {
                if (std::strcmp(extension.extensionName, name) == 0)
                {
                    return true;
                }
            }

            return false;
        }

        QueueFamilies find_queue_families(VkPhysicalDevice device, VkSurfaceKHR surface)
        {
            QueueFamilies families;

            u32 count = 0;
            vkGetPhysicalDeviceQueueFamilyProperties(device, &count, nullptr);
            vector<VkQueueFamilyProperties> properties(count);
            vkGetPhysicalDeviceQueueFamilyProperties(device, &count, properties.data());

            for (u32 index = 0; index < count; ++index)
            {
                if ((properties[index].queueFlags & VK_QUEUE_GRAPHICS_BIT) != 0 &&
                    !families.graphics.has_value())
                {
                    families.graphics = index;
                }

                VkBool32 presentable = VK_FALSE;
                if (vkGetPhysicalDeviceSurfaceSupportKHR(device, index, surface, &presentable) ==
                        VK_SUCCESS &&
                    presentable == VK_TRUE && !families.present.has_value())
                {
                    families.present = index;
                }

                if (families.complete())
                {
                    break;
                }
            }

            return families;
        }

        bool has_required_extensions(VkPhysicalDevice device)
        {
            for (char const* const required : kRequiredDeviceExtensions)
            {
                if (!has_extension(device, required))
                {
                    return false;
                }
            }

            return true;
        }

        // Self-referential through pNext, so it must never be copied or moved.
        struct FeatureChain
        {
            VkPhysicalDeviceFeatures2        core{};
            VkPhysicalDeviceVulkan11Features v11{};
            VkPhysicalDeviceVulkan12Features v12{};
            VkPhysicalDeviceVulkan13Features v13{};

            FeatureChain()
            {
                core.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
                core.pNext = &v11;
                v11.sType  = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES;
                v11.pNext  = &v12;
                v12.sType  = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
                v12.pNext  = &v13;
                v13.sType  = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
                v13.pNext  = nullptr;
            }

            FeatureChain(FeatureChain const&)            = delete;
            FeatureChain& operator=(FeatureChain const&) = delete;
        };

        // Sets every feature encke needs in `want`, checking each against
        // `have`. Each feature appears exactly once, so selection and creation
        // cannot drift apart. `missing` names the first one absent.
        //
        //  dynamicRendering, synchronization2 -- no render passes, sync2 barriers.
        //  shaderDrawParameters -- Slang lowers SV_VertexID to gl_VertexIndex
        //      and emits the DrawParameters capability with it.
        //  descriptor indexing set -- the bindless model: runtime-sized arrays,
        //      non-uniform indexing, partially bound, update-after-bind.
        //  storage image read/write without format -- bindless storage images
        //      are declared without a format qualifier.
        bool require_features(FeatureChain const& have, FeatureChain& want, char const*& missing)
        {
            bool ok = true;

            auto need = [&](VkBool32 available, VkBool32& enabled, char const* name) {
                if (available != VK_TRUE)
                {
                    if (missing == nullptr)
                    {
                        missing = name;
                    }
                    return false;
                }
                enabled = VK_TRUE;
                return true;
            };

#define ENCKE_REQUIRE(field) ok = need(have.field, want.field, #field) && ok
            ENCKE_REQUIRE(core.features.shaderStorageImageReadWithoutFormat);
            ENCKE_REQUIRE(core.features.shaderStorageImageWriteWithoutFormat);

            ENCKE_REQUIRE(v11.shaderDrawParameters);

            ENCKE_REQUIRE(v12.runtimeDescriptorArray);
            ENCKE_REQUIRE(v12.descriptorBindingPartiallyBound);
            ENCKE_REQUIRE(v12.descriptorBindingSampledImageUpdateAfterBind);
            ENCKE_REQUIRE(v12.descriptorBindingStorageImageUpdateAfterBind);
            ENCKE_REQUIRE(v12.descriptorBindingStorageBufferUpdateAfterBind);
            ENCKE_REQUIRE(v12.descriptorBindingUpdateUnusedWhilePending);
            ENCKE_REQUIRE(v12.shaderSampledImageArrayNonUniformIndexing);
            ENCKE_REQUIRE(v12.shaderStorageImageArrayNonUniformIndexing);
            ENCKE_REQUIRE(v12.shaderStorageBufferArrayNonUniformIndexing);

            ENCKE_REQUIRE(v13.dynamicRendering);
            ENCKE_REQUIRE(v13.synchronization2);
#undef ENCKE_REQUIRE

            return ok;
        }

        bool has_required_features(VkPhysicalDevice device, char const*& missing)
        {
            FeatureChain have;
            vkGetPhysicalDeviceFeatures2(device, &have.core);

            FeatureChain want;
            return require_features(have, want, missing);
        }

        bool supports_surface(VkPhysicalDevice device, VkSurfaceKHR surface)
        {
            u32 formats = 0;
            vkGetPhysicalDeviceSurfaceFormatsKHR(device, surface, &formats, nullptr);

            u32 modes = 0;
            vkGetPhysicalDeviceSurfacePresentModesKHR(device, surface, &modes, nullptr);

            return formats > 0 && modes > 0;
        }

        // Higher is better; 0 means unusable, and `reason` says why.
        u32 score_device(VkPhysicalDevice device, VkSurfaceKHR surface,
                         VkPhysicalDeviceProperties const& properties, char const*& reason)
        {
            if (properties.apiVersion < VK_API_VERSION_1_3)
            {
                reason = "Vulkan 1.3 unsupported";
                return 0;
            }
            if (!find_queue_families(device, surface).complete())
            {
                reason = "no graphics+present queue";
                return 0;
            }
            if (!has_required_extensions(device))
            {
                reason = "VK_KHR_swapchain missing";
                return 0;
            }
            if (!has_required_features(device, reason))
            {
                return 0;
            }
            if (!supports_surface(device, surface))
            {
                reason = "surface reports no formats or present modes";
                return 0;
            }

            return properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ? 1000u : 100u;
        }

        char const* device_type_name(VkPhysicalDeviceType type)
        {
            switch (type)
            {
            case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU:   return "discrete";
            case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU: return "integrated";
            case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU:    return "virtual";
            case VK_PHYSICAL_DEVICE_TYPE_CPU:            return "cpu";
            default:                                     return "other";
            }
        }
    }

    VulkanDevice::~VulkanDevice()
    {
        shutdown();
    }

    bool VulkanDevice::init(VulkanContext const& context)
    {
        u32 count = 0;
        VkResult result = vkEnumeratePhysicalDevices(context.instance(), &count, nullptr);
        if (result != VK_SUCCESS || count == 0)
        {
            log::error("no Vulkan physical devices found");
            return false;
        }

        vector<VkPhysicalDevice> devices(count);
        result = vkEnumeratePhysicalDevices(context.instance(), &count, devices.data());
        if (result != VK_SUCCESS)
        {
            log::vk_error("vkEnumeratePhysicalDevices", result);
            return false;
        }

        u32 best_score = 0;
        VkPhysicalDeviceProperties best_properties{};

        for (VkPhysicalDevice const candidate : devices)
        {
            VkPhysicalDeviceProperties properties{};
            vkGetPhysicalDeviceProperties(candidate, &properties);

            char const* reason = nullptr;
            u32 const   score  = score_device(candidate, context.surface(), properties, reason);
            if (score == 0)
            {
                log::info("gpu: %s (%s) -- unsuitable: %s", properties.deviceName,
                          device_type_name(properties.deviceType),
                          reason != nullptr ? reason : "unknown");
            }
            else
            {
                log::info("gpu: %s (%s) -- usable", properties.deviceName,
                          device_type_name(properties.deviceType));
            }

            if (score > best_score)
            {
                best_score      = score;
                physical_       = candidate;
                best_properties = properties;
            }
        }

        if (physical_ == VK_NULL_HANDLE)
        {
            log::error("no device supports Vulkan 1.3 with dynamic rendering, "
                       "synchronization2, shaderDrawParameters and a presentable "
                       "swapchain queue");
            return false;
        }

        log::info("selected %s (Vulkan %u.%u.%u)",
                  best_properties.deviceName,
                  VK_API_VERSION_MAJOR(best_properties.apiVersion),
                  VK_API_VERSION_MINOR(best_properties.apiVersion),
                  VK_API_VERSION_PATCH(best_properties.apiVersion));

        properties_ = best_properties;
        families_   = find_queue_families(physical_, context.surface());

        {
            u32 family_count = 0;
            vkGetPhysicalDeviceQueueFamilyProperties(physical_, &family_count, nullptr);
            vector<VkQueueFamilyProperties> family_properties(family_count);
            vkGetPhysicalDeviceQueueFamilyProperties(physical_, &family_count,
                                                     family_properties.data());
            timestamp_valid_bits_ = family_properties[*families_.graphics].timestampValidBits;
        }

        vector<char const*> extensions(std::begin(kRequiredDeviceExtensions),
                                       std::end(kRequiredDeviceExtensions));

        // ENCKE_UI_SRGB takes the path a GPU without the extension would, so
        // the sRGB-target UI stays exercised on hardware that has it.
        bool const force_srgb_ui  = std::getenv("ENCKE_UI_SRGB") != nullptr;
        mutable_swapchain_format_ = !force_srgb_ui && has_extension(physical_, kMutableFormatExtension);
        if (mutable_swapchain_format_)
        {
            extensions.push_back(kMutableFormatExtension);
        }
        else
        {
            log::info("%s -- the UI draws through the sRGB view and blends in linear space",
                      force_srgb_ui ? "ENCKE_UI_SRGB set" : "no swapchain mutable format");
        }

        // Graphics and present are usually the same family; submitting the
        // same index twice is invalid, so deduplicate.
        vector<u32> unique_families{*families_.graphics};
        if (*families_.present != *families_.graphics)
        {
            unique_families.push_back(*families_.present);
        }

        f32 const priority = 1.0f;
        vector<VkDeviceQueueCreateInfo> queue_infos;
        queue_infos.reserve(unique_families.size());

        for (u32 const family : unique_families)
        {
            queue_infos.push_back(VkDeviceQueueCreateInfo{
                .sType            = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
                .pNext            = nullptr,
                .flags            = 0,
                .queueFamilyIndex = family,
                .queueCount       = 1,
                .pQueuePriorities = &priority,
            });
        }

        // Selection already proved these are all present; this pass only
        // builds the chain of what to enable, from the same single list.
        FeatureChain have;
        vkGetPhysicalDeviceFeatures2(physical_, &have.core);

        FeatureChain enabled;
        char const*  missing = nullptr;
        if (!require_features(have, enabled, missing))
        {
            log::error("selected device lost feature %s", missing);
            return false;
        }

        VkDeviceCreateInfo const device_info{
            .sType                   = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
            .pNext                   = &enabled.core,
            .flags                   = 0,
            .queueCreateInfoCount    = static_cast<u32>(queue_infos.size()),
            .pQueueCreateInfos       = queue_infos.data(),
            .enabledLayerCount       = 0,
            .ppEnabledLayerNames     = nullptr,
            .enabledExtensionCount   = static_cast<u32>(extensions.size()),
            .ppEnabledExtensionNames = extensions.data(),
            // pEnabledFeatures must stay null when VkPhysicalDeviceFeatures2
            // is chained into pNext.
            .pEnabledFeatures        = nullptr,
        };

        result = vkCreateDevice(physical_, &device_info, memory::vulkan_callbacks(), &device_);
        if (result != VK_SUCCESS)
        {
            log::vk_error("vkCreateDevice", result);
            return false;
        }

        // Narrows the dispatch to this one device, skipping the loader's
        // multi-device trampolines.
        volkLoadDevice(device_);

        vkGetDeviceQueue(device_, *families_.graphics, 0, &graphics_queue_);
        vkGetDeviceQueue(device_, *families_.present, 0, &present_queue_);

        log::info("queues: graphics=%u present=%u", *families_.graphics, *families_.present);

        VkCommandPoolCreateInfo const pool_info{
            .sType            = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
            .pNext            = nullptr,
            .flags            = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT,
            .queueFamilyIndex = *families_.graphics,
        };

        result = vkCreateCommandPool(device_, &pool_info, memory::vulkan_callbacks(),
                                     &upload_pool_);
        if (result != VK_SUCCESS)
        {
            log::vk_error("vkCreateCommandPool (upload)", result);
            return false;
        }

        return true;
    }

    bool VulkanDevice::submit_immediate(function<void(VkCommandBuffer)> const& record) const
    {
        VkCommandBufferAllocateInfo const alloc_info{
            .sType              = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
            .pNext              = nullptr,
            .commandPool        = upload_pool_,
            .level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
            .commandBufferCount = 1,
        };

        VkCommandBuffer command = VK_NULL_HANDLE;
        VkResult result = vkAllocateCommandBuffers(device_, &alloc_info, &command);
        if (result != VK_SUCCESS)
        {
            log::vk_error("vkAllocateCommandBuffers (upload)", result);
            return false;
        }

        VkCommandBufferBeginInfo const begin{
            .sType            = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
            .pNext            = nullptr,
            .flags            = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
            .pInheritanceInfo = nullptr,
        };

        bool ok = true;
        result  = vkBeginCommandBuffer(command, &begin);
        if (result != VK_SUCCESS)
        {
            log::vk_error("vkBeginCommandBuffer (upload)", result);
            ok = false;
        }
        else
        {
            record(command);

            result = vkEndCommandBuffer(command);
            if (result != VK_SUCCESS)
            {
                log::vk_error("vkEndCommandBuffer (upload)", result);
                ok = false;
            }
        }

        if (ok)
        {
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
                .waitSemaphoreInfoCount   = 0,
                .pWaitSemaphoreInfos      = nullptr,
                .commandBufferInfoCount   = 1,
                .pCommandBufferInfos      = &command_info,
                .signalSemaphoreInfoCount = 0,
                .pSignalSemaphoreInfos    = nullptr,
            };

            result = vkQueueSubmit2(graphics_queue_, 1, &submit, VK_NULL_HANDLE);
            if (result != VK_SUCCESS)
            {
                log::vk_error("vkQueueSubmit2 (upload)", result);
                ok = false;
            }
            else
            {
                // Crude, and correct for startup work. A fence would let the
                // caller overlap uploads; nothing needs that yet.
                vkQueueWaitIdle(graphics_queue_);
            }
        }

        vkFreeCommandBuffers(device_, upload_pool_, 1, &command);
        return ok;
    }

    void VulkanDevice::wait_idle() const
    {
        if (device_ != VK_NULL_HANDLE)
        {
            vkDeviceWaitIdle(device_);
        }
    }

    void VulkanDevice::shutdown()
    {
        if (upload_pool_ != VK_NULL_HANDLE)
        {
            vkDestroyCommandPool(device_, upload_pool_, memory::vulkan_callbacks());
            upload_pool_ = VK_NULL_HANDLE;
        }

        if (device_ != VK_NULL_HANDLE)
        {
            vkDestroyDevice(device_, memory::vulkan_callbacks());
            device_ = VK_NULL_HANDLE;
        }

        physical_                 = VK_NULL_HANDLE;
        graphics_queue_           = VK_NULL_HANDLE;
        present_queue_            = VK_NULL_HANDLE;
        families_                 = QueueFamilies{};
        properties_               = VkPhysicalDeviceProperties{};
        timestamp_valid_bits_     = 0;
        mutable_swapchain_format_ = false;
    }
}

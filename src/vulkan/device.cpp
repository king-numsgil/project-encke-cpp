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

            for (char const* const required : kRequiredDeviceExtensions)
            {
                bool found = false;
                for (VkExtensionProperties const& extension : available)
                {
                    if (std::strcmp(extension.extensionName, required) == 0)
                    {
                        found = true;
                        break;
                    }
                }

                if (!found)
                {
                    return false;
                }
            }

            return true;
        }

        // Dynamic rendering removes VkRenderPass and VkFramebuffer entirely;
        // synchronization2 gives the cleaner barrier and submit structs. Both
        // are core in 1.3 but still opt-in features.
        bool has_required_features(VkPhysicalDevice device)
        {
            VkPhysicalDeviceVulkan13Features features13{};
            features13.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;

            VkPhysicalDeviceFeatures2 features{};
            features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
            features.pNext = &features13;

            vkGetPhysicalDeviceFeatures2(device, &features);

            return features13.dynamicRendering == VK_TRUE &&
                   features13.synchronization2 == VK_TRUE;
        }

        bool supports_surface(VkPhysicalDevice device, VkSurfaceKHR surface)
        {
            u32 formats = 0;
            vkGetPhysicalDeviceSurfaceFormatsKHR(device, surface, &formats, nullptr);

            u32 modes = 0;
            vkGetPhysicalDeviceSurfacePresentModesKHR(device, surface, &modes, nullptr);

            return formats > 0 && modes > 0;
        }

        // Higher is better; 0 means unusable.
        u32 score_device(VkPhysicalDevice device, VkSurfaceKHR surface,
                         VkPhysicalDeviceProperties const& properties)
        {
            if (properties.apiVersion < VK_API_VERSION_1_3)
            {
                return 0;
            }
            if (!find_queue_families(device, surface).complete())
            {
                return 0;
            }
            if (!has_required_extensions(device) || !has_required_features(device))
            {
                return 0;
            }
            if (!supports_surface(device, surface))
            {
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

            u32 const score = score_device(candidate, context.surface(), properties);
            log::info("gpu: %s (%s) -- %s",
                      properties.deviceName,
                      device_type_name(properties.deviceType),
                      score == 0 ? "unsuitable" : "usable");

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
                       "synchronization2 and a presentable swapchain queue");
            return false;
        }

        log::info("selected %s (Vulkan %u.%u.%u)",
                  best_properties.deviceName,
                  VK_API_VERSION_MAJOR(best_properties.apiVersion),
                  VK_API_VERSION_MINOR(best_properties.apiVersion),
                  VK_API_VERSION_PATCH(best_properties.apiVersion));

        families_ = find_queue_families(physical_, context.surface());

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

        VkPhysicalDeviceVulkan13Features features13{};
        features13.sType            = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
        features13.dynamicRendering = VK_TRUE;
        features13.synchronization2 = VK_TRUE;

        VkPhysicalDeviceFeatures2 features{};
        features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
        features.pNext = &features13;

        VkDeviceCreateInfo const device_info{
            .sType                   = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
            .pNext                   = &features,
            .flags                   = 0,
            .queueCreateInfoCount    = static_cast<u32>(queue_infos.size()),
            .pQueueCreateInfos       = queue_infos.data(),
            .enabledLayerCount       = 0,
            .ppEnabledLayerNames     = nullptr,
            .enabledExtensionCount   = static_cast<u32>(std::size(kRequiredDeviceExtensions)),
            .ppEnabledExtensionNames = kRequiredDeviceExtensions,
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
        return true;
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
        if (device_ != VK_NULL_HANDLE)
        {
            vkDestroyDevice(device_, memory::vulkan_callbacks());
            device_ = VK_NULL_HANDLE;
        }

        physical_       = VK_NULL_HANDLE;
        graphics_queue_ = VK_NULL_HANDLE;
        present_queue_  = VK_NULL_HANDLE;
        families_       = QueueFamilies{};
    }
}

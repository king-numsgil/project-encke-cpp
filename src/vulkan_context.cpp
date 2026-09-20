#include "pch.hpp"

#include "vulkan_context.hpp"

#include "log.hpp"
#include "window.hpp"

#include <cstring>

namespace encke
{
    namespace
    {
        constexpr char kValidationLayer[] = "VK_LAYER_KHRONOS_validation";

#ifdef NDEBUG
        constexpr bool kWantValidation = false;
#else
        constexpr bool kWantValidation = true;
#endif

        bool layer_present(char const* name)
        {
            u32 count = 0;
            VkResult result = vkEnumerateInstanceLayerProperties(&count, nullptr);
            if (result != VK_SUCCESS)
            {
                log::vk_error("vkEnumerateInstanceLayerProperties", result);
                return false;
            }

            vector<VkLayerProperties> layers(count);
            result = vkEnumerateInstanceLayerProperties(&count, layers.data());
            if (result != VK_SUCCESS)
            {
                log::vk_error("vkEnumerateInstanceLayerProperties", result);
                return false;
            }

            for (VkLayerProperties const& layer : layers)
            {
                if (std::strcmp(layer.layerName, name) == 0)
                {
                    return true;
                }
            }

            return false;
        }

        bool extension_present(char const* name)
        {
            u32 count = 0;
            VkResult result = vkEnumerateInstanceExtensionProperties(nullptr, &count, nullptr);
            if (result != VK_SUCCESS)
            {
                log::vk_error("vkEnumerateInstanceExtensionProperties", result);
                return false;
            }

            vector<VkExtensionProperties> extensions(count);
            result = vkEnumerateInstanceExtensionProperties(nullptr, &count, extensions.data());
            if (result != VK_SUCCESS)
            {
                log::vk_error("vkEnumerateInstanceExtensionProperties", result);
                return false;
            }

            for (VkExtensionProperties const& extension : extensions)
            {
                if (std::strcmp(extension.extensionName, name) == 0)
                {
                    return true;
                }
            }

            return false;
        }

        VKAPI_ATTR VkBool32 VKAPI_CALL on_validation_message(
            VkDebugUtilsMessageSeverityFlagBitsEXT      severity,
            VkDebugUtilsMessageTypeFlagsEXT             types,
            VkDebugUtilsMessengerCallbackDataEXT const* data,
            void*                                       user_data)
        {
            static_cast<void>(types);
            static_cast<void>(user_data);

            char const* const text = (data != nullptr && data->pMessage != nullptr)
                                         ? data->pMessage
                                         : "(no message)";

            if ((severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) != 0)
            {
                log::error("vk: %s", text);
            }
            else if ((severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) != 0)
            {
                log::warn("vk: %s", text);
            }
            else
            {
                log::info("vk: %s", text);
            }

            // VK_TRUE would abort the offending call. Validation messages are
            // for reading, not for changing behaviour.
            return VK_FALSE;
        }

        VkDebugUtilsMessengerCreateInfoEXT messenger_info()
        {
            return VkDebugUtilsMessengerCreateInfoEXT{
                .sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT,
                .pNext = nullptr,
                .flags = 0,
                .messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
                                   VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT,
                .messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                               VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                               VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT,
                .pfnUserCallback = on_validation_message,
                .pUserData       = nullptr,
            };
        }
    }

    VulkanContext::~VulkanContext()
    {
        shutdown();
    }

    bool VulkanContext::init(Window const& window)
    {
        VkResult const loader = volkInitialize();
        if (loader != VK_SUCCESS)
        {
            log::vk_error("volkInitialize", loader);
            return false;
        }

        u32 const version = volkGetInstanceVersion();
        log::info("vulkan loader %u.%u.%u",
                  VK_API_VERSION_MAJOR(version),
                  VK_API_VERSION_MINOR(version),
                  VK_API_VERSION_PATCH(version));

        return create_instance() && create_messenger() && create_surface(window);
    }

    bool VulkanContext::create_instance()
    {
        Uint32 sdl_count = 0;
        char const* const* const sdl_extensions = SDL_Vulkan_GetInstanceExtensions(&sdl_count);
        if (sdl_extensions == nullptr)
        {
            log::sdl_error("SDL_Vulkan_GetInstanceExtensions");
            return false;
        }

        vector<char const*> extensions(sdl_extensions, sdl_extensions + sdl_count);
        vector<char const*> layers;

        if constexpr (kWantValidation)
        {
            bool const has_layer     = layer_present(kValidationLayer);
            bool const has_debug_ext = extension_present(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);

            if (has_layer && has_debug_ext)
            {
                layers.push_back(kValidationLayer);
                extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
                validating_ = true;
            }
            else if (!has_layer)
            {
                log::warn("%s not installed -- running without validation. "
                          "Install the Vulkan SDK to enable it.", kValidationLayer);
            }
            else
            {
                log::warn("%s missing -- running without validation.",
                          VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
            }
        }

        for (char const* const extension : extensions)
        {
            log::info("instance extension: %s", extension);
        }

        VkApplicationInfo const app_info{
            .sType              = VK_STRUCTURE_TYPE_APPLICATION_INFO,
            .pNext              = nullptr,
            .pApplicationName   = "encke",
            .applicationVersion = VK_MAKE_API_VERSION(0, 0, 1, 0),
            .pEngineName        = "encke",
            .engineVersion      = VK_MAKE_API_VERSION(0, 0, 1, 0),
            .apiVersion         = VK_API_VERSION_1_3,
        };

        // Chaining the messenger info into pNext covers vkCreateInstance and
        // vkDestroyInstance themselves, which the standalone messenger cannot
        // -- it does not exist yet at create time and is gone by destroy time.
        VkDebugUtilsMessengerCreateInfoEXT const debug_info = messenger_info();

        VkInstanceCreateInfo const instance_info{
            .sType                   = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
            .pNext                   = validating_ ? &debug_info : nullptr,
            .flags                   = 0,
            .pApplicationInfo        = &app_info,
            .enabledLayerCount       = static_cast<u32>(layers.size()),
            .ppEnabledLayerNames     = layers.data(),
            .enabledExtensionCount   = static_cast<u32>(extensions.size()),
            .ppEnabledExtensionNames = extensions.data(),
        };

        VkResult const created = vkCreateInstance(&instance_info, nullptr, &instance_);
        if (created != VK_SUCCESS)
        {
            log::vk_error("vkCreateInstance", created);
            return false;
        }

        // Swaps the loader trampolines for the real instance-level entry
        // points. Everything below this line depends on it.
        volkLoadInstance(instance_);
        return true;
    }

    bool VulkanContext::create_messenger()
    {
        if (!validating_)
        {
            return true;
        }

        VkDebugUtilsMessengerCreateInfoEXT const info = messenger_info();
        VkResult const created =
            vkCreateDebugUtilsMessengerEXT(instance_, &info, nullptr, &messenger_);

        if (created != VK_SUCCESS)
        {
            log::vk_error("vkCreateDebugUtilsMessengerEXT", created);
            return false;
        }

        log::info("validation enabled (%s)", kValidationLayer);
        return true;
    }

    bool VulkanContext::create_surface(Window const& window)
    {
        if (!SDL_Vulkan_CreateSurface(window.handle(), instance_, nullptr, &surface_))
        {
            log::sdl_error("SDL_Vulkan_CreateSurface");
            return false;
        }

        log::info("surface created");
        return true;
    }

    void VulkanContext::shutdown()
    {
        if (surface_ != VK_NULL_HANDLE)
        {
            SDL_Vulkan_DestroySurface(instance_, surface_, nullptr);
            surface_ = VK_NULL_HANDLE;
        }

        if (messenger_ != VK_NULL_HANDLE)
        {
            vkDestroyDebugUtilsMessengerEXT(instance_, messenger_, nullptr);
            messenger_ = VK_NULL_HANDLE;
        }

        if (instance_ != VK_NULL_HANDLE)
        {
            vkDestroyInstance(instance_, nullptr);
            instance_ = VK_NULL_HANDLE;
            volkFinalize();
        }

        validating_ = false;
    }

    VkViewport flipped_viewport(f32 width, f32 height)
    {
        return VkViewport{
            .x        = 0.0f,
            .y        = height,
            .width    = width,
            .height   = -height,
            .minDepth = 0.0f,
            .maxDepth = 1.0f,
        };
    }
}

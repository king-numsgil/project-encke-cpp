#include "pch.hpp"

namespace
{
    constexpr int   kWindowWidth  = 1280;
    constexpr int   kWindowHeight = 720;
    constexpr char  kWindowTitle[] = "encke";

    struct App
    {
        SDL_Window*  window   = nullptr;
        VkInstance   instance = VK_NULL_HANDLE;
        VkSurfaceKHR surface  = VK_NULL_HANDLE;
    };

    void fail_sdl(char const* what)
    {
        std::fprintf(stderr, "%s: %s\n", what, SDL_GetError());
    }

    void fail_vk(char const* what, VkResult result)
    {
        std::fprintf(stderr, "%s: VkResult %d\n", what, static_cast<int>(result));
    }

    bool init_window(App& app)
    {
        if (!SDL_Init(SDL_INIT_VIDEO))
        {
            fail_sdl("SDL_Init");
            return false;
        }

        app.window = SDL_CreateWindow(
            kWindowTitle, kWindowWidth, kWindowHeight,
            SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE);

        if (app.window == nullptr)
        {
            fail_sdl("SDL_CreateWindow");
            return false;
        }

        return true;
    }

    bool init_vulkan(App& app)
    {
        // Loads the Vulkan loader and resolves vkGetInstanceProcAddr. Nothing
        // else in the program may call a Vulkan entry point before this.
        VkResult const loader = volkInitialize();
        if (loader != VK_SUCCESS)
        {
            fail_vk("volkInitialize", loader);
            return false;
        }

        uint32_t const loader_version = volkGetInstanceVersion();
        std::printf("Vulkan loader %u.%u.%u\n",
                    VK_API_VERSION_MAJOR(loader_version),
                    VK_API_VERSION_MINOR(loader_version),
                    VK_API_VERSION_PATCH(loader_version));

        // SDL decides which surface extensions this platform's window needs.
        Uint32 sdl_extension_count = 0;
        char const* const* const sdl_extensions =
            SDL_Vulkan_GetInstanceExtensions(&sdl_extension_count);

        if (sdl_extensions == nullptr)
        {
            fail_sdl("SDL_Vulkan_GetInstanceExtensions");
            return false;
        }

        std::vector<char const*> const extensions(
            sdl_extensions, sdl_extensions + sdl_extension_count);

        VkApplicationInfo const app_info{
            .sType              = VK_STRUCTURE_TYPE_APPLICATION_INFO,
            .pNext              = nullptr,
            .pApplicationName   = kWindowTitle,
            .applicationVersion = VK_MAKE_API_VERSION(0, 0, 1, 0),
            .pEngineName        = kWindowTitle,
            .engineVersion      = VK_MAKE_API_VERSION(0, 0, 1, 0),
            .apiVersion         = VK_API_VERSION_1_3,
        };

        VkInstanceCreateInfo const instance_info{
            .sType                   = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
            .pNext                   = nullptr,
            .flags                   = 0,
            .pApplicationInfo        = &app_info,
            .enabledLayerCount       = 0,
            .ppEnabledLayerNames     = nullptr,
            .enabledExtensionCount   = static_cast<uint32_t>(extensions.size()),
            .ppEnabledExtensionNames = extensions.data(),
        };

        VkResult const created = vkCreateInstance(&instance_info, nullptr, &app.instance);
        if (created != VK_SUCCESS)
        {
            fail_vk("vkCreateInstance", created);
            return false;
        }

        // Replaces the loader's trampolines with the real instance-level
        // entry points. Device-level functions need volkLoadDevice later.
        volkLoadInstance(app.instance);

        if (!SDL_Vulkan_CreateSurface(app.window, app.instance, nullptr, &app.surface))
        {
            fail_sdl("SDL_Vulkan_CreateSurface");
            return false;
        }

        return true;
    }

    void run(App const& app)
    {
        bool running = true;
        while (running)
        {
            SDL_Event event;
            while (SDL_PollEvent(&event))
            {
                if (event.type == SDL_EVENT_QUIT)
                {
                    running = false;
                }
                else if (event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED &&
                         event.window.windowID == SDL_GetWindowID(app.window))
                {
                    running = false;
                }
            }

            // No swapchain yet, so there is nothing to present. Yield rather
            // than spin a core on an empty event loop.
            SDL_Delay(16);
        }
    }

    void shutdown(App& app)
    {
        if (app.surface != VK_NULL_HANDLE)
        {
            SDL_Vulkan_DestroySurface(app.instance, app.surface, nullptr);
            app.surface = VK_NULL_HANDLE;
        }

        if (app.instance != VK_NULL_HANDLE)
        {
            vkDestroyInstance(app.instance, nullptr);
            app.instance = VK_NULL_HANDLE;
            volkFinalize();
        }

        if (app.window != nullptr)
        {
            SDL_DestroyWindow(app.window);
            app.window = nullptr;
        }

        SDL_Quit();
    }
}

int main(int argc, char** argv)
{
    static_cast<void>(argc);
    static_cast<void>(argv);

    App app;

    if (!init_window(app) || !init_vulkan(app))
    {
        shutdown(app);
        return EXIT_FAILURE;
    }

    run(app);
    shutdown(app);
    return EXIT_SUCCESS;
}

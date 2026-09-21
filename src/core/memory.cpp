#include "core/pch.hpp"

#include "core/memory.hpp"

#include "core/log.hpp"

#include <imgui.h>

#if ENCKE_USE_MIMALLOC

#   include <mimalloc.h>

// Defines global operator new/delete in terms of mimalloc. This must appear in
// exactly one translation unit in the whole program, and at file scope.
#   include <mimalloc-new-delete.h>

#endif

namespace encke::memory
{
#if ENCKE_USE_MIMALLOC

    namespace
    {
        void* VKAPI_PTR vk_allocate(void* user, size_t size, size_t alignment,
                                    VkSystemAllocationScope scope)
        {
            static_cast<void>(user);
            static_cast<void>(scope);
            return mi_malloc_aligned(size, alignment);
        }

        void* VKAPI_PTR vk_reallocate(void* user, void* original, size_t size, size_t alignment,
                                      VkSystemAllocationScope scope)
        {
            static_cast<void>(user);
            static_cast<void>(scope);

            // Vulkan defines both degenerate cases, and mi_realloc_aligned
            // does not promise the same behaviour, so handle them here.
            if (original == nullptr)
            {
                return mi_malloc_aligned(size, alignment);
            }

            if (size == 0)
            {
                mi_free(original);
                return nullptr;
            }

            return mi_realloc_aligned(original, size, alignment);
        }

        void VKAPI_PTR vk_free(void* user, void* memory)
        {
            static_cast<void>(user);
            // mi_free handles null, and handles aligned blocks without the
            // separate _aligned_free that MSVC's CRT would demand.
            mi_free(memory);
        }

        void* imgui_allocate(size_t size, void* user)
        {
            static_cast<void>(user);
            return mi_malloc(size);
        }

        void imgui_free(void* memory, void* user)
        {
            static_cast<void>(user);
            mi_free(memory);
        }

        constinit VkAllocationCallbacks const g_vulkan_callbacks{
            .pUserData             = nullptr,
            .pfnAllocation         = vk_allocate,
            .pfnReallocation       = vk_reallocate,
            .pfnFree               = vk_free,
            .pfnInternalAllocation = nullptr,
            .pfnInternalFree       = nullptr,
        };
    }

    bool install_sdl_allocator()
    {
        if (!SDL_SetMemoryFunctions(mi_malloc, mi_calloc, mi_realloc, mi_free))
        {
            log::sdl_error("SDL_SetMemoryFunctions");
            return false;
        }

        return true;
    }

    void install_imgui_allocator()
    {
        ImGui::SetAllocatorFunctions(imgui_allocate, imgui_free);
    }

    VkAllocationCallbacks const* vulkan_callbacks()
    {
        return &g_vulkan_callbacks;
    }

    char const* backend_name()
    {
        return "mimalloc";
    }

#else

    bool install_sdl_allocator()
    {
        return true;
    }

    void install_imgui_allocator()
    {
    }

    VkAllocationCallbacks const* vulkan_callbacks()
    {
        return nullptr;
    }

    char const* backend_name()
    {
        return "system";
    }

#endif
}

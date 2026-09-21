#pragma once

// Allocator routing. When ENCKE_USE_MIMALLOC is 1, global operator new/delete,
// SDL and Vulkan host allocations all land in mimalloc. When it is 0 every
// entry point here degrades to the platform default.

namespace encke::memory
{
    // Points SDL3 at mimalloc. Must run before SDL_Init -- SDL refuses to
    // swap allocators once it has allocated anything.
    bool install_sdl_allocator();

    // Points Dear ImGui, and ImPlot through it, at mimalloc. Must run before
    // the first ImGui context exists: a block freed by a different allocator
    // from the one that made it is heap corruption.
    void install_imgui_allocator();

    // Host-memory callbacks for vkCreate*/vkDestroy*. Returns nullptr when
    // mimalloc is disabled, which is exactly what Vulkan reads as "use the
    // driver's own allocator", so call sites need no branch.
    //
    // The same pointer must be passed to the destroy call as to the create
    // call that made the object.
    VkAllocationCallbacks const* vulkan_callbacks();

    // For the startup log.
    char const* backend_name();
}

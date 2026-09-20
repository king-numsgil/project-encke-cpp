#pragma once

// volk.h must precede anything that pulls in vulkan.h. SDL3/SDL_vulkan.h
// declares its own Vulkan handle typedefs unless VULKAN_H_ is already defined,
// and volk.h errors outright if vulkan.h was included without VK_NO_PROTOTYPES.
#include <volk.h>

#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>

// Pulls in glm and the std headers behind the global type aliases.
#include "core/types.hpp"

#include <cstdio>
#include <cstdlib>
#include <memory>

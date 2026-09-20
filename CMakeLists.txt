cmake_minimum_required(VERSION 3.25)
project(encke LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(Vulkan REQUIRED)
find_package(volk CONFIG REQUIRED)
find_package(VulkanMemoryAllocator CONFIG REQUIRED)
find_package(SDL3 CONFIG REQUIRED)
find_package(glm CONFIG REQUIRED)

add_executable(encke
    src/main.cpp
    src/app.cpp
    src/log.cpp
    src/window.cpp
    src/vulkan_context.cpp
)

target_include_directories(encke PRIVATE src)

# volk.h, SDL3 and glm are stable and heavy; main.cpp is not yet either.
target_precompile_headers(encke PRIVATE src/pch.hpp)

set_target_properties(encke PROPERTIES
    POSITION_INDEPENDENT_CODE ON
    CXX_VISIBILITY_PRESET     "hidden"
)

target_link_libraries(encke PUBLIC
    # Headers only, never Vulkan::Vulkan. The import library for vulkan-1.dll
    # defines vkCreateInstance and friends as real symbols, and volk defines the
    # same names as function pointers -- linking both is a multiple-definition
    # error. volk loads the entry points at runtime instead.
    Vulkan::Headers volk::volk
    GPUOpen::VulkanMemoryAllocator
    SDL3::SDL3
    glm::glm
)

target_compile_definitions(encke PUBLIC
    # volk owns every Vulkan entry point. Without this, vulkan.h declares
    # prototypes that link against the static loader and silently bypass volk,
    # and volk.h itself hard-errors if it sees vulkan.h included without it.
    VK_NO_PROTOTYPES

    GLM_FORCE_EXPLICIT_CTOR
    GLM_FORCE_SIZE_T_LENGTH
    GLM_FORCE_AVX2

    # Vulkan's clip space is depth 0..1. GLM defaults to OpenGL's -1..1, which
    # silently halves the usable depth range and breaks depth testing. This is
    # the one Vulkan-correctness flag GLM actually provides; the Y axis has no
    # equivalent and must be handled at the projection (see CLAUDE.md).
    GLM_FORCE_DEPTH_ZERO_TO_ONE

    # Unlocks the gtx/ extensions -- dual quaternions among them. They carry no
    # API stability promise across GLM releases.
    GLM_ENABLE_EXPERIMENTAL

    # Deliberately NOT GLM_FORCE_DEFAULT_ALIGNED_GENTYPES. It would not remove a
    # capability (packed_vec3 stays 12 bytes under it), it flips what vec3 means
    # by default -- and the mistakes are not symmetric. Aligned by default makes
    # a plain {vec3,vec3,vec2} vertex silently 48 bytes at offsets 0/16/32, so a
    # forgotten packed_vec3 corrupts GPU uploads; packed by default means a
    # forgotten aligned_vec4 only costs SIMD. Opt in per type via
    # glm::aligned_* (gtc/type_aligned.hpp) where SIMD or std140/std430 is
    # actually wanted.
)

target_compile_options(encke PUBLIC
    $<$<CXX_COMPILER_ID:GNU,Clang>:-mavx2 -mfma>
    $<$<CXX_COMPILER_ID:MSVC>:/arch:AVX2>

    # --- warnings: GCC / Clang ---
    $<$<CXX_COMPILER_ID:GNU,Clang>:
        -Wall -Wextra -Wpedantic -Wunused

        -Wreorder
        -Wignored-qualifiers
        -Wmissing-braces
        -Wreturn-type
        -Wswitch
        -Wuninitialized
        -Wmissing-field-initializers
        -Wshadow
        -Wconversion
        -Wsign-conversion
        -Wold-style-cast

        -Wno-switch-default
        -Wno-double-promotion
        -Wno-unknown-pragmas
        -Wno-unused-parameter
    >

    # --- warnings: GCC-specific ---
    $<$<CXX_COMPILER_ID:GNU>:
        -Wmaybe-uninitialized
        -Wno-literal-suffix
        -Wno-misleading-indentation
    >

    # --- optimization level override in Debug (GCC/Clang default to -O0) ---
    $<$<AND:$<CXX_COMPILER_ID:GNU,Clang>,$<CONFIG:Debug>>:-Og>

    # --- warnings: MSVC ---
    $<$<CXX_COMPILER_ID:MSVC>:
        /W4
        # /WX

        /wd4251   # class needs dll-interface to be used by clients of another class
        /wd4592   # symbol will be dynamically initialized (implementation limitation)
        /wd4201   # nonstandard extension: nameless struct/union (GLM does this)
    >

    # --- MSVC Release tuning ---
    $<$<AND:$<CXX_COMPILER_ID:MSVC>,$<CONFIG:Release>>:/Gw /GL /GF>
    $<$<AND:$<CXX_COMPILER_ID:MSVC>,$<CONFIG:RelWithDebInfo>>:/Gw /GL /GF>
)

target_compile_definitions(encke PUBLIC
    $<$<CXX_COMPILER_ID:GNU,Clang>:$<$<CONFIG:Debug>:_GLIBCXX_ASSERTIONS>>
    $<$<CXX_COMPILER_ID:MSVC>:_SCL_SECURE_NO_WARNINGS _CRT_SECURE_NO_WARNINGS>
)

enable_testing()

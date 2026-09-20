cmake_minimum_required(VERSION 3.25)
project(encke LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Routes operator new/delete, SDL and Vulkan host allocations through mimalloc.
# See src/core/memory.cpp. Off under the sanitizers -- ASan cannot instrument
# what it does not allocate, so leaving this on there trades heap diagnostics
# for allocator throughput, which is the wrong way round in a debug build.
option(ENCKE_USE_MIMALLOC "Route allocations through mimalloc" ON)

find_package(Vulkan REQUIRED)
find_package(volk CONFIG REQUIRED)
find_package(VulkanMemoryAllocator CONFIG REQUIRED)
find_package(SDL3 CONFIG REQUIRED)
find_package(glm CONFIG REQUIRED)

if(ENCKE_USE_MIMALLOC)
    # Built via CPM rather than vcpkg, because mimalloc 3.x needs a build-time
    # define that vcpkg cannot deliver. Its CMake sets MI_MINGW_UCRT64 only from
    # $ENV{MSYSTEM}, and vcpkg scrubs the environment for port builds (the
    # triplet passes through PATH alone). Without it, mingw ends up with two
    # process-init paths -- a DLL_PROCESS_ATTACH callback registered through
    # data sections plus a constructor attribute -- and mimalloc aborts on
    # startup with `assertion failed: "mi_out_default == NULL"`.
    set(CPM_SOURCE_CACHE "${CMAKE_SOURCE_DIR}/.cpm" CACHE PATH
        "Shared download cache so each build directory does not refetch sources")
    include(cmake/CPM.cmake)

    CPMAddPackage(
        NAME mimalloc
        GITHUB_REPOSITORY microsoft/mimalloc
        VERSION 3.5.3
        OPTIONS
            "MI_BUILD_SHARED OFF"
            "MI_BUILD_STATIC ON"
            "MI_BUILD_OBJECT OFF"
            "MI_BUILD_TESTS OFF"
            "MI_OVERRIDE OFF"
            "MI_INSTALL_TOPLEVEL OFF"
    )

    foreach(mi_target mimalloc-static mimalloc)
        if(NOT TARGET ${mi_target})
            continue()
        endif()

        # The define upstream would have set itself given MSYSTEM. Removing this
        # reintroduces the double process-init and the startup assertion; it was
        # A/B'd on one build tree with nothing else changed. Its only other
        # effect upstream is a minject step on a dynamic test executable, which
        # is not built here.
        if(MINGW)
            target_compile_definitions(${mi_target} PRIVATE MI_MINGW_UCRT64=1)
        endif()

        # We do not maintain mimalloc, and building it in-tree puts its
        # diagnostics in our output where they would mask ours. GCC emits a
        # -Wstringop-overflow= false positive from its atomics at -O2.
        target_compile_options(${mi_target} PRIVATE -w)
    endforeach()
endif()

add_executable(encke
    src/main.cpp
    src/app.cpp

    src/core/log.cpp
    src/core/memory.cpp

    src/platform/window.cpp

    src/render/renderer.cpp

    src/vulkan/context.cpp
    src/vulkan/device.cpp
    src/vulkan/swapchain.cpp
)

target_include_directories(encke PRIVATE src)

# volk.h, SDL3 and glm are stable and heavy; our own code is not yet either.
target_precompile_headers(encke PRIVATE src/core/pch.hpp)

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

if(ENCKE_USE_MIMALLOC)
    target_link_libraries(encke PUBLIC
        $<IF:$<TARGET_EXISTS:mimalloc-static>,mimalloc-static,mimalloc>
    )
endif()

target_compile_definitions(encke PUBLIC
    ENCKE_USE_MIMALLOC=$<BOOL:${ENCKE_USE_MIMALLOC}>
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

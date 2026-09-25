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

# Shaders are Slang, compiled to SPIR-V at build time. slangc ships with the
# Vulkan SDK, so it costs no extra dependency -- but it does mean the build
# needs the SDK, where everything else comes from the vcpkg manifest. That is a
# deliberate trade; fail here rather than at link time with something cryptic.
find_program(ENCKE_SLANGC
    NAMES slangc
    HINTS "$ENV{VULKAN_SDK}/Bin" "$ENV{VULKAN_SDK}/bin"
    DOC "Slang compiler, shipped with the Vulkan SDK"
)
if(NOT ENCKE_SLANGC)
    message(FATAL_ERROR
        "slangc not found. It ships with the Vulkan SDK -- install the SDK and make sure "
        "VULKAN_SDK is set, or put slangc on PATH.")
endif()

find_package(Vulkan REQUIRED)
find_package(volk CONFIG REQUIRED)
find_package(VulkanMemoryAllocator CONFIG REQUIRED)
find_package(SDL3 CONFIG REQUIRED)
find_package(SDL3_image CONFIG REQUIRED)
find_package(fastgltf CONFIG REQUIRED)
find_package(glm CONFIG REQUIRED)
find_package(cpuinfo CONFIG REQUIRED)
find_package(EnTT CONFIG REQUIRED)
# The overlay port in ports/fastnoise2: patched for a lattice offset, AVX2
# only, strict floating point.
find_package(FastNoise2 CONFIG REQUIRED)

# Header-only, and the port installs no CMake package: just the header.
find_path(BS_THREAD_POOL_INCLUDE_DIR BS_thread_pool.hpp REQUIRED)

set(CPM_SOURCE_CACHE "${CMAKE_SOURCE_DIR}/.cpm" CACHE PATH
    "Shared download cache so each build directory does not refetch sources")
include(cmake/CPM.cmake)

if(ENCKE_USE_MIMALLOC)
    # Built via CPM rather than vcpkg, because mimalloc 3.x needs a build-time
    # define that vcpkg cannot deliver. Its CMake sets MI_MINGW_UCRT64 only from
    # $ENV{MSYSTEM}, and vcpkg scrubs the environment for port builds (the
    # triplet passes through PATH alone). Without it, mingw ends up with two
    # process-init paths -- a DLL_PROCESS_ATTACH callback registered through
    # data sections plus a constructor attribute -- and mimalloc aborts on
    # startup with `assertion failed: "mi_out_default == NULL"`.
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

# --- Dear ImGui (docking branch) and ImPlot ------------------------------------

# CPM, not vcpkg. The vcpkg port's vulkan-binding links Vulkan::Vulkan, whose
# import library defines vkCreateInstance and friends as real symbols -- the same
# multiple-definition clash with volk that the encke target avoids below. So the
# sources are fetched bare and built here. ImPlot has to follow, because it must
# compile against this exact ImGui rather than vcpkg's.
#
# Only the SDL3 platform backend comes from upstream. The Vulkan renderer backend
# is a fork in src/ui/imgui_vulkan, version-locked to this tag.
#
# Docking branch for the docking and multi-viewport features. Neither is enabled
# yet; taking the branch now keeps a later switch from being a dependency change.
CPMAddPackage(
    NAME imgui
    GITHUB_REPOSITORY ocornut/imgui
    GIT_TAG v1.92.9b-docking
    DOWNLOAD_ONLY YES
)

CPMAddPackage(
    NAME implot
    GITHUB_REPOSITORY epezent/implot
    GIT_TAG v1.0
    DOWNLOAD_ONLY YES
)

add_library(encke_imgui STATIC
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_demo.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    # The Vulkan renderer backend is not built from here: encke carries a fork
    # of it in src/ui/imgui_vulkan, on VMA and the bindless set.
    ${imgui_SOURCE_DIR}/backends/imgui_impl_sdl3.cpp

    ${implot_SOURCE_DIR}/implot.cpp
    ${implot_SOURCE_DIR}/implot_demo.cpp
    ${implot_SOURCE_DIR}/implot_items.cpp
)

# SYSTEM, so our -Wold-style-cast and -Wconversion do not fire inside their
# headers wherever encke includes them.
target_include_directories(encke_imgui SYSTEM PUBLIC
    ${imgui_SOURCE_DIR}
    ${imgui_SOURCE_DIR}/backends
    ${implot_SOURCE_DIR}
)

# Not IMGUI_DISABLE_OBSOLETE_FUNCTIONS: ImPlot v1.0 still calls the old
# AddPolyline(flags, thickness) argument order, which that define deletes.

target_link_libraries(encke_imgui PUBLIC SDL3::SDL3)

# Not ours to fix; keep their diagnostics out of our build output.
target_compile_options(encke_imgui PRIVATE -w)

# Everything but main(), as an object library so encke and the tests link the
# same objects. Not a static library: core/memory.cpp replaces operator new,
# and a static archive would link that object only if something happened to
# reference it, leaving a binary half on mimalloc.
add_library(encke_core OBJECT
    src/app.cpp

    src/assets/asset_manager.cpp
    src/assets/worker.cpp

    src/core/log.cpp
    src/core/memory.cpp

    src/platform/cpu.cpp
    src/platform/thread.cpp
    src/platform/window.cpp
    src/platform/worker_pool.cpp

    src/render/camera.cpp
    src/render/extract.cpp
    src/render/fly_camera.cpp
    src/render/geometry_pool.cpp
    src/render/gltf.cpp
    src/render/material.cpp
    src/render/mesh.cpp
    src/render/pipeline.cpp
    src/render/pixels.cpp
    src/render/renderer.cpp
    src/render/scene.cpp
    src/render/shadows.cpp

    src/terrain/benchmark.cpp
    src/terrain/detail_noise.cpp
    src/terrain/macro_field.cpp
    src/terrain/planet.cpp
    src/terrain/surface_nets.cpp
    src/terrain/terrain_field.cpp

    src/ui/image_window.cpp
    src/ui/imgui_layer.cpp
    src/ui/imgui_vulkan.cpp
    src/ui/stats_window.cpp

    src/vulkan/allocator.cpp
    src/vulkan/bindless.cpp
    src/vulkan/buffer.cpp
    src/vulkan/context.cpp
    src/vulkan/device.cpp
    src/vulkan/image.cpp
    src/vulkan/staging.cpp
    src/vulkan/swapchain.cpp
    src/vulkan/texture.cpp
    src/vulkan/timestamps.cpp

    src/world/bodies.cpp
    src/world/transform.cpp
)

add_executable(encke src/main.cpp)
target_link_libraries(encke PRIVATE encke_core)

# Terrain must come out bit-identical on every machine for multiplayer. GCC
# in ISO mode already leaves a * b + c as two roundings, but clang fuses it
# into an FMA wherever -mfma allows, so the lattice split and the field would
# differ between the two compilers. FastNoise2 itself is built strict by its
# port.
set_source_files_properties(
    src/terrain/detail_noise.cpp
    src/terrain/macro_field.cpp
    src/terrain/surface_nets.cpp
    src/terrain/terrain_field.cpp
    PROPERTIES COMPILE_OPTIONS "$<$<CXX_COMPILER_ID:GNU,Clang>:-ffp-contract=off>"
)

target_include_directories(encke_core PUBLIC src)
target_include_directories(encke_core SYSTEM PRIVATE ${BS_THREAD_POOL_INCLUDE_DIR})

# Textures are read from the source tree, not copied beside the executable the
# way SPIR-V is: they are tens of megabytes, change rarely, and a copy per
# build directory would buy nothing on a development machine.
target_compile_definitions(encke_core PUBLIC
    ENCKE_ASSET_DIR="${CMAKE_CURRENT_SOURCE_DIR}/assets"
)

# --- shaders -----------------------------------------------------------------

# Lands beside the executable; the runtime resolves SPIR-V against
# SDL_GetBasePath() + "shaders/". A generator expression would be more precise
# but is not allowed in a custom command's OUTPUT, and the single-config
# generators used here put the executable directly in the binary directory.
set(ENCKE_SHADER_DIR "${CMAKE_CURRENT_BINARY_DIR}/shaders")

# One .slang file yields one .spv holding every [shader(...)] entry point in it.
# -fvk-use-entrypoint-name keeps their names; without it they all become "main"
# and the pipeline cannot tell them apart.
function(encke_add_shader out_spv source)
    get_filename_component(shader_name "${source}" NAME_WE)
    set(spv "${ENCKE_SHADER_DIR}/${shader_name}.spv")

    add_custom_command(
        OUTPUT "${spv}"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${ENCKE_SHADER_DIR}"
        COMMAND "${ENCKE_SLANGC}" "${CMAKE_CURRENT_SOURCE_DIR}/${source}"
                -I "${CMAKE_CURRENT_SOURCE_DIR}/shaders"
                -target spirv
                -profile spirv_1_5
                -fvk-use-entrypoint-name
                # GLM is column-major and so is SPIR-V by default; Slang would
                # otherwise use HLSL's row-major and silently transpose every
                # matrix pushed from C++.
                -matrix-layout-column-major
                # 41012 is "profile implicitly upgraded": Slang's own capability
                # bookkeeping, not extra SPIR-V capabilities. The emitted module
                # is unchanged.
                # 39001 is "explicit binding overlap": lib/bindless.slang views
                # the sampler binding as both plain and comparison samplers on
                # purpose, which is legal descriptor aliasing in Vulkan.
                -warnings-disable 41012,39001
                -depfile "${spv}.d"
                -o "${spv}"
        DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/${source}"
        # slangc writes the modules it imported here, so editing shaders/lib/*
        # rebuilds whatever imported it without listing dependencies by hand.
        DEPFILE "${spv}.d"
        COMMENT "slangc ${source}"
        VERBATIM
    )

    set(${out_spv} "${spv}" PARENT_SCOPE)
endfunction()

encke_add_shader(ENCKE_GBUFFER_SPV shaders/gbuffer.slang)
encke_add_shader(ENCKE_CLUSTER_SPV shaders/cluster_build.slang)
encke_add_shader(ENCKE_LIGHTING_SPV shaders/lighting.slang)
encke_add_shader(ENCKE_TONEMAP_SPV shaders/tonemap.slang)
encke_add_shader(ENCKE_DEBUG_VIEWS_SPV shaders/debug_views.slang)
encke_add_shader(ENCKE_IMGUI_SPV shaders/imgui.slang)
encke_add_shader(ENCKE_SHADOW_DEPTH_SPV shaders/shadow_depth.slang)
encke_add_shader(ENCKE_EXPOSURE_SPV shaders/exposure.slang)

add_custom_target(encke_shaders DEPENDS
    "${ENCKE_GBUFFER_SPV}" "${ENCKE_CLUSTER_SPV}"
    "${ENCKE_LIGHTING_SPV}" "${ENCKE_TONEMAP_SPV}"
    "${ENCKE_DEBUG_VIEWS_SPV}" "${ENCKE_IMGUI_SPV}"
    "${ENCKE_SHADOW_DEPTH_SPV}" "${ENCKE_EXPOSURE_SPV}"
)
add_dependencies(encke encke_shaders)

# volk.h, SDL3 and glm are stable and heavy; our own code is not yet either.
# main.cpp is one file and includes the header directly, so encke has none.
target_precompile_headers(encke_core PRIVATE src/core/pch.hpp)

set_target_properties(encke_core encke PROPERTIES
    POSITION_INDEPENDENT_CODE ON
    CXX_VISIBILITY_PRESET     "hidden"
)

target_link_libraries(encke_core PUBLIC
    # Headers only, never Vulkan::Vulkan. The import library for vulkan-1.dll
    # defines vkCreateInstance and friends as real symbols, and volk defines the
    # same names as function pointers -- linking both is a multiple-definition
    # error. volk loads the entry points at runtime instead.
    Vulkan::Headers volk::volk
    GPUOpen::VulkanMemoryAllocator
    SDL3::SDL3
    $<IF:$<TARGET_EXISTS:SDL3_image::SDL3_image-shared>,SDL3_image::SDL3_image-shared,SDL3_image::SDL3_image-static>
    fastgltf::fastgltf
    glm::glm
    cpuinfo::cpuinfo
    EnTT::EnTT
    FastNoise2::FastNoise
    encke_imgui
)

# std::jthread needs the platform's thread library. MinGW brings it along
# implicitly and glibc 2.34+ folds it into libc, but older glibc does not: the
# build links, then the first thread throws "Enable multithreading to use
# std::thread: Operation not permitted". Correct and harmless everywhere.
find_package(Threads REQUIRED)
target_link_libraries(encke_core PUBLIC Threads::Threads)

if(ENCKE_USE_MIMALLOC)
    target_link_libraries(encke_core PUBLIC
        $<IF:$<TARGET_EXISTS:mimalloc-static>,mimalloc-static,mimalloc>
    )

    # The C++ runtime must be linked statically, or mimalloc replaces
    # operator new only in code compiled into encke.exe. A replacement does
    # not reach into a DLL on Windows: libstdc++-6.dll keeps its own new,
    # which is malloc, so anything it allocates and encke's inlined code
    # frees crosses heaps. std::filesystem::path did exactly that and
    # corrupted mimalloc's pages; MI_DEBUG=FULL caught it as
    # "mi_free_size: invalid pointer". Statically linked, libstdc++'s
    # references to operator new resolve to mimalloc's like everything else.
    # PUBLIC, so every executable linking encke_core gets it, tests included.
    if(MINGW)
        target_link_options(encke_core PUBLIC -static-libstdc++ -static-libgcc)
    endif()
endif()

target_compile_definitions(encke_core PUBLIC
    ENCKE_USE_MIMALLOC=$<BOOL:${ENCKE_USE_MIMALLOC}>
)

target_compile_definitions(encke_core PUBLIC
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

target_compile_options(encke_core PUBLIC
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

target_compile_definitions(encke_core PUBLIC
    $<$<CXX_COMPILER_ID:GNU,Clang>:$<$<CONFIG:Debug>:_GLIBCXX_ASSERTIONS>>
    $<$<CXX_COMPILER_ID:MSVC>:_SCL_SECURE_NO_WARNINGS _CRT_SECURE_NO_WARNINGS>
)

# --- tests -------------------------------------------------------------------

enable_testing()

find_package(Catch2 3 CONFIG REQUIRED)
include(Catch)

add_executable(encke_tests
    tests/render/camera_test.cpp
    tests/terrain/detail_noise_test.cpp
    tests/terrain/fastnoise_allocator_test.cpp
    tests/terrain/octree_test.cpp
    tests/terrain/perlin_reference.cpp
    tests/terrain/planet_test.cpp
    tests/terrain/surface_nets_test.cpp
    tests/terrain/terrain_field_test.cpp
    tests/world/transform_test.cpp
)

# Every flag, define and link option comes through encke_core, so the tests
# build exactly as the app does. Catch2WithMain supplies main().
target_link_libraries(encke_tests PRIVATE encke_core Catch2::Catch2WithMain)

# Its own, not REUSE_FROM encke_core: Catch2's include directories differ,
# and clang rejects a PCH built under other flags.
target_precompile_headers(encke_tests PRIVATE src/core/pch.hpp)

set_target_properties(encke_tests PROPERTIES
    POSITION_INDEPENDENT_CODE ON
    CXX_VISIBILITY_PRESET     "hidden"
)

# PRE_TEST lists the tests when ctest runs rather than as a post-build step,
# so a build never executes the binary: under clang-sanitize that needs the
# ASan runtime DLL on PATH, which only a test run can be expected to set.
catch_discover_tests(encke_tests DISCOVERY_MODE PRE_TEST)

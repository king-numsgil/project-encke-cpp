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

add_executable(encke main.cpp)

set_target_properties(encke PROPERTIES
    POSITION_INDEPENDENT_CODE ON
    CXX_VISIBILITY_PRESET     "hidden"
)

target_link_libraries(encke PUBLIC
    Vulkan::Vulkan volk::volk
    GPUOpen::VulkanMemoryAllocator
    SDL3::SDL3
    glm::glm
)

target_compile_definitions(encke PUBLIC
    GLM_FORCE_EXPLICIT_CTOR
    GLM_FORCE_SIZE_T_LENGTH
    GLM_FORCE_AVX2
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

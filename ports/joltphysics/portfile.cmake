# Jolt Physics v5.6.0, unpatched. The overlay exists for its build options,
# which the upstream vcpkg port hardcodes or does not expose.
vcpkg_from_github(
    OUT_SOURCE_PATH SOURCE_PATH
    REPO jrouwe/JoltPhysics
    REF "v${VERSION}"
    SHA512 bc6f2436bef91a6ffd09eee98186be645f6e9a9b3f65a7a645abf00432ac40ada03320c1fe946661817a94eda372cf8d88e4889991cdd25f1c16ecf9a4486677
    HEAD_REF master
)

# Every JPH_* define Jolt's exported target carries must be the same in the
# library and in encke, or the two disagree on class layouts. Jolt checks the
# ones it knows about at runtime (VerifyJoltVersionID). Several of its
# defines are per configuration, evaluated in the consumer's configuration,
# and encke's release is RelWithDebInfo, which Jolt's lists leave out; so
# every such option is off here, and the library is the same in every build.
#
# DOUBLE_PRECISION: positions are f64 (RVec3), everything else stays f32.
# CROSS_PLATFORM_DETERMINISTIC: -ffp-contract=off and no FMA intrinsics, so
#   simulations agree bit for bit across compilers, which multiplayer needs.
#   Headers inlined into encke need the same flag; see CMakeLists.txt.
# FLOATING_POINT_EXCEPTIONS_ENABLED: Debug and Release only, so off.
# DEBUG_RENDERER, PROFILER: Debug and Release only, so off.
# INTERPROCEDURAL_OPTIMIZATION: LTO objects are compiler IR, readable only by
#   the compiler that wrote them; plain objects link under either toolchain.
vcpkg_cmake_configure(
    SOURCE_PATH "${SOURCE_PATH}/Build"
    OPTIONS
        -DDOUBLE_PRECISION=ON
        -DCROSS_PLATFORM_DETERMINISTIC=ON
        -DFLOATING_POINT_EXCEPTIONS_ENABLED=OFF
        -DDEBUG_RENDERER_IN_DEBUG_AND_RELEASE=OFF
        -DPROFILER_IN_DEBUG_AND_RELEASE=OFF
        -DINTERPROCEDURAL_OPTIMIZATION=OFF
        -DUSE_AVX512=OFF
        -DENABLE_ALL_WARNINGS=OFF
        -DOVERRIDE_CXX_FLAGS=OFF
        -DGENERATE_DEBUG_SYMBOLS=OFF
        -DTARGET_UNIT_TESTS=OFF
        -DTARGET_HELLO_WORLD=OFF
        -DTARGET_PERFORMANCE_TEST=OFF
        -DTARGET_SAMPLES=OFF
        -DTARGET_VIEWER=OFF
        -DJPH_USE_DX12=OFF
        -DJPH_USE_VK=OFF
        -DJPH_USE_MTL=OFF
    # Optimised like FastNoise2's debug library: Jolt's maths is SIMD
    # wrappers that only cost nothing once inlined. NDEBUG stays unset, so
    # the library keeps JPH_DEBUG and matches encke's debug builds.
    OPTIONS_DEBUG
        "-DCMAKE_CXX_FLAGS_DEBUG=-O2 -g"
)

vcpkg_cmake_install()
vcpkg_cmake_config_fixup(PACKAGE_NAME Jolt CONFIG_PATH lib/cmake/Jolt)

file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/debug/include")
file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/debug/share")

vcpkg_install_copyright(FILE_LIST "${SOURCE_PATH}/LICENSE")

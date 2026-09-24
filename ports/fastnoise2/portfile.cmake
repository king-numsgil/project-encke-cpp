# FastNoise2 v1.1.1, the latest release, so that a released Node Editor
# encodes node trees the way this library decodes them.
#
# lattice-offset.patch adds Perlin::SetLatticeOffset, which shifts the integer
# lattice the 3D hash sees (the lattice split in src/terrain/detail_noise), and
# a FASTNOISE2_FEATURE_SETS cache variable so only the SIMD level encke pins is
# compiled.
vcpkg_from_github(
    OUT_SOURCE_PATH SOURCE_PATH
    REPO Auburn/FastNoise2
    REF 903c1f2d2f9d53ddce94cd223f32727d9ab3aeaa
    SHA512 0bafb3d00d972d12909639dfb23f54b30077f3451fbeadb8ebcd09be57bc3630f5355636d8e0bab1994bb9a1e1e6c72231e11770f946e7095da718cbb6c3a638
    HEAD_REF master
    PATCHES
        lattice-offset.patch
)

# FastNoise2 fetches FastSIMD through CPM at configure time. A port must not
# download during its build, so the commit FastNoise2 pins is fetched here and
# handed to CPM as a local source.
vcpkg_from_github(
    OUT_SOURCE_PATH FASTSIMD_SOURCE_PATH
    REPO Auburn/FastSIMD
    REF 16450dae9528727e500e7254f635a671f9c7ee2d
    SHA512 e7d97a4bff643e99c93cb7a01d488bd7f8f187425f3d68e7cdba18d032ca2154c19e61346dd29fb69a94a708f219d5d357b544b7375f2b9c28d9694ebd7d0b4d
    HEAD_REF master
    PATCHES
        # -Wa,-muse-unaligned-vector-move on GCC only. It works around GNU as
        # and GCC's unaligned AVX spills on Windows; clang64's integrated
        # assembler rejects it, and clang realigns the stack itself.
        fastsimd-mingw-clang.patch
)

# STRICT_FP drops -ffast-math so every build computes the same bits, which
# multiplayer needs. AVX2 alone is compiled: encke already requires it, and
# a runtime choice between levels would let two machines differ.
vcpkg_cmake_configure(
    SOURCE_PATH "${SOURCE_PATH}"
    OPTIONS
        -DFASTNOISE2_STANDALONE_PROJECT=OFF
        -DFASTNOISE2_TOOLS=OFF
        -DFASTNOISE2_TESTS=OFF
        -DFASTNOISE2_UTILITY=OFF
        -DFASTNOISE2_STRICT_FP=ON
        -DFASTNOISE2_FEATURE_SETS=AVX2
        -DFASTSIMD_EXAMPLES=OFF
        -DFASTSIMD_TESTS=OFF
        "-DCPM_FastSIMD_SOURCE=${FASTSIMD_SOURCE_PATH}"
        -DCPM_DOWNLOAD_ALL=OFF
        -DCPM_USE_LOCAL_PACKAGES=OFF
    # The debug library is optimised too. FastSIMD is intrinsic wrappers that
    # only cost nothing once inlined; at -O0 every SIMD operation is a call,
    # and debug builds meshed the planet about twenty times slower than
    # release. Only the flags differ: it stays the debug configuration, so
    # its runtime settings still match encke's debug builds.
    OPTIONS_DEBUG
        "-DCMAKE_CXX_FLAGS_DEBUG=-O2 -g"
        "-DCMAKE_C_FLAGS_DEBUG=-O2 -g"
)

vcpkg_cmake_install()
vcpkg_cmake_config_fixup(PACKAGE_NAME FastNoise2 CONFIG_PATH lib/cmake/FastNoise2)

file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/debug/include")
file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/debug/share")

vcpkg_install_copyright(FILE_LIST "${SOURCE_PATH}/LICENSE")

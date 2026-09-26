#include "core/pch.hpp"

// AddressSanitizer's defaults for this binary, which the runtime reads
// before main. Only under ASan; elsewhere the file is empty.
//
// Container-overflow checking is off. It relies on every piece of code that
// touches a std::vector annotating it, and the vcpkg ports are built without
// the sanitizers: FastNoise2's graph decoder grows a vector through its own
// uninstrumented code while the linker hands it some of our instrumented
// template instances, and the two disagree about where the vector ends. The
// macro graphs first grew deep enough to make it reallocate with the ridged
// mountains, and ASan aborted on a false positive inside FastNoise2. Real
// out-of-bounds accesses are still caught by the redzones.

#if defined(__has_feature)
#if __has_feature(address_sanitizer)
#define ENCKE_ASAN 1
#endif
#endif
#if defined(__SANITIZE_ADDRESS__)
#define ENCKE_ASAN 1
#endif

#if defined(ENCKE_ASAN)
extern "C" __attribute__((visibility("default"), used)) char const* __asan_default_options()
{
    return "detect_container_overflow=0";
}
#endif

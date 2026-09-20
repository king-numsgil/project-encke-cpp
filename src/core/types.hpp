#pragma once

// Project prelude: short scalar names and GLM aliases in the global namespace.
// Included by pch.hpp, so every translation unit sees it.

#include <cstddef>
#include <cstdint>

#include <array>
#include <bit>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/gtx/dual_quaternion.hpp>
// Not aliased below on purpose -- glm::aligned_vec4 at the use site is meant to
// be conspicuous. This just makes it reachable without a second include.
#include <glm/gtc/type_aligned.hpp>

// -- std ---------------------------------------------------------------------

using std::array;
using std::function;
using std::optional;
using std::span;
using std::string;
using std::string_view;
using std::vector;

using std::nullopt;
using std::ref;
using std::reference_wrapper;

using std::bit_cast;

using std::byte;

using std::literals::operator""s;
using std::literals::operator""sv;

// `byte` is safe here only because nothing in the PCH reaches windows.h --
// volk.h tests VK_USE_PLATFORM_WIN32_KHR but never defines it, and SDL3's
// public headers do not include it either. Defining that macro (to build a
// VkWin32SurfaceCreateInfoKHR by hand rather than via SDL) would pull in
// rpcndr.h, whose own global `byte` collides with this. Drop this line if that
// day comes.

// -- scalars -----------------------------------------------------------------

using i8  = std::int8_t;
using u8  = std::uint8_t;

using i16 = std::int16_t;
using u16 = std::uint16_t;

using i32 = std::int32_t;
using u32 = std::uint32_t;

using i64 = std::int64_t;
using u64 = std::uint64_t;

using f32 = float;
using f64 = double;

// -- vectors -----------------------------------------------------------------

// glm::defaultp is packed here (no GLM_FORCE_DEFAULT_ALIGNED_GENTYPES), so
// vec3<f32> is 12 bytes and uploads to a vertex buffer as-is. Where 16-byte
// alignment is wanted -- SIMD, or std140/std430 mirror structs -- use
// glm::aligned_vec3 / glm::aligned_vec4 from <glm/gtc/type_aligned.hpp>
// explicitly rather than changing what these mean.

template<class T> using vec2 = glm::vec<2, T, glm::defaultp>;
template<class T> using vec3 = glm::vec<3, T, glm::defaultp>;
template<class T> using vec4 = glm::vec<4, T, glm::defaultp>;

using bvec2 = vec2<bool>;
using bvec3 = vec3<bool>;
using bvec4 = vec4<bool>;

using i8vec2 = vec2<i8>;
using i8vec3 = vec3<i8>;
using i8vec4 = vec4<i8>;
using u8vec2 = vec2<u8>;
using u8vec3 = vec3<u8>;
using u8vec4 = vec4<u8>;

using i16vec2 = vec2<i16>;
using i16vec3 = vec3<i16>;
using i16vec4 = vec4<i16>;
using u16vec2 = vec2<u16>;
using u16vec3 = vec3<u16>;
using u16vec4 = vec4<u16>;

using i32vec2 = vec2<i32>;
using i32vec3 = vec3<i32>;
using i32vec4 = vec4<i32>;
using u32vec2 = vec2<u32>;
using u32vec3 = vec3<u32>;
using u32vec4 = vec4<u32>;

using i64vec2 = vec2<i64>;
using i64vec3 = vec3<i64>;
using i64vec4 = vec4<i64>;
using u64vec2 = vec2<u64>;
using u64vec3 = vec3<u64>;
using u64vec4 = vec4<u64>;

using f32vec2 = vec2<f32>;
using f32vec3 = vec3<f32>;
using f32vec4 = vec4<f32>;
using f64vec2 = vec2<f64>;
using f64vec3 = vec3<f64>;
using f64vec4 = vec4<f64>;

// -- matrices ----------------------------------------------------------------

// GLM spells these mat<C, R>: columns first, same as Magnum and GLSL. mat4x3 is
// four columns of three rows.

template<class T> using mat2x2 = glm::mat<2, 2, T, glm::defaultp>;
template<class T> using mat2x3 = glm::mat<2, 3, T, glm::defaultp>;
template<class T> using mat2x4 = glm::mat<2, 4, T, glm::defaultp>;
template<class T> using mat3x2 = glm::mat<3, 2, T, glm::defaultp>;
template<class T> using mat3x3 = glm::mat<3, 3, T, glm::defaultp>;
template<class T> using mat3x4 = glm::mat<3, 4, T, glm::defaultp>;
template<class T> using mat4x2 = glm::mat<4, 2, T, glm::defaultp>;
template<class T> using mat4x3 = glm::mat<4, 3, T, glm::defaultp>;
template<class T> using mat4x4 = glm::mat<4, 4, T, glm::defaultp>;

template<class T> using mat2 = mat2x2<T>;
template<class T> using mat3 = mat3x3<T>;
template<class T> using mat4 = mat4x4<T>;

using f32mat2x2 = mat2x2<f32>;
using f32mat2x3 = mat2x3<f32>;
using f32mat2x4 = mat2x4<f32>;
using f32mat3x2 = mat3x2<f32>;
using f32mat3x3 = mat3x3<f32>;
using f32mat3x4 = mat3x4<f32>;
using f32mat4x2 = mat4x2<f32>;
using f32mat4x3 = mat4x3<f32>;
using f32mat4x4 = mat4x4<f32>;

using f64mat2x2 = mat2x2<f64>;
using f64mat2x3 = mat2x3<f64>;
using f64mat2x4 = mat2x4<f64>;
using f64mat3x2 = mat3x2<f64>;
using f64mat3x3 = mat3x3<f64>;
using f64mat3x4 = mat3x4<f64>;
using f64mat4x2 = mat4x2<f64>;
using f64mat4x3 = mat4x3<f64>;
using f64mat4x4 = mat4x4<f64>;

using f32mat2 = f32mat2x2;
using f32mat3 = f32mat3x3;
using f32mat4 = f32mat4x4;
using f64mat2 = f64mat2x2;
using f64mat3 = f64mat3x3;
using f64mat4 = f64mat4x4;

// -- quaternions -------------------------------------------------------------

// Watch the asymmetry: the constructor is qua(w, x, y, z) but the default
// memory layout is x, y, z, w. Constructing is w-first, uploading is w-last.
// GLM_FORCE_QUAT_DATA_WXYZ would align them, at the cost of making the memory
// order unusual on the shader side.

template<class T> using quat = glm::qua<T, glm::defaultp>;

using f32quat = quat<f32>;
using f64quat = quat<f64>;

// gtx, gated behind GLM_ENABLE_EXPERIMENTAL (set in CMakeLists.txt).
template<class T> using dquat = glm::tdualquat<T, glm::defaultp>;

using f32dquat = dquat<f32>;
using f64dquat = dquat<f64>;

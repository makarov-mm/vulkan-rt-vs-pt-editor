#pragma once
#include <array>
#include <cstdint>

#include "mat4.h"

// -----------------------------------------------------------------------------
//  GPU-visible data. Every struct mirrors a std430 / std140 block in the
//  shaders; the static_asserts below turn a host/GLSL mismatch into a build
//  error instead of garbage on screen.
// -----------------------------------------------------------------------------
using Float4 = std::array<float, 4>;
using UInt4  = std::array<uint32_t, 4>;

// shaders/closesthit.rchit: struct Vertex (two vec4)
struct Vertex {
    Float4 pos{};   // xyz = position
    Float4 nrm{};   // xyz = normal
};

// shaders/raygen_common.glsl: uniform FrameUniforms
struct FrameUniforms {
    Mat4   viewInverse;
    Mat4   projInverse;
    Float4 light{};        // xyz = position, w = radius
    Float4 lightColor{};   // rgb = tint, w = emitted radiance (HDR)
    UInt4  frame{};        // x = accumulated frame index (0 = restart), y = free-running frame,
                           // z = samples per pixel and frame, w = max path length
    Float4 marquee{};      // rubber-band rectangle in pixels (x0,y0,x1,y1); x1 < x0 disables
};

// shaders/closesthit.rchit: struct ObjData. Indexed by gl_InstanceCustomIndexEXT.
struct GpuObj {
    Float4 color{};    // rgb = albedo, w = selected flag (1 = highlighted)
    Float4 params{};   // x = material id (see Material), y = reflectivity
    UInt4  mesh{};     // x = first index of this instance's mesh in the shared index buffer
};

// shaders/atrous.comp: push_constant PC
struct DenoisePC {
    Float4  marquee{};        // re-composited by the final pass
    int32_t step      = 1;    // a-trous tap spacing in pixels
    int32_t firstPass = 0;    // demodulate the input by the primary-hit albedo
    int32_t finalPass = 0;    // remodulate, tone map and write the display image
};

static_assert(sizeof(Vertex)        == 32,  "Vertex must match the std430 layout (two vec4)");
static_assert(sizeof(FrameUniforms) == 192, "FrameUniforms must match the std140 block");
static_assert(sizeof(GpuObj)        == 48,  "GpuObj must match ObjData (three 16-byte rows)");
static_assert(sizeof(DenoisePC)     == 28,  "DenoisePC must match the push_constant block in atrous.comp");

// Marquee value meaning "no rectangle" (x1 < x0).
inline constexpr Float4 MARQUEE_OFF{ 0.0f, 0.0f, -1.0f, -1.0f };

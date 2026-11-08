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

// shaders/payload.glsl: struct Vertex (two vec4)
struct Vertex {
    Float4 pos{};   // xyz = position
    Float4 nrm{};   // xyz = normal
};

// shaders/raygen_common.glsl: uniform CameraProperties
struct UBO {
    Mat4   viewInverse;
    Mat4   projInverse;
    Float4 lightPos{};   // xyz = position, w = radius
    Float4 params{};     // x = time, y = max path length, z = light emission, w = samples/frame
    UInt4  frame{};      // x = accumulated frame index, y = free-running frame
    Float4 marquee{};    // rubber-band rectangle in pixels (x0,y0,x1,y1); x1 < x0 disables
};

// shaders/closesthit.rchit: struct ObjData. Indexed by gl_InstanceCustomIndexEXT.
struct GpuObj {
    Float4 color{};    // rgb = albedo, w = selected flag (1 = highlighted)
    Float4 params{};   // x = material id (see Material), y = reflectivity
    UInt4  mesh{};     // x = first index of this instance's mesh in the shared index buffer
};

// shaders/atrous.comp: push_constant PC (the vec4 is 16-byte aligned, hence the padding).
struct DenoisePC {
    int32_t sizeX = 0, sizeY = 0, step = 0, finalPass = 0;
    float   exposure  = 1.0f;
    int32_t firstPass = 0;
    float   pad0 = 0.0f, pad1 = 0.0f;
    Float4  marquee{};
};

static_assert(sizeof(Vertex)    == 32,  "Vertex must match the std430 layout (two vec4)");
static_assert(sizeof(UBO)       == 192, "UBO must match the CameraProperties block");
static_assert(sizeof(GpuObj)    == 48,  "GpuObj must match ObjData (three 16-byte rows)");
static_assert(sizeof(DenoisePC) == 48,  "DenoisePC must match the push_constant block in atrous.comp");

// Marquee value meaning "no rectangle" (x1 < x0).
inline constexpr Float4 MARQUEE_OFF{ 0.0f, 0.0f, -1.0f, -1.0f };

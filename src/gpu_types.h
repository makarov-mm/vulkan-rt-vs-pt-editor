#pragma once
#include <cstdint>

#include "mat4.h"

// -----------------------------------------------------------------------------
//  GPU-visible data (matches the GLSL std430 layouts)
// -----------------------------------------------------------------------------
struct Vertex 
{
    float pos[3];  float _pad0;   // vec4: xyz position
    float nrm[3];  float _pad1;   // vec4: xyz normal
};

struct UBO 
{
    Mat4     viewInverse;
    Mat4     projInverse;
    float    lightPos[4];  // xyz = position, w = radius (area light)
    float    params[4];    // x = time, y = maxBounces, z = light emission strength, w = samples/frame
    uint32_t frame[4];     // x = accumulated frame index, y = free-running frame
    float    marquee[4];   // rubber-band rectangle in pixels (x0,y0,x1,y1); x1 < x0 disables
};

// Per-instance shading data, indexed by gl_InstanceCustomIndexEXT.
struct GpuObj 
{
    float    color[4];     // rgb = albedo, w = selected flag (1 = highlighted)
    float    params[4];    // x = matId (0 floor, 1 solid, 2 glass, 3 emissive), y = reflectivity
    uint32_t mesh[4];      // x = first index of this instance's mesh in the shared index buffer
};

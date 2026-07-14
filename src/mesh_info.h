#pragma once
#include <cstdint>

#include "platform.h"
#include "vec3.h"
#include "buffer.h"

// One prototype mesh: its slice of the shared vertex/index buffers, local
// bounds for picking/placement, and the BLAS built over it.
struct MeshInfo
{
    uint32_t firstIndex = 0, indexCount = 0;
    uint32_t firstVert  = 0, vertCount  = 0;
    Vec3  bcenter;            // local bounding-sphere centre
    float bradius = 1.0f;     // local bounding-sphere radius
    float minY    = 0.0f;     // lowest local point (for on-floor placement)
    VkAccelerationStructureKHR blas = VK_NULL_HANDLE;
    Buffer blasBuf;
    VkDeviceAddress blasAddr = 0;
};

#pragma once
#include "platform.h"

// -----------------------------------------------------------------------------
//  Ray tracing entry points (loaded at runtime; not exported by the loader lib)
// -----------------------------------------------------------------------------
extern PFN_vkGetBufferDeviceAddress                   pvkGetBufferDeviceAddress;
extern PFN_vkCreateAccelerationStructureKHR           pvkCreateAccelerationStructure;
extern PFN_vkDestroyAccelerationStructureKHR          pvkDestroyAccelerationStructure;
extern PFN_vkGetAccelerationStructureBuildSizesKHR    pvkGetAccelerationStructureBuildSizes;
extern PFN_vkCmdBuildAccelerationStructuresKHR        pvkCmdBuildAccelerationStructures;
extern PFN_vkGetAccelerationStructureDeviceAddressKHR pvkGetAccelerationStructureDeviceAddress;
extern PFN_vkCreateRayTracingPipelinesKHR             pvkCreateRayTracingPipelines;
extern PFN_vkGetRayTracingShaderGroupHandlesKHR       pvkGetRayTracingShaderGroupHandles;
extern PFN_vkCmdTraceRaysKHR                          pvkCmdTraceRays;

void loadRayTracingFunctions(VkDevice dev);

#pragma once
#include "platform.h"

// -----------------------------------------------------------------------------
//  VK_KHR_acceleration_structure / VK_KHR_ray_tracing_pipeline entry points.
//  Extension functions are not exported by the loader library, so they are
//  fetched per device at startup.
// -----------------------------------------------------------------------------
extern PFN_vkCreateAccelerationStructureKHR           pvkCreateAccelerationStructure;
extern PFN_vkDestroyAccelerationStructureKHR          pvkDestroyAccelerationStructure;
extern PFN_vkGetAccelerationStructureBuildSizesKHR    pvkGetAccelerationStructureBuildSizes;
extern PFN_vkCmdBuildAccelerationStructuresKHR        pvkCmdBuildAccelerationStructures;
extern PFN_vkGetAccelerationStructureDeviceAddressKHR pvkGetAccelerationStructureDeviceAddress;
extern PFN_vkCreateRayTracingPipelinesKHR             pvkCreateRayTracingPipelines;
extern PFN_vkGetRayTracingShaderGroupHandlesKHR       pvkGetRayTracingShaderGroupHandles;
extern PFN_vkCmdTraceRaysKHR                          pvkCmdTraceRays;

void loadRayTracingFunctions(VkDevice dev);

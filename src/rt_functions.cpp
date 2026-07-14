 #include "rt_functions.h"

#include <stdexcept>

PFN_vkGetBufferDeviceAddress                   pvkGetBufferDeviceAddress                = nullptr;
PFN_vkCreateAccelerationStructureKHR           pvkCreateAccelerationStructure           = nullptr;
PFN_vkDestroyAccelerationStructureKHR          pvkDestroyAccelerationStructure          = nullptr;
PFN_vkGetAccelerationStructureBuildSizesKHR    pvkGetAccelerationStructureBuildSizes    = nullptr;
PFN_vkCmdBuildAccelerationStructuresKHR        pvkCmdBuildAccelerationStructures        = nullptr;
PFN_vkGetAccelerationStructureDeviceAddressKHR pvkGetAccelerationStructureDeviceAddress = nullptr;
PFN_vkCreateRayTracingPipelinesKHR             pvkCreateRayTracingPipelines             = nullptr;
PFN_vkGetRayTracingShaderGroupHandlesKHR       pvkGetRayTracingShaderGroupHandles       = nullptr;
PFN_vkCmdTraceRaysKHR                          pvkCmdTraceRays                          = nullptr;

#define LOAD_DEV(name, var) \
    var = (PFN_##name)vkGetDeviceProcAddr(dev, #name); \
    if (!var) throw std::runtime_error("Failed to load " #name);

void loadRayTracingFunctions(VkDevice dev)
{
    LOAD_DEV(vkGetBufferDeviceAddress,                   pvkGetBufferDeviceAddress);
    LOAD_DEV(vkCreateAccelerationStructureKHR,           pvkCreateAccelerationStructure);
    LOAD_DEV(vkDestroyAccelerationStructureKHR,          pvkDestroyAccelerationStructure);
    LOAD_DEV(vkGetAccelerationStructureBuildSizesKHR,    pvkGetAccelerationStructureBuildSizes);
    LOAD_DEV(vkCmdBuildAccelerationStructuresKHR,        pvkCmdBuildAccelerationStructures);
    LOAD_DEV(vkGetAccelerationStructureDeviceAddressKHR, pvkGetAccelerationStructureDeviceAddress);
    LOAD_DEV(vkCreateRayTracingPipelinesKHR,             pvkCreateRayTracingPipelines);
    LOAD_DEV(vkGetRayTracingShaderGroupHandlesKHR,       pvkGetRayTracingShaderGroupHandles);
    LOAD_DEV(vkCmdTraceRaysKHR,                          pvkCmdTraceRays);
}

#include "rt_functions.h"

#include <stdexcept>
#include <string>

PFN_vkCreateAccelerationStructureKHR           pvkCreateAccelerationStructure           = nullptr;
PFN_vkDestroyAccelerationStructureKHR          pvkDestroyAccelerationStructure          = nullptr;
PFN_vkGetAccelerationStructureBuildSizesKHR    pvkGetAccelerationStructureBuildSizes    = nullptr;
PFN_vkCmdBuildAccelerationStructuresKHR        pvkCmdBuildAccelerationStructures        = nullptr;
PFN_vkGetAccelerationStructureDeviceAddressKHR pvkGetAccelerationStructureDeviceAddress = nullptr;
PFN_vkCreateRayTracingPipelinesKHR             pvkCreateRayTracingPipelines             = nullptr;
PFN_vkGetRayTracingShaderGroupHandlesKHR       pvkGetRayTracingShaderGroupHandles       = nullptr;
PFN_vkCmdTraceRaysKHR                          pvkCmdTraceRays                          = nullptr;

namespace {
template <class Fn>
void load(VkDevice dev, Fn& fn, const char* name) {
    fn = reinterpret_cast<Fn>(vkGetDeviceProcAddr(dev, name));
    if (!fn) throw std::runtime_error(std::string("Failed to load ") + name);
}
} // namespace

void loadRayTracingFunctions(VkDevice dev) {
    load(dev, pvkCreateAccelerationStructure,           "vkCreateAccelerationStructureKHR");
    load(dev, pvkDestroyAccelerationStructure,          "vkDestroyAccelerationStructureKHR");
    load(dev, pvkGetAccelerationStructureBuildSizes,    "vkGetAccelerationStructureBuildSizesKHR");
    load(dev, pvkCmdBuildAccelerationStructures,        "vkCmdBuildAccelerationStructuresKHR");
    load(dev, pvkGetAccelerationStructureDeviceAddress, "vkGetAccelerationStructureDeviceAddressKHR");
    load(dev, pvkCreateRayTracingPipelines,             "vkCreateRayTracingPipelinesKHR");
    load(dev, pvkGetRayTracingShaderGroupHandles,       "vkGetRayTracingShaderGroupHandlesKHR");
    load(dev, pvkCmdTraceRays,                          "vkCmdTraceRaysKHR");
}

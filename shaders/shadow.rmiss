#version 460
#extension GL_EXT_ray_tracing : require

layout(location = 1) rayPayloadInEXT float shadowed;

void main() {
    // The shadow ray reached its end: nothing blocks the light.
    shadowed = 0.0;
}

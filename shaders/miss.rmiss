#version 460
#extension GL_EXT_ray_tracing : require
#extension GL_GOOGLE_include_directive : require

#include "payload.glsl"

layout(location = 0) rayPayloadInEXT HitPayload payload;

void main() {
    // Negative t signals "no hit"; the ray-gen shader adds the sky colour.
    payload.t = -1.0;
}

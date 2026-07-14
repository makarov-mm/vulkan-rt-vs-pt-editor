#version 460
#extension GL_EXT_ray_tracing : require

// Must match the HitPayload declared in raygen.rgen / closesthit.rchit
// field-for-field: payloads on the same location are shared memory, and a
// shorter struct here is undefined behaviour (worked by luck on some drivers).
struct HitPayload {
    vec3  hitPos;
    vec3  normal;
    vec3  albedo;
    float reflectivity;
    float matId;
    float t;
    float frontFace;
    float selected;
};

layout(location = 0) rayPayloadInEXT HitPayload payload;

void main() {
    // Negative t signals "no hit"; the ray-gen shader adds the sky colour.
    payload.t = -1.0;
}

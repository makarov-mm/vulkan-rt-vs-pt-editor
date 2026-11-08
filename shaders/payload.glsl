// Shared by every ray tracing stage: the hit payload and the material ids.
// Payloads on the same location are shared memory, so every stage must see
// the identical struct -- hence one definition included everywhere.
#ifndef PAYLOAD_GLSL
#define PAYLOAD_GLSL

// Material ids (Material enum in src/scene_object.h).
const int MAT_FLOOR    = 0;
const int MAT_SOLID    = 1;
const int MAT_GLASS    = 2;
const int MAT_EMISSIVE = 3;

struct HitPayload {
    vec3  hitPos;
    vec3  normal;        // faces against the incoming ray
    vec3  albedo;
    float reflectivity;
    float matId;         // MAT_*
    float t;             // < 0 means miss
    float frontFace;     // 1 = ray hit the outer surface, 0 = inside
    float selected;      // 1 = this object is selected in the editor
};

#endif

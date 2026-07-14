#version 460
#extension GL_EXT_ray_tracing : require

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
hitAttributeEXT vec2 attribs; // barycentric coordinates of the hit

// Prototype vertex: position + normal in LOCAL space (std430, vec4-padded).
struct Vertex {
    vec4 pos;      // xyz = position
    vec4 normal;   // xyz = normal
};

// Per-instance shading data, indexed by gl_InstanceCustomIndexEXT.
//   color  : rgb = albedo, w = selected flag
//   params : x = matId (0 floor, 1 solid, 2 glass, 3 emissive), y = reflectivity
//   mesh   : x = first index of this instance's mesh in the shared index buffer
struct ObjData {
    vec4  color;
    vec4  params;
    uvec4 mesh;
};

layout(std430, set = 0, binding = 3) readonly buffer Vertices { Vertex  v[]; } vertices;
layout(std430, set = 0, binding = 4) readonly buffer Indices  { uint    i[]; } indices;
layout(std430, set = 0, binding = 7) readonly buffer Objects  { ObjData o[]; } objects;

void main() {
    ObjData od = objects.o[gl_InstanceCustomIndexEXT];

    // gl_PrimitiveID is relative to this instance's BLAS, whose index slice
    // starts at od.mesh.x; the stored indices address the shared vertex buffer.
    uint fi = od.mesh.x + 3u * uint(gl_PrimitiveID);
    uint i0 = indices.i[fi + 0u];
    uint i1 = indices.i[fi + 1u];
    uint i2 = indices.i[fi + 2u];

    Vertex a = vertices.v[i0];
    Vertex b = vertices.v[i1];
    Vertex c = vertices.v[i2];

    const vec3 bary = vec3(1.0 - attribs.x - attribs.y, attribs.x, attribs.y);

    // Interpolate in object space, then take the normal to world space with the
    // instance transform (rotation + uniform scale, so no inverse-transpose needed).
    vec3 nObj = a.normal.xyz * bary.x + b.normal.xyz * bary.y + c.normal.xyz * bary.z;
    vec3 N = normalize(mat3(gl_ObjectToWorldEXT) * nObj);

    // Flip the normal so it always faces the incoming ray, and record whether
    // we hit the outer (front) surface — needed for glass refraction.
    bool front = dot(N, gl_WorldRayDirectionEXT) < 0.0;
    if (!front) N = -N;

    vec3 hitPos = gl_WorldRayOriginEXT + gl_WorldRayDirectionEXT * gl_HitTEXT;

    payload.hitPos       = hitPos;
    payload.normal       = N;
    payload.albedo       = od.color.rgb;
    payload.reflectivity = od.params.y;
    payload.matId        = od.params.x;
    payload.t            = gl_HitTEXT;
    payload.frontFace    = front ? 1.0 : 0.0;
    payload.selected     = od.color.w;
}

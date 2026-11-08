#version 460
#extension GL_EXT_ray_tracing : require
#extension GL_GOOGLE_include_directive : require

#include "payload.glsl"

layout(location = 0) rayPayloadInEXT HitPayload payload;
hitAttributeEXT vec2 attribs;   // barycentrics of the hit

// Prototype vertex: position + normal in LOCAL space (Vertex in src/gpu_types.h).
struct Vertex {
    vec4 pos;      // xyz = position
    vec4 normal;   // xyz = normal
};

// Per-instance shading data (GpuObj in src/gpu_types.h), indexed by gl_InstanceCustomIndexEXT.
//   color  : rgb = albedo, w = selected flag
//   params : x = material id (MAT_*), y = reflectivity
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
    vec3 n0 = vertices.v[indices.i[fi + 0u]].normal.xyz;
    vec3 n1 = vertices.v[indices.i[fi + 1u]].normal.xyz;
    vec3 n2 = vertices.v[indices.i[fi + 2u]].normal.xyz;

    // Interpolate in object space, then go to world space with the instance
    // transform (rotation + uniform scale: no inverse-transpose needed).
    vec3 bary = vec3(1.0 - attribs.x - attribs.y, attribs.x, attribs.y);
    vec3 N    = normalize(mat3(gl_ObjectToWorldEXT) * (n0 * bary.x + n1 * bary.y + n2 * bary.z));

    // Face the normal against the ray and record which side was hit (glass needs it).
    bool front = dot(N, gl_WorldRayDirectionEXT) < 0.0;

    payload.hitPos       = gl_WorldRayOriginEXT + gl_WorldRayDirectionEXT * gl_HitTEXT;
    payload.normal       = front ? N : -N;
    payload.albedo       = od.color.rgb;
    payload.reflectivity = od.params.y;
    payload.matId        = od.params.x;
    payload.t            = gl_HitTEXT;
    payload.frontFace    = front ? 1.0 : 0.0;
    payload.selected     = od.color.w;
}

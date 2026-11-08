// Everything the Whitted (raygen_rt) and path-traced (raygen_pt) ray-gen
// shaders share: bindings, camera, RNG, sampling helpers, the dispersive
// glass interaction and the per-pixel temporal accumulation. The two
// renderers differ only in how they shade diffuse / reflective surfaces.
#ifndef RAYGEN_COMMON_GLSL
#define RAYGEN_COMMON_GLSL

#include "payload.glsl"
#include "display.glsl"

layout(binding = 0, set = 0) uniform accelerationStructureEXT topLevelAS;
layout(binding = 1, set = 0, rgba8) uniform image2D outImage;
layout(binding = 2, set = 0) uniform FrameUniforms {   // FrameUniforms in src/gpu_types.h
    mat4  viewInverse;
    mat4  projInverse;
    vec4  light;        // xyz = position, w = radius (spherical area light)
    vec4  lightColor;   // rgb = tint, w = emitted radiance (HDR)
    uvec4 frame;        // x = accumulated frame index (0 on camera move / scene edit), y = free-running,
                        // z = samples per frame, w = max path length
    vec4  marquee;      // rubber-band rect in pixels (x0,y0,x1,y1); x1 < x0 disables
} ubo;
layout(binding = 5, set = 0, rgba32f) uniform image2D accumImage;   // rgb = running mean, a = sample count
layout(binding = 6, set = 0, rgba32f) uniform image2D histImage;    // xyz = previous primary hit, w = glass signature

layout(location = 0) rayPayloadEXT HitPayload payload;
layout(location = 1) rayPayloadEXT float      shadowed;

const float PI     = 3.14159265358979;
const float TWO_PI = 6.28318530717959;
const float EPS    = 0.001;

// Per-channel IOR for chromatic dispersion (exaggerated for visibility).
const float IOR_R = 1.50, IOR_G = 1.52, IOR_B = 1.54;
const float GLASS_R0 = ((1.0 - IOR_G) / (1.0 + IOR_G)) * ((1.0 - IOR_G) / (1.0 + IOR_G));

// ---------------------------------------------------------------------------
//  Random numbers: PCG
// ---------------------------------------------------------------------------
uint pcgHash(uint v) {
    uint state = v * 747796405u + 2891336453u;
    uint word  = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
    return (word >> 22u) ^ word;
}
uint pcg(inout uint state) {
    state = state * 747796405u + 2891336453u;
    uint word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
    return (word >> 22u) ^ word;
}
float rnd(inout uint state) { return float(pcg(state)) * (1.0 / 4294967296.0); }

// Decorrelated seed per pixel and global sample index. Chained hashing avoids
// the diagonal patterns a linear combination of the inputs can produce.
uint pixelSeed(uint sampleIndex) {
    return pcgHash(gl_LaunchIDEXT.x + pcgHash(gl_LaunchIDEXT.y + pcgHash(sampleIndex)));
}

// ---------------------------------------------------------------------------
//  Camera
// ---------------------------------------------------------------------------
vec3 cameraOrigin() { return (ubo.viewInverse * vec4(0, 0, 0, 1)).xyz; }

// Primary ray direction through `pixel` (in launch pixels, centre = +0.5).
vec3 cameraDir(vec2 pixel) {
    vec2 d      = pixel / vec2(gl_LaunchSizeEXT.xy) * 2.0 - 1.0;
    vec4 target = ubo.projInverse * vec4(d.x, d.y, 1, 1);
    return normalize((ubo.viewInverse * vec4(normalize(target.xyz), 0)).xyz);
}

// Angular size of one pixel (radians), for the checker filter footprint.
// abs() because the projection bakes in the Vulkan Y flip.
float pixelSpread() {
    return 2.0 * abs(ubo.projInverse[1][1]) / float(gl_LaunchSizeEXT.y);
}

// ---------------------------------------------------------------------------
//  Scene helpers
// ---------------------------------------------------------------------------
vec3 skyColor(vec3 d) {
    // Shared by both renderers so the halves differ algorithmically, not
    // tonally. For the path tracer it is a full environment light: kept dim
    // and saturated so the emissive sphere stays the key light.
    float h = 0.5 * (normalize(d).y + 1.0);
    return mix(vec3(0.42, 0.50, 0.62), vec3(0.10, 0.20, 0.45), h);
}

// Box-filtered checkerboard (iq). w = filter width in world units, i.e. how
// much of the plane one pixel covers. At w -> 0 it is the hard checker; at
// large w it converges to the mean grey, which removes moire on the floor
// and in its reflections.
vec3 checker(vec3 p, float w) {
    vec2 uv = p.xz;
    vec2 fw = vec2(max(w, 1e-4));
    vec2 i  = 2.0 * (abs(fract((uv - 0.5 * fw) * 0.5) - 0.5)
                   - abs(fract((uv + 0.5 * fw) * 0.5) - 0.5)) / fw;
    float f = 0.5 - 0.5 * i.x * i.y;   // 0 = light cell, 1 = dark
    return mix(vec3(0.85), vec3(0.10), f);
}

// Floor albedo at the current payload hit; the footprint grows with the path
// length and stretches at grazing angles (clamped so it cannot explode).
vec3 surfaceAlbedo(int mat, vec3 N, vec3 dir, float pathLen) {
    if (mat != MAT_FLOOR) return payload.albedo;
    float fw = pathLen * pixelSpread() / max(abs(dot(N, dir)), 0.25);
    return checker(payload.hitPos, fw);
}

void basis(vec3 n, out vec3 t, out vec3 b) {
    vec3 a = abs(n.x) > 0.9 ? vec3(0, 1, 0) : vec3(1, 0, 0);
    t = normalize(cross(n, a));
    b = cross(n, t);
}

// Uniform direction inside the cone of half-angle acos(cosThetaMax) around dir.
// pdf (solid angle) = 1 / (2*PI*(1 - cosThetaMax)).
vec3 sampleCone(vec3 dir, float cosThetaMax, float u1, float u2) {
    float cosT = mix(1.0, cosThetaMax, u1);
    float sinT = sqrt(max(0.0, 1.0 - cosT * cosT));
    float phi  = TWO_PI * u2;
    vec3 t, b; basis(dir, t, b);
    return normalize(t * (cos(phi) * sinT) + b * (sin(phi) * sinT) + dir * cosT);
}

// 1 if something blocks the segment, 0 if it reaches tmax.
float traceShadow(vec3 origin, vec3 L, float tmax) {
    shadowed = 1.0;
    traceRayEXT(topLevelAS,
        gl_RayFlagsTerminateOnFirstHitEXT | gl_RayFlagsOpaqueEXT | gl_RayFlagsSkipClosestHitShaderEXT,
        0xFF, 0, 0, 1, origin, EPS, L, tmax, 1);
    return shadowed;
}

// Closest hit into `payload`; payload.t < 0 on a miss.
void traceClosest(vec3 origin, vec3 dir) {
    payload.t = 0.0;
    traceRayEXT(topLevelAS, gl_RayFlagsOpaqueEXT, 0xFF, 0, 0, 0, origin, EPS, dir, 10000.0, 0);
}

// Warm rim highlight for selected objects (primary hits only).
vec3 selectionRim(vec3 dir, vec3 N) {
    float cosI = clamp(dot(-dir, N), 0.0, 1.0);
    float rim  = pow(1.0 - cosI, 3.0);
    return vec3(1.0, 0.62, 0.15) * (0.06 + 0.9 * rim) * 0.7;
}

// ---------------------------------------------------------------------------
//  Dispersive Fresnel glass (hero wavelength). Reflects or refracts the ray at
//  the current payload hit; `u` decides between the two. On the first
//  refraction the path collapses to its hero channel (x3 keeps the energy),
//  so over a frame each channel sees its own IOR -> dispersion, no channel noise.
// ---------------------------------------------------------------------------
void glassScatter(vec3 N, int hero, float u,
                  inout vec3 origin, inout vec3 dir, inout vec3 throughput, inout bool dispersed) {
    float ior  = (hero == 0) ? IOR_R : (hero == 1) ? IOR_G : IOR_B;
    float eta  = (payload.frontFace > 0.5) ? (1.0 / ior) : ior;
    float cosI = clamp(dot(-dir, N), 0.0, 1.0);
    float fres = GLASS_R0 + (1.0 - GLASS_R0) * pow(1.0 - cosI, 5.0);

    vec3 refr = refract(dir, N, eta);
    bool tir  = dot(refr, refr) < 1e-6;

    if (tir || u < fres) {
        dir    = reflect(dir, N);
        origin = payload.hitPos + N * EPS;          // reflection stays full-colour
    } else {
        dir    = normalize(refr);
        origin = payload.hitPos - N * EPS;
        if (!dispersed) {
            vec3 mask = (hero == 0) ? vec3(1, 0, 0) : (hero == 1) ? vec3(0, 1, 0) : vec3(0, 0, 1);
            throughput *= 3.0 * mask;
            dispersed = true;
        }
        throughput *= mix(vec3(1.0), payload.albedo, 0.65);   // coloured glass
    }
    throughput *= vec3(0.98);                                 // very light absorption
}

// ---------------------------------------------------------------------------
//  Per-pixel adaptive temporal accumulation
//
//  Static pixels accumulate forever (converge to a clean, sharp image).
//  Pixels whose primary hit moved -- or, for glass, whose refraction-chain
//  signature changed -- reset, so dragged objects stay sharp instead of
//  smearing. Returns the accumulated HDR colour.
// ---------------------------------------------------------------------------
vec3 accumulate(ivec2 pix, vec3 color, vec3 curPos, float curSig) {
    vec4  prevHist = imageLoad(histImage, pix);
    float camDist  = distance(curPos, cameraOrigin());
    float moved    = distance(curPos, prevHist.xyz);
    bool  reset    = (ubo.frame.x == 0u)
                  || (moved > 0.0008 * camDist + 0.0012)
                  || (abs(curSig - prevHist.w) > 0.02);

    vec4  prevAcc = imageLoad(accumImage, pix);
    float count   = reset ? 0.0 : prevAcc.a;
    vec3  accum   = (count < 0.5) ? color : mix(prevAcc.rgb, color, 1.0 / (count + 1.0));
    imageStore(accumImage, pix, vec4(accum, count + 1.0));
    imageStore(histImage,  pix, vec4(curPos, curSig));
    return accum;
}

// Non-jittered centre ray of this pixel -> world hit position (the sky gives
// a stable far point). Used by the motion test only, so no AA jitter here.
// Leaves the hit in `payload`.
vec3 primaryHitPos() {
    vec3 origin = cameraOrigin();
    vec3 dir    = cameraDir(vec2(gl_LaunchIDEXT.xy) + 0.5);
    traceClosest(origin, dir);
    return (payload.t < 0.0) ? origin + dir * 1000.0 : payload.hitPos;
}

// One full (jittered) sample of the pixel; defined by each ray-gen shader.
vec3 renderSample(inout uint seed, int sampleIdx, int spp);

// Mean of this frame's samples. The seed follows the free-running frame
// counter (frame.y), not the accumulation counter, so the noise keeps
// changing while the camera moves.
vec3 renderPixel() {
    int  spp   = max(1, int(ubo.frame.z));
    vec3 color = vec3(0.0);
    for (int s = 0; s < spp; ++s) {
        uint seed = pixelSeed(ubo.frame.y * uint(spp) + uint(s));
        color += renderSample(seed, s, spp);
    }
    return color / float(spp);
}

#endif

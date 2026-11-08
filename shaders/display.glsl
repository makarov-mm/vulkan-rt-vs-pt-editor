// Display transform and editor overlay, shared by both ray-gen shaders and the
// denoiser so the two halves (and denoised vs raw) stay tonally identical.
#ifndef DISPLAY_GLSL
#define DISPLAY_GLSL

// ACES filmic tone map (Narkowicz fit).
vec3 acesToneMap(vec3 x) {
    const float a = 2.51, b = 0.03, c = 2.43, d = 0.59, e = 0.14;
    return clamp((x * (a * x + b)) / (x * (c * x + d) + e), 0.0, 1.0);
}

// HDR radiance -> display value: ACES, then gamma 2.2.
vec3 toDisplay(vec3 hdr) {
    return pow(acesToneMap(hdr), vec3(1.0 / 2.2));
}

// Editor marquee (rubber band) composited over the final image, so it never
// touches the accumulation buffers. rect = (x0, y0, x1, y1); x1 < x0 disables.
vec3 applyMarquee(vec3 c, vec2 p, vec4 rect) {
    if (rect.z < rect.x) return c;
    const vec3 tint = vec3(1.0, 0.75, 0.2);
    bool inX = p.x >= rect.x && p.x <= rect.z;
    bool inY = p.y >= rect.y && p.y <= rect.w;
    bool border =
        (inY && (abs(p.x - rect.x) < 1.5 || abs(p.x - rect.z) < 1.5)) ||
        (inX && (abs(p.y - rect.y) < 1.5 || abs(p.y - rect.w) < 1.5));
    if (border)          return mix(c, tint, 0.9);
    if (inX && inY)      return mix(c, tint, 0.08);
    return c;
}

#endif

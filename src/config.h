#pragma once
#include <cstdint>

#include "vec3.h"

// ---- Window / views ---------------------------------------------------------
inline constexpr uint32_t VIEWS     = 2;     // side-by-side renderers (RT | PT)
inline constexpr uint32_t WIDTH     = 800;   // one renderer's viewport; the window is WIDTH * VIEWS wide
inline constexpr uint32_t HEIGHT    = 800;
inline constexpr uint32_t TOOLBAR_H = 46;    // toolbar strip above the viewport

// ---- Scene limits -----------------------------------------------------------
inline constexpr uint32_t MAX_INSTANCES = 64;   // floor + light + editable objects
inline constexpr uint32_t FIXED_SLOTS   = 2;    // slot 0 = floor, slot 1 = light

// ---- Camera -----------------------------------------------------------------
inline constexpr Vec3  ORBIT_CENTER{ 0.0f, 1.0f, 0.0f };
inline constexpr float FOV_Y_DEG = 60.0f;
inline constexpr float Z_NEAR    = 0.1f;
inline constexpr float Z_FAR     = 100.0f;

// ---- Light: a visible emissive sphere, the key light of both renderers ------
// Position, size, tint and emission reach the shaders through the frame UBO.
inline constexpr Vec3  LIGHT_POS{ 4.0f, 7.0f, -2.0f };
inline constexpr float LIGHT_RADIUS   = 1.2f;                  // bigger = softer shadows
inline constexpr Vec3  LIGHT_COLOR{ 1.0f, 0.95f, 0.85f };      // warm tint
inline constexpr float LIGHT_EMISSION = 45.0f;                 // HDR radiance

// ---- Prototype meshes -------------------------------------------------------
// The light is the sphere prototype scaled by LIGHT_RADIUS / SPHERE_RADIUS.
inline constexpr float SPHERE_RADIUS = 1.3f;

// ---- Renderer quality -------------------------------------------------------
inline constexpr uint32_t SAMPLES_PER_FRAME = 24;   // lower on weaker GPUs
inline constexpr uint32_t MAX_PATH_LENGTH   = 8;    // glass needs a few bounces
static_assert(SAMPLES_PER_FRAME % 3 == 0, "dispersion cycles the hero wavelength over R, G, B per sample");

#ifdef NDEBUG
inline constexpr bool ENABLE_VALIDATION = false;
#else
inline constexpr bool ENABLE_VALIDATION = true;
#endif

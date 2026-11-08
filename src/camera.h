#pragma once
#include <optional>

#include "mat4.h"
#include "vec3.h"

struct Ray {
    Vec3 origin;
    Vec3 dir;   // unit length
};

struct ScreenPoint {
    float x = 0.0f, y = 0.0f;
};

// -----------------------------------------------------------------------------
//  Orbit camera around ORBIT_CENTER (RMB drag + wheel). One camera drives both
//  views, and every CPU-side ray / projection here uses exactly the same math
//  as the ray-gen shaders, so picking matches what is on screen.
// -----------------------------------------------------------------------------
class OrbitCamera {
public:
    void orbit(float dYaw, float dPitch) noexcept;
    void zoom(float wheelSteps) noexcept;

    [[nodiscard]] Vec3 eye()     const noexcept;
    [[nodiscard]] Vec3 forward() const noexcept;   // unit, eye -> orbit centre
    [[nodiscard]] Vec3 right()   const noexcept;   // unit, screen +x
    [[nodiscard]] float distance() const noexcept { return dist_; }

    // Matrices uploaded to the shaders (camera-to-world and clip-to-camera).
    [[nodiscard]] Mat4 viewInverse() const noexcept;
    [[nodiscard]] static Mat4 projInverse() noexcept;

    // Primary ray through the centre of pixel (px, py) of one view.
    [[nodiscard]] Ray rayThroughPixel(float px, float py) const noexcept;

    // World point -> view pixel; nullopt if it is behind the camera.
    [[nodiscard]] std::optional<ScreenPoint> project(const Vec3& world) const noexcept;

private:
    float yaw_   = 2.2f;
    float pitch_ = 0.42f;
    float dist_  = 15.0f;
};

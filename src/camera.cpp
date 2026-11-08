#include "camera.h"

#include <algorithm>
#include <numbers>

#include "config.h"

namespace {
constexpr Vec3  WORLD_UP{ 0.0f, 1.0f, 0.0f };
constexpr float ASPECT    = static_cast<float>(WIDTH) / static_cast<float>(HEIGHT);
constexpr float MAX_PITCH = 1.5f;   // ~86 degrees, keeps lookAt away from the pole
constexpr float MIN_DIST  = 3.0f;
constexpr float MAX_DIST  = 30.0f;

// tan(fovY / 2), the half-height of the image plane at unit distance.
float tanHalfFov() noexcept {
    static const float t = std::tan(FOV_Y_DEG * std::numbers::pi_v<float> / 360.0f);
    return t;
}
} // namespace

void OrbitCamera::orbit(float dYaw, float dPitch) noexcept {
    yaw_  += dYaw;
    pitch_ = std::clamp(pitch_ + dPitch, -MAX_PITCH, MAX_PITCH);
}

void OrbitCamera::zoom(float wheelSteps) noexcept {
    dist_ = std::clamp(dist_ * std::pow(0.9f, wheelSteps), MIN_DIST, MAX_DIST);
}

Vec3 OrbitCamera::eye() const noexcept {
    const float cp = std::cos(pitch_);
    return ORBIT_CENTER + Vec3{ cp * std::cos(yaw_), std::sin(pitch_), cp * std::sin(yaw_) } * dist_;
}

Vec3 OrbitCamera::forward() const noexcept { return (ORBIT_CENTER - eye()).normalize(); }
Vec3 OrbitCamera::right()   const noexcept { return forward().cross(WORLD_UP).normalize(); }

// Inverse of a right-handed lookAt: the columns are the camera basis and eye.
Mat4 OrbitCamera::viewInverse() const noexcept {
    const Vec3 f = forward(), s = right(), u = s.cross(f), e = eye();
    Mat4 r;
    r.m = { s.x,  s.y,  s.z,  0.0f,
            u.x,  u.y,  u.z,  0.0f,
           -f.x, -f.y, -f.z,  0.0f,
            e.x,  e.y,  e.z,  1.0f };
    return r;
}

// Analytic inverse of the Vulkan perspective matrix (y flipped, depth 0..1):
//   P = [ 1/(a*t) 0 0 0 | 0 -1/t 0 0 | 0 0 c -1 | 0 0 d 0 ]  (columns)
// with c = far / (near - far), d = -far * near / (far - near).
Mat4 OrbitCamera::projInverse() noexcept {
    const float t = tanHalfFov();
    const float c = Z_FAR / (Z_NEAR - Z_FAR);
    const float d = -(Z_FAR * Z_NEAR) / (Z_FAR - Z_NEAR);
    Mat4 r;
    r.m = { ASPECT * t, 0.0f, 0.0f,     0.0f,
            0.0f,       -t,   0.0f,     0.0f,
            0.0f,       0.0f, 0.0f,     1.0f / d,
            0.0f,       0.0f, -1.0f,    c / d };
    return r;
}

// Same construction as the ray-gen shaders: NDC -> camera-space direction
// (x * aspect * t, -y * t, -1) -> world space.
Ray OrbitCamera::rayThroughPixel(float px, float py) const noexcept {
    const float ndcX = 2.0f * (px + 0.5f) / static_cast<float>(WIDTH)  - 1.0f;
    const float ndcY = 2.0f * (py + 0.5f) / static_cast<float>(HEIGHT) - 1.0f;
    const float t    = tanHalfFov();
    const Vec3  f = forward(), s = right(), u = s.cross(f);
    return { eye(), (f + s * (ndcX * ASPECT * t) - u * (ndcY * t)).normalize() };
}

std::optional<ScreenPoint> OrbitCamera::project(const Vec3& world) const noexcept {
    const Vec3  f = forward(), s = right(), u = s.cross(f);
    const Vec3  v = world - eye();
    const float depth = v.dot(f);
    if (depth <= 1e-5f) return std::nullopt;

    const float t    = tanHalfFov();
    const float ndcX =  v.dot(s) / (depth * ASPECT * t);
    const float ndcY = -v.dot(u) / (depth * t);   // Vulkan NDC y points down, like window pixels
    return ScreenPoint{ (ndcX * 0.5f + 0.5f) * static_cast<float>(WIDTH),
                        (ndcY * 0.5f + 0.5f) * static_cast<float>(HEIGHT) };
}

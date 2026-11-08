#include "mat3.h"

#include <cmath>

Mat3 Mat3::rotation(const Vec3& axis, float angle) noexcept {
    const Vec3  a = axis.normalize();
    const float c = std::cos(angle);
    const float s = std::sin(angle);
    const float t = 1.0f - c;

    return {
        { c + a.x * a.x * t,       a.x * a.y * t + a.z * s, a.x * a.z * t - a.y * s },
        { a.x * a.y * t - a.z * s, c + a.y * a.y * t,       a.y * a.z * t + a.x * s },
        { a.x * a.z * t + a.y * s, a.y * a.z * t - a.x * s, c + a.z * a.z * t       },
    };
}

Mat3 Mat3::orthonormalized() const noexcept {
    const Vec3 x = c0.normalize();
    const Vec3 y = (c1 - x * x.dot(c1)).normalize();
    return { x, y, x.cross(y) };
}

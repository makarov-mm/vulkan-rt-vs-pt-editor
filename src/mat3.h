#pragma once
#include "vec3.h"

// Column-major 3x3 matrix (used for object rotations).
struct Mat3 {
    Vec3 c0{ 1, 0, 0 }, c1{ 0, 1, 0 }, c2{ 0, 0, 1 };

    // Rotation by `angle` radians about `axis` (Rodrigues).
    [[nodiscard]] static Mat3 rotation(const Vec3& axis, float angle) noexcept;

    // Gram-Schmidt re-orthonormalization. Rotations composed over thousands of
    // drag steps drift away from orthonormal (the object slowly shears and
    // scales); calling this after each composition keeps them rigid.
    [[nodiscard]] Mat3 orthonormalized() const noexcept;
};

[[nodiscard]] constexpr Vec3 operator*(const Mat3& m, const Vec3& v) noexcept {
    return m.c0 * v.x + m.c1 * v.y + m.c2 * v.z;
}

[[nodiscard]] constexpr Mat3 operator*(const Mat3& a, const Mat3& b) noexcept {
    return { a * b.c0, a * b.c1, a * b.c2 };
}

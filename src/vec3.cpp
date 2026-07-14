#include "vec3.h"

Vec3 Vec3::normalize() const noexcept
{
    float l = std::sqrt(this->dot(*this));
    return l > 0 ? Vec3{ x / l, y / l, z / l } : *this;
}

#include "mat3.h"
#include "vec3.h"

Mat3 rot_axis(const Vec3& a, float ang) noexcept
{
    Vec3 axis = a.normalize();

    float c = std::cos(ang);
    float s = std::sin(ang);
    float t = 1 - c;

    float x = axis.x;
    float y = axis.y;
	float z = axis.z;

    Mat3 m;
    m.c0 = {
    	c + x * x * t,
    	x * y * t + z * s,
    	x * z * t - y * s
    };

    m.c1 = { 
    	x * y * t - z * s,
    	c + y * y * t,
    	y * z * t + x * s
    };

    m.c2 = { 
    	x * z * t + y * s, 
    	y * z * t - x * s, 
    	c + z * z * t
    };

    return m;
}

#pragma once
#include "vec3.h"
#include "mat3.h"

// -----------------------------------------------------------------------------
//  Shape types (one BLAS each) and scene objects (one TLAS instance each)
// -----------------------------------------------------------------------------
enum MeshType
{
    M_SPHERE = 0, M_DIAMOND, M_CUBE, M_PYRAMID, M_CYLINDER,
    M_DODECAHEDRON, M_SUPERTOROID, M_SUPERSHAPE,
    M_FLOOR,                       // not user-addable
    MESH_COUNT
};
static constexpr int ADDABLE_MESHES = 8;

struct SceneObject 
{
    int   mesh = M_SPHERE;
    Vec3  pos;
    Mat3  rot;
    float scale = 1.0f;
    Vec3  color{ 1, 1, 1 };
    float matId = 2.0f;         // 1 = solid, 2 = glass, 3 = emissive
    float reflectivity = 0.0f;
    bool  sel = false;
};

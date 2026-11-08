#pragma once
#include <cstdint>
#include <vector>

#include "gpu_types.h"
#include "vec3.h"

// One prototype mesh: its slice of the shared vertex/index buffers plus local
// bounds for picking and on-floor placement.
struct MeshRange {
    uint32_t firstIndex = 0, indexCount = 0;
    uint32_t firstVert  = 0, vertCount  = 0;
    Vec3     bcenter;            // local bounding-sphere centre
    float    bradius = 1.0f;     // local bounding-sphere radius
    float    minY    = 0.0f;     // lowest local point
};

// -----------------------------------------------------------------------------
//  Generates every prototype shape once, in LOCAL space, into one shared
//  vertex/index pool. Colour and material are per instance, so vertices
//  carry only position + normal.
//
//  Usage: begin(); <shape>(...); MeshRange r = end();
// -----------------------------------------------------------------------------
class MeshBuilder {
public:
    void      begin() noexcept;
    MeshRange end() const noexcept;

    void sphere(float radius, int stacks = 24, int slices = 48);
    void diamond(float size, int facets = 16);          // brilliant-cut gem, flat facets
    void cube(float size);                              // extents +-size/2
    void pyramid(float base, float height);             // base at y = 0
    void cylinder(float radius, float height, int segments = 48);   // smooth sides, base at y = 0
    void dodecahedron(float circumradius);
    void supertoroid(int segU = 120, int segV = 60);    // superellipse tube, integer twist
    void supershape(int segU = 120, int segV = 60);     // soft 6-lobe superformula preset
    void floor(float halfSize);

    [[nodiscard]] const std::vector<Vertex>&   vertices() const noexcept { return verts_; }
    [[nodiscard]] const std::vector<uint32_t>& indices()  const noexcept { return indices_; }

private:
    uint32_t vertex(const Vec3& p, const Vec3& n);
    void     triangle(uint32_t a, uint32_t b, uint32_t c);
    void     quad(uint32_t a, uint32_t b, uint32_t c, uint32_t d);   // a-b-c, a-c-d

    // Flat-shaded triangle whose normal points away from `interior`.
    void flatTriangle(const Vec3& a, const Vec3& b, const Vec3& c, const Vec3& interior);

    // (segU+1) x (segV+1) vertex grid for parametric surfaces. `pos(i, j)` must
    // accept indices one past either end (central-difference normals).
    template <class PosFn>
    void grid(int segU, int segV, bool outward, PosFn pos);

    std::vector<Vertex>   verts_;
    std::vector<uint32_t> indices_;
    uint32_t startVert_ = 0, startIndex_ = 0;
};

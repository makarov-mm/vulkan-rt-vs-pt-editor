// Mouse / keyboard handling, picking and marquee selection.
#include "scene_editor.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>

namespace {
constexpr float ORBIT_SPEED    = 0.005f;   // radians per pixel
constexpr float ROTATE_SPEED   = 0.01f;    // radians per pixel
constexpr float LIFT_SPEED     = 0.0015f;  // vertical move per pixel, times the camera distance
constexpr int   DRAG_THRESHOLD = 3;        // pixels (Manhattan) before a click becomes a drag

constexpr Vec3 WORLD_UP{ 0.0f, 1.0f, 0.0f };
} // namespace

// =============================================================================
//  Keyboard
// =============================================================================
void SceneEditor::onKeyDown(WPARAM key, LPARAM flags) {
    const bool repeat = (flags & (1 << 30)) != 0;   // auto-repeat while the key is held
    switch (key) {
    case VK_ESCAPE: quit_ = true;      break;
    case VK_DELETE: deleteSelected();  break;
    case 'D':       if (!repeat) denoiseEnabled_ = !denoiseEnabled_; break;
    default:        break;
    }
}

// =============================================================================
//  Viewport mouse: LMB = object operations (per tool), RMB = orbit camera
// =============================================================================
int SceneEditor::localX(int x) const noexcept {
    return std::clamp(x - viewOffsetX_, 0, static_cast<int>(WIDTH) - 1);
}

void SceneEditor::onWheel(float steps) {
    camera_.zoom(steps);
    resetAccum_ = true;
}

void SceneEditor::onRButtonDown(int x, int y) {
    orbiting_  = true;
    lastMouse_ = { x, y };
    SetCapture(viewHwnd_);
    SetFocus(viewHwnd_);
}

void SceneEditor::onRButtonUp() {
    orbiting_ = false;
    if (!lmbDown_) ReleaseCapture();
}

void SceneEditor::onLButtonDown(int x, int y) {
    lmbDown_   = true;
    dragMoved_ = false;
    downMouse_ = lastMouse_ = { x, y };
    SetCapture(viewHwnd_);
    SetFocus(viewHwnd_);

    // Anchor this interaction to the half it started in; positions are then
    // handled in that view's local coordinates (deltas stay raw).
    viewOffsetX_ = (x >= static_cast<int>(WIDTH)) ? static_cast<int>(WIDTH) : 0;
    const int lx = localX(x);

    if (mode_ == EditMode::Select) {
        mq0_ = mq1_ = { lx, y };
        marqueeOn_ = false;   // becomes true once the drag exceeds the threshold
        return;
    }

    // Move / rotate: grab the object under the cursor. Clicking an unselected
    // object makes it the (sole, unless Shift) selection first.
    const auto hit = pickObject(lx, y);
    if (!hit) return;
    if (!objects_[*hit].selected) {
        if (!shiftHeld()) clearSelection();
        objects_[*hit].selected = true;
        markSceneDirty();
    }

    if (mode_ == EditMode::Move) {
        grabPlaneY_ = objects_[*hit].pos.y;
        const Ray r = camera_.rayThroughPixel(static_cast<float>(lx), static_cast<float>(y));
        float t = (std::fabs(r.dir.y) > 1e-4f) ? (grabPlaneY_ - r.origin.y) / r.dir.y : camera_.distance();
        if (t < 0.1f) t = camera_.distance();
        grabPoint_ = r.origin + r.dir * t;
        dragHoriz_ = {};
        dragVert_  = 0.0f;
        dragOrig_.clear();
        for (std::size_t i = 0; i < objects_.size(); ++i)
            if (objects_[i].selected) dragOrig_.emplace_back(i, objects_[i].pos);
        movingObjs_ = true;
    } else {
        rotatingObjs_ = true;
    }
}

void SceneEditor::onMouseMove(int x, int y) {
    const int dx = x - lastMouse_.x;
    const int dy = y - lastMouse_.y;
    lastMouse_ = { x, y };

    if (orbiting_) {
        camera_.orbit(static_cast<float>(dx) * ORBIT_SPEED, static_cast<float>(dy) * ORBIT_SPEED);
        resetAccum_ = true;
        return;
    }
    if (!lmbDown_) return;

    if (std::abs(x - downMouse_.x) + std::abs(y - downMouse_.y) > DRAG_THRESHOLD)
        dragMoved_ = true;

    switch (mode_) {
    case EditMode::Select:
        if (dragMoved_) {
            marqueeOn_ = true;
            mq1_ = { localX(x), y };   // drawn by the shaders via the UBO / push constants
        }
        break;

    case EditMode::Move:
        if (!movingObjs_) break;
        if (shiftHeld()) {
            dragVert_ -= static_cast<float>(dy) * LIFT_SPEED * camera_.distance();   // vertical while Shift is held
        } else {
            const Ray r = camera_.rayThroughPixel(static_cast<float>(localX(x)), static_cast<float>(y));
            if (std::fabs(r.dir.y) > 1e-4f) {
                const float t = (grabPlaneY_ - r.origin.y) / r.dir.y;
                if (t > 0.1f) {
                    const Vec3 p = r.origin + r.dir * t;
                    dragHoriz_ = { p.x - grabPoint_.x, 0.0f, p.z - grabPoint_.z };
                }
            }
        }
        for (const auto& [i, orig] : dragOrig_)
            objects_[i].pos = orig + Vec3{ dragHoriz_.x, dragVert_, dragHoriz_.z };
        markSceneDirty();
        break;

    case EditMode::Rotate: {
        if (!rotatingObjs_ || (dx == 0 && dy == 0)) break;
        // dx spins about the world Y axis, dy about the camera's right axis.
        Mat3 dR = Mat3::rotation(WORLD_UP, static_cast<float>(dx) * ROTATE_SPEED);
        if (dy) dR = Mat3::rotation(camera_.right(), static_cast<float>(dy) * ROTATE_SPEED) * dR;
        for (SceneObject& o : objects_)
            if (o.selected) o.rot = (dR * o.rot).orthonormalized();
        markSceneDirty();
        break;
    }
    }
}

void SceneEditor::onLButtonUp(int x, int y) {
    if (!lmbDown_) return;
    lmbDown_ = false;

    if (mode_ == EditMode::Select) {
        if (marqueeOn_) {
            marqueeSelect(shiftHeld());
            marqueeOn_ = false;
        } else if (const auto hit = pickObject(localX(x), y); shiftHeld()) {
            if (hit) { objects_[*hit].selected = !objects_[*hit].selected; markSceneDirty(); }
        } else {
            const bool hadSelection = std::ranges::any_of(objects_, &SceneObject::selected);
            clearSelection();
            if (hit) objects_[*hit].selected = true;
            if (hadSelection || hit) markSceneDirty();
        }
    }
    endObjectDrag();

    // Last: releasing capture sends WM_CAPTURECHANGED, which resets the drag state.
    if (!orbiting_) ReleaseCapture();
}

// Capture taken away (Alt+Tab, a modal box, ...): end every interaction so no
// button stays logically "held" without its matching button-up message.
void SceneEditor::onCaptureLost() noexcept {
    orbiting_  = false;
    lmbDown_   = false;
    marqueeOn_ = false;
    endObjectDrag();
}

// =============================================================================
//  Picking and marquee selection
// =============================================================================
Vec3 SceneEditor::worldCenter(const SceneObject& o) const noexcept {
    return o.pos + o.rot * (meshes_[o.mesh].bcenter * o.scale);
}

// CPU ray against each object's bounding sphere; nearest hit wins.
std::optional<std::size_t> SceneEditor::pickObject(int px, int py) const noexcept {
    const Ray r = camera_.rayThroughPixel(static_cast<float>(px), static_cast<float>(py));

    std::optional<std::size_t> best;
    float bestT = 1e30f;
    for (std::size_t i = 0; i < objects_.size(); ++i) {
        const SceneObject& o = objects_[i];
        const float radius = meshes_[o.mesh].bradius * o.scale;
        const Vec3  oc = r.origin - worldCenter(o);
        const float b  = oc.dot(r.dir);
        const float h2 = b * b - (oc.dot(oc) - radius * radius);
        if (h2 < 0.0f) continue;

        const float h = std::sqrt(h2);
        float t = -b - h;
        if (t < 0.001f) t = -b + h;   // camera inside the sphere
        if (t > 0.001f && t < bestT) { bestT = t; best = i; }
    }
    return best;
}

// Selects every object whose projected bounding circle touches the marquee.
void SceneEditor::marqueeSelect(bool additive) {
    const auto [x0, y0, x1, y1] = marqueeRect();
    const Vec3 right = camera_.right();

    if (!additive) clearSelection();
    for (SceneObject& o : objects_) {
        const Vec3 c  = worldCenter(o);
        const auto sc = camera_.project(c);
        if (!sc) continue;

        // Projected radius from a second point offset along the camera's right
        // axis, which stays perpendicular to the view at any pitch.
        float r = 4.0f;   // minimum radius in pixels
        if (const auto se = camera_.project(c + right * (meshes_[o.mesh].bradius * o.scale)))
            r = std::max(r, std::hypot(se->x - sc->x, se->y - sc->y));

        const float nx = std::clamp(sc->x, x0, x1);   // closest point of the rectangle
        const float ny = std::clamp(sc->y, y0, y1);
        if (std::hypot(sc->x - nx, sc->y - ny) <= r) o.selected = true;
    }
    markSceneDirty();
}

Float4 SceneEditor::marqueeRect() const noexcept {
    if (!marqueeOn_) return MARQUEE_OFF;
    return { static_cast<float>(std::min(mq0_.x, mq1_.x)), static_cast<float>(std::min(mq0_.y, mq1_.y)),
             static_cast<float>(std::max(mq0_.x, mq1_.x)), static_cast<float>(std::max(mq0_.y, mq1_.y)) };
}

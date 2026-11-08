// Win32 window, toolbar, mouse/keyboard handling, editor actions and picking.
#include "scene_editor.h"

#include <algorithm>
#include <cmath>
#include <format>
#include <numbers>
#include <string>

namespace {
constexpr float ORBIT_SPEED  = 0.005f;   // radians per pixel
constexpr float ROTATE_SPEED = 0.01f;    // radians per pixel
constexpr int   DRAG_THRESHOLD = 3;      // pixels (Manhattan) before a click becomes a drag

constexpr Vec3 WORLD_UP{ 0.0f, 1.0f, 0.0f };

constexpr std::array<Vec3, 8> SPAWN_PALETTE{ {
    { 0.55f, 0.72f, 1.00f }, { 1.00f, 0.65f, 0.72f }, { 0.55f, 0.95f, 0.65f },
    { 1.00f, 0.85f, 0.45f }, { 0.45f, 0.92f, 0.92f }, { 0.78f, 0.60f, 1.00f },
    { 1.00f, 0.55f, 0.35f }, { 0.90f, 0.90f, 0.98f },
} };

int xParam(LPARAM lp) noexcept { return static_cast<short>(LOWORD(lp)); }
int yParam(LPARAM lp) noexcept { return static_cast<short>(HIWORD(lp)); }

// Stores the SceneEditor passed to CreateWindowEx and returns it on later messages.
template <class T>
T* windowSelf(HWND h, UINT msg, LPARAM lp) noexcept {
    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<const CREATESTRUCTW*>(lp);
        SetWindowLongPtrW(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
    }
    return reinterpret_cast<T*>(GetWindowLongPtrW(h, GWLP_USERDATA));
}
} // namespace

// =============================================================================
//  Window procedures
// =============================================================================
LRESULT CALLBACK SceneEditor::mainProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    SceneEditor* self = windowSelf<SceneEditor>(h, msg, lp);
    if (self) {
        switch (msg) {
        case WM_COMMAND:    self->onCommand(LOWORD(wp)); return 0;
        case WM_DRAWITEM:   return self->onDrawItem(*reinterpret_cast<const DRAWITEMSTRUCT*>(lp));
        case WM_MOUSEWHEEL: self->onWheel(GET_WHEEL_DELTA_WPARAM(wp) / static_cast<float>(WHEEL_DELTA)); return 0;
        case WM_KEYDOWN:    self->onKeyDown(wp, lp); return 0;
        case WM_CLOSE:      self->quit = true; return 0;
        }
    }
    if (msg == WM_DESTROY) { PostQuitMessage(0); return 0; }
    return DefWindowProcW(h, msg, wp, lp);
}

LRESULT CALLBACK SceneEditor::viewProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    SceneEditor* self = windowSelf<SceneEditor>(h, msg, lp);
    if (self) {
        switch (msg) {
        case WM_RBUTTONDOWN:    self->onRButtonDown(xParam(lp), yParam(lp)); return 0;
        case WM_RBUTTONUP:
            self->orbiting = false;
            if (!self->lmbDown) ReleaseCapture();
            return 0;
        case WM_LBUTTONDOWN:    self->onLButtonDown(xParam(lp), yParam(lp)); return 0;
        case WM_LBUTTONUP:      self->onLButtonUp(xParam(lp), yParam(lp)); return 0;
        case WM_MOUSEMOVE:      self->onMouseMove(xParam(lp), yParam(lp)); return 0;
        case WM_MOUSEWHEEL:     self->onWheel(GET_WHEEL_DELTA_WPARAM(wp) / static_cast<float>(WHEEL_DELTA)); return 0;
        case WM_KEYDOWN:        self->onKeyDown(wp, lp); return 0;
        case WM_CAPTURECHANGED: if (reinterpret_cast<HWND>(lp) != h) self->onCaptureLost(); return 0;
        }
    }
    return DefWindowProcW(h, msg, wp, lp);
}

// =============================================================================
//  Window + toolbar
// =============================================================================
void SceneEditor::createWindow(HINSTANCE hInst) {
    WNDCLASSEXW wc{ .cbSize = sizeof(WNDCLASSEXW) };
    wc.style         = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc   = mainProc;
    wc.hInstance     = hInst;
    wc.hCursor       = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
    wc.lpszClassName = L"VkRtEditorMain";
    RegisterClassExW(&wc);

    WNDCLASSEXW vc{ .cbSize = sizeof(WNDCLASSEXW) };
    vc.style         = CS_HREDRAW | CS_VREDRAW;
    vc.lpfnWndProc   = viewProc;
    vc.hInstance     = hInst;
    vc.hCursor       = LoadCursorW(nullptr, IDC_ARROW);
    vc.lpszClassName = L"VkRtEditorView";
    RegisterClassExW(&vc);

    // Fixed-size window (no resize) keeps the swapchain logic simple.
    constexpr DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    RECT rect{ 0, 0, static_cast<LONG>(WIDTH * VIEWS), static_cast<LONG>(TOOLBAR_H + HEIGHT) };
    AdjustWindowRect(&rect, style, FALSE);

    hwnd = CreateWindowExW(0, wc.lpszClassName,
        L"Vulkan Ray Tracing vs Path Tracing - Scene Editor", style,
        CW_USEDEFAULT, CW_USEDEFAULT, rect.right - rect.left, rect.bottom - rect.top,
        nullptr, nullptr, hInst, this);
    if (!hwnd) throw std::runtime_error("CreateWindowEx failed (main window)");

    viewHwnd = CreateWindowExW(0, vc.lpszClassName, L"", WS_CHILD | WS_VISIBLE,
        0, static_cast<int>(TOOLBAR_H), static_cast<int>(WIDTH * VIEWS), static_cast<int>(HEIGHT),
        hwnd, nullptr, hInst, this);
    if (!viewHwnd) throw std::runtime_error("CreateWindowEx failed (viewport)");

    createToolbar(hInst);

    ShowWindow(hwnd, SW_SHOW);
    UpdateWindow(hwnd);
}

void SceneEditor::loadIcons() {
    // Same relative-path convention as the shaders. A missing or unreadable
    // PNG leaves nullptr, and the button falls back to a GDI-drawn icon.
    static constexpr std::array<const wchar_t*, ICON_COUNT> names{
        L"sphere", L"diamond", L"cube", L"pyramid", L"cylinder",
        L"dodecahedron", L"supertoroid", L"supershape",
        L"select", L"move", L"rotate", L"delete", L"delete_all",
    };
    for (std::size_t i = 0; i < names.size(); ++i) {
        const std::wstring path = std::wstring(L"icons/") + names[i] + L".png";
        auto bmp = std::make_unique<Gdiplus::Bitmap>(path.c_str());
        if (bmp->GetLastStatus() == Gdiplus::Ok) icons[i] = std::move(bmp);
    }
}

void SceneEditor::createToolbar(HINSTANCE hInst) {
    loadIcons();

    constexpr int size = 36, gap = 4, y = (static_cast<int>(TOOLBAR_H) - size) / 2;
    int x = 6;

    auto button = [&](int id) {
        HWND b = CreateWindowExW(0, L"BUTTON", L"", WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
            x, y, size, size, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), hInst, nullptr);
        x += size + gap;
        return b;
    };
    auto separator = [&] {
        CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE | SS_ETCHEDVERT,
            x + 2, y + 4, 2, size - 8, hwnd, nullptr, hInst, nullptr);
        x += 12;
    };

    for (int i = 0; i < ADDABLE_MESHES; ++i) button(ID_SHAPE_BASE + i);
    separator();
    toolButtons = { button(ID_TOOL_SELECT), button(ID_TOOL_MOVE), button(ID_TOOL_ROTATE) };
    separator();
    button(ID_DELETE);
    button(ID_DELETE_ALL);
}

void SceneEditor::onCommand(int id) {
    if (auto shape = addableMesh(id - ID_SHAPE_BASE)) {
        addObject(*shape);
    } else {
        switch (id) {
        case ID_TOOL_SELECT: mode = EditMode::Select; break;
        case ID_TOOL_MOVE:   mode = EditMode::Move;   break;
        case ID_TOOL_ROTATE: mode = EditMode::Rotate; break;
        case ID_DELETE:      deleteSelected();        break;
        case ID_DELETE_ALL:  deleteAll();             break;
        }
        for (HWND b : toolButtons) InvalidateRect(b, nullptr, TRUE);
    }
    SetFocus(viewHwnd);   // keep Del / Esc / D working after a button click
}

void SceneEditor::onKeyDown(WPARAM key, LPARAM flags) {
    const bool repeat = (flags & (1 << 30)) != 0;   // auto-repeat while the key is held
    switch (key) {
    case VK_ESCAPE: quit = true;      break;
    case VK_DELETE: deleteSelected(); break;
    case 'D':       if (!repeat) denoiseEnabled = !denoiseEnabled; break;
    }
}

// =============================================================================
//  Owner-drawn toolbar buttons
// =============================================================================
int SceneEditor::iconIndex(int id) {
    if (addableMesh(id - ID_SHAPE_BASE)) return id - ID_SHAPE_BASE;
    switch (id) {
    case ID_TOOL_SELECT: return 8;
    case ID_TOOL_MOVE:   return 9;
    case ID_TOOL_ROTATE: return 10;
    case ID_DELETE:      return 11;
    case ID_DELETE_ALL:  return 12;
    default:             return -1;
    }
}

BOOL SceneEditor::onDrawItem(const DRAWITEMSTRUCT& d) {
    const int  id = static_cast<int>(d.CtlID);
    const RECT rc = d.rcItem;

    const bool activeTool =
        (id == ID_TOOL_SELECT && mode == EditMode::Select) ||
        (id == ID_TOOL_MOVE   && mode == EditMode::Move)   ||
        (id == ID_TOOL_ROTATE && mode == EditMode::Rotate);

    HBRUSH bg = CreateSolidBrush(activeTool ? RGB(196, 219, 249) : GetSysColor(COLOR_BTNFACE));
    FillRect(d.hDC, &rc, bg);
    DeleteObject(bg);

    RECT edge = rc;
    DrawEdge(d.hDC, &edge, ((d.itemState & ODS_SELECTED) || activeTool) ? EDGE_SUNKEN : EDGE_RAISED, BF_RECT);

    if (const int i = iconIndex(id); i >= 0 && icons[i]) {
        // 32x32 PNG drawn 1:1 inside the 36 px button (2 px edge on each side).
        Gdiplus::Graphics g(d.hDC);
        g.DrawImage(icons[i].get(), static_cast<INT>(rc.left + 2), static_cast<INT>(rc.top + 2), 32, 32);
    } else {
        drawFallbackIcon(d.hDC, rc, id);
    }
    return TRUE;
}

void SceneEditor::drawFallbackIcon(HDC dc, const RECT& rc, int id) const {
    const int cx = (rc.left + rc.right) / 2;
    const int cy = (rc.top + rc.bottom) / 2;

    const bool danger = id == ID_DELETE || id == ID_DELETE_ALL;
    HPEN   pen      = CreatePen(PS_SOLID, 2, danger ? RGB(178, 44, 44) : RGB(45, 45, 45));
    HGDIOBJ oldPen   = SelectObject(dc, pen);
    HGDIOBJ oldBrush = SelectObject(dc, GetStockObject(NULL_BRUSH));
    const int oldBk  = SetBkMode(dc, TRANSPARENT);

    auto poly = [&](std::span<const POINT> pts) { Polygon(dc, pts.data(), static_cast<int>(pts.size())); };
    auto line = [&](int x0, int y0, int x1, int y1) { MoveToEx(dc, x0, y0, nullptr); LineTo(dc, x1, y1); };
    auto at   = [&](double r, double a) {
        return POINT{ cx + std::lround(r * std::cos(a)), cy + std::lround(r * std::sin(a)) };
    };

    if (auto shape = addableMesh(id - ID_SHAPE_BASE)) {
        switch (*shape) {
        case MeshType::Sphere:
            Ellipse(dc, cx - 9, cy - 9, cx + 9, cy + 9);
            Arc(dc, cx - 9, cy - 4, cx + 9, cy + 4, cx - 9, cy, cx + 9, cy);   // equator
            break;
        case MeshType::Diamond: {
            const POINT g[] = { { cx - 9, cy - 2 }, { cx - 5, cy - 8 }, { cx + 5, cy - 8 }, { cx + 9, cy - 2 }, { cx, cy + 9 } };
            poly(g);
            line(cx - 5, cy - 8, cx, cy + 9);
            line(cx + 5, cy - 8, cx, cy + 9);
            line(cx - 9, cy - 2, cx + 9, cy - 2);
            break;
        }
        case MeshType::Cube: {
            const POINT top[]   = { { cx - 8, cy - 2 }, { cx, cy - 7 }, { cx + 8, cy - 2 }, { cx, cy + 3 } };
            const POINT left[]  = { { cx - 8, cy - 2 }, { cx, cy + 3 }, { cx, cy + 10 }, { cx - 8, cy + 5 } };
            const POINT right[] = { { cx + 8, cy - 2 }, { cx, cy + 3 }, { cx, cy + 10 }, { cx + 8, cy + 5 } };
            poly(top); poly(left); poly(right);
            break;
        }
        case MeshType::Pyramid: {
            const POINT t[] = { { cx, cy - 9 }, { cx - 9, cy + 8 }, { cx + 9, cy + 8 } };
            poly(t);
            line(cx, cy - 9, cx + 2, cy + 4);
            line(cx + 2, cy + 4, cx - 9, cy + 8);
            line(cx + 2, cy + 4, cx + 9, cy + 8);
            break;
        }
        case MeshType::Cylinder:
            Ellipse(dc, cx - 8, cy - 9, cx + 8, cy - 3);
            line(cx - 8, cy - 6, cx - 8, cy + 6);
            line(cx + 8, cy - 6, cx + 8, cy + 6);
            Arc(dc, cx - 8, cy + 3, cx + 8, cy + 9, cx - 8, cy + 6, cx + 8, cy + 6);   // bottom rim
            break;
        case MeshType::Dodecahedron: {
            std::array<POINT, 5> outer, inner;
            for (int k = 0; k < 5; ++k) {
                const double a = -std::numbers::pi / 2 + k * 2 * std::numbers::pi / 5;
                outer[k] = at(10.0, a);
                inner[k] = at(4.5, a + std::numbers::pi / 5);
            }
            poly(outer); poly(inner);
            for (int k = 0; k < 5; ++k) line(outer[k].x, outer[k].y, inner[(k + 4) % 5].x, inner[(k + 4) % 5].y);
            break;
        }
        case MeshType::Supertoroid:
            Ellipse(dc, cx - 10, cy - 7, cx + 10, cy + 7);
            Ellipse(dc, cx - 4, cy - 3, cx + 4, cy + 3);
            break;
        case MeshType::Supershape: {
            std::array<POINT, 24> s;
            for (int k = 0; k < 24; ++k) {
                const double a = k * 2 * std::numbers::pi / 24;
                s[k] = at(6.5 + 3.0 * std::cos(6.0 * a), a);
            }
            poly(s);
            break;
        }
        default: break;
        }
    } else {
        switch (id) {
        case ID_TOOL_SELECT: {   // mouse-cursor arrow
            const POINT a[] = { { cx - 4, cy - 9 }, { cx - 4, cy + 7 }, { cx - 1, cy + 4 }, { cx + 2, cy + 10 },
                                { cx + 4, cy + 9 }, { cx + 1, cy + 3 }, { cx + 6, cy + 3 } };
            HGDIOBJ prev = SelectObject(dc, GetStockObject(WHITE_BRUSH));
            poly(a);
            SelectObject(dc, prev);
            break;
        }
        case ID_TOOL_MOVE: {     // four-way arrows
            line(cx - 10, cy, cx + 10, cy);
            line(cx, cy - 10, cx, cy + 10);
            const POINT heads[4][3] = {
                { { cx + 10, cy }, { cx + 5, cy - 4 }, { cx + 5, cy + 4 } },
                { { cx - 10, cy }, { cx - 5, cy - 4 }, { cx - 5, cy + 4 } },
                { { cx, cy - 10 }, { cx - 4, cy - 5 }, { cx + 4, cy - 5 } },
                { { cx, cy + 10 }, { cx - 4, cy + 5 }, { cx + 4, cy + 5 } },
            };
            for (const auto& h : heads) poly(h);
            break;
        }
        case ID_TOOL_ROTATE: {   // circular arrow
            Arc(dc, cx - 8, cy - 8, cx + 8, cy + 8, cx + 8, cy - 3, cx + 6, cy + 6);
            const POINT head[] = { { cx + 8, cy - 8 }, { cx + 11, cy - 1 }, { cx + 4, cy - 2 } };
            poly(head);
            break;
        }
        case ID_DELETE:          // trash can
            Rectangle(dc, cx - 6, cy - 4, cx + 6, cy + 9);
            line(cx - 8, cy - 4, cx + 8, cy - 4);
            line(cx - 3, cy - 7, cx + 3, cy - 7);
            line(cx - 3, cy - 7, cx - 3, cy - 4);
            line(cx + 3, cy - 7, cx + 3, cy - 4);
            line(cx - 2, cy - 1, cx - 2, cy + 6);
            line(cx + 2, cy - 1, cx + 2, cy + 6);
            break;
        case ID_DELETE_ALL:      // trash can + cross = wipe everything
            Rectangle(dc, cx - 8, cy - 3, cx + 4, cy + 10);
            line(cx - 10, cy - 3, cx + 6, cy - 3);
            line(cx - 5, cy - 6, cx + 1, cy - 6);
            line(cx - 5, cy - 6, cx - 5, cy - 3);
            line(cx + 1, cy - 6, cx + 1, cy - 3);
            line(cx - 4, cy, cx - 4, cy + 7);
            line(cx, cy, cx, cy + 7);
            line(cx + 6, cy - 10, cx + 12, cy - 4);
            line(cx + 12, cy - 10, cx + 6, cy - 4);
            break;
        }
    }

    SetBkMode(dc, oldBk);
    SelectObject(dc, oldBrush);
    SelectObject(dc, oldPen);
    DeleteObject(pen);
}

// =============================================================================
//  Viewport mouse: LMB = object operations (per tool), RMB = orbit camera
// =============================================================================
int SceneEditor::localX(int x) const noexcept {
    return std::clamp(x - viewOffsetX, 0, static_cast<int>(WIDTH) - 1);
}

void SceneEditor::onWheel(float steps) {
    camera.zoom(steps);
    resetAccum = true;
}

void SceneEditor::onRButtonDown(int x, int y) {
    orbiting  = true;
    lastMouse = { x, y };
    SetCapture(viewHwnd);
    SetFocus(viewHwnd);
}

void SceneEditor::onLButtonDown(int x, int y) {
    lmbDown   = true;
    dragMoved = false;
    downMouse = lastMouse = { x, y };
    SetCapture(viewHwnd);
    SetFocus(viewHwnd);

    // Anchor this interaction to the half it started in; positions are then
    // handled in that view's local coordinates (deltas stay raw).
    viewOffsetX = (x >= static_cast<int>(WIDTH)) ? static_cast<int>(WIDTH) : 0;
    const int lx = localX(x);

    if (mode == EditMode::Select) {
        mq0 = mq1 = { lx, y };
        marqueeOn = false;   // becomes true once the drag exceeds the threshold
        return;
    }

    // Move / rotate: grab the object under the cursor. Clicking an unselected
    // object makes it the (sole, unless Shift) selection first.
    const auto hit = pickObject(lx, y);
    if (!hit) return;
    if (!objects[*hit].selected) {
        if (!shiftHeld()) clearSelection();
        objects[*hit].selected = true;
        markSceneDirty();
    }

    if (mode == EditMode::Move) {
        grabPlaneY = objects[*hit].pos.y;
        const Ray r = camera.rayThroughPixel(static_cast<float>(lx), static_cast<float>(y));
        float t = (std::fabs(r.dir.y) > 1e-4f) ? (grabPlaneY - r.origin.y) / r.dir.y : camera.distance();
        if (t < 0.1f) t = camera.distance();
        grabPoint = r.origin + r.dir * t;
        dragHoriz = {};
        dragVert  = 0.0f;
        dragOrig.clear();
        for (std::size_t i = 0; i < objects.size(); ++i)
            if (objects[i].selected) dragOrig.emplace_back(i, objects[i].pos);
        movingObjs = true;
    } else {
        rotatingObjs = true;
    }
}

void SceneEditor::onMouseMove(int x, int y) {
    const int dx = x - lastMouse.x;
    const int dy = y - lastMouse.y;
    lastMouse = { x, y };

    if (orbiting) {
        camera.orbit(static_cast<float>(dx) * ORBIT_SPEED, static_cast<float>(dy) * ORBIT_SPEED);
        resetAccum = true;
        return;
    }
    if (!lmbDown) return;

    if (std::abs(x - downMouse.x) + std::abs(y - downMouse.y) > DRAG_THRESHOLD)
        dragMoved = true;

    switch (mode) {
    case EditMode::Select:
        if (dragMoved) {
            marqueeOn = true;
            mq1 = { localX(x), y };   // drawn by the shaders via the UBO / push constants
        }
        break;

    case EditMode::Move:
        if (!movingObjs) break;
        if (shiftHeld()) {
            dragVert -= static_cast<float>(dy) * 0.0015f * camera.distance();   // vertical while Shift is held
        } else {
            const Ray r = camera.rayThroughPixel(static_cast<float>(localX(x)), static_cast<float>(y));
            if (std::fabs(r.dir.y) > 1e-4f) {
                const float t = (grabPlaneY - r.origin.y) / r.dir.y;
                if (t > 0.1f) {
                    const Vec3 p = r.origin + r.dir * t;
                    dragHoriz = { p.x - grabPoint.x, 0.0f, p.z - grabPoint.z };
                }
            }
        }
        for (const auto& [i, orig] : dragOrig)
            objects[i].pos = orig + Vec3{ dragHoriz.x, dragVert, dragHoriz.z };
        markSceneDirty();
        break;

    case EditMode::Rotate: {
        if (!rotatingObjs || (dx == 0 && dy == 0)) break;
        // dx spins about the world Y axis, dy about the camera's right axis.
        Mat3 dR = Mat3::rotation(WORLD_UP, static_cast<float>(dx) * ROTATE_SPEED);
        if (dy) dR = Mat3::rotation(camera.right(), static_cast<float>(dy) * ROTATE_SPEED) * dR;
        for (SceneObject& o : objects)
            if (o.selected) o.rot = (dR * o.rot).orthonormalized();
        markSceneDirty();
        break;
    }
    }
}

void SceneEditor::onLButtonUp(int x, int y) {
    if (!lmbDown) return;
    lmbDown = false;

    if (mode == EditMode::Select) {
        if (marqueeOn) {
            marqueeSelect(shiftHeld());
            marqueeOn = false;
        } else if (const auto hit = pickObject(localX(x), y); shiftHeld()) {
            if (hit) { objects[*hit].selected = !objects[*hit].selected; markSceneDirty(); }
        } else {
            const bool hadSelection = std::ranges::any_of(objects, &SceneObject::selected);
            clearSelection();
            if (hit) objects[*hit].selected = true;
            if (hadSelection || hit) markSceneDirty();
        }
    }
    movingObjs   = false;
    rotatingObjs = false;
    dragOrig.clear();

    // Last: releasing capture sends WM_CAPTURECHANGED, which resets the drag state.
    if (!orbiting) ReleaseCapture();
}

// Capture taken away (Alt+Tab, a modal box, ...): end every interaction so no
// button stays logically "held" without its matching button-up message.
void SceneEditor::onCaptureLost() {
    orbiting     = false;
    lmbDown      = false;
    movingObjs   = false;
    rotatingObjs = false;
    marqueeOn    = false;
    dragOrig.clear();
}

// =============================================================================
//  Editor actions
// =============================================================================
void SceneEditor::markSceneDirty() noexcept {
    sceneDirty = true;
    resetAccum = true;   // restart accumulation so edits show up clean
}

void SceneEditor::clearSelection() noexcept {
    for (SceneObject& o : objects) o.selected = false;
}

void SceneEditor::addObject(MeshType type) {
    if (FIXED_SLOTS + objects.size() >= MAX_INSTANCES) {
        const auto text = std::format(L"Instance limit reached ({}).", MAX_INSTANCES);
        MessageBoxW(hwnd, text.c_str(), L"Scene Editor", MB_OK | MB_ICONINFORMATION);
        return;
    }

    // Walk a golden-angle spiral so new objects don't stack on each other.
    spawnAngle += 2.39996f;
    const float radius = 4.2f + 1.4f * static_cast<float>((objects.size() / 6) % 3);

    SceneObject o;
    o.mesh     = type;
    o.color    = SPAWN_PALETTE[objects.size() % SPAWN_PALETTE.size()];
    o.material = Material::Glass;
    o.pos      = { std::cos(spawnAngle) * radius,
                   -mesh(type).minY,                // base sits on the floor
                   std::sin(spawnAngle) * radius };
    o.selected = true;                              // the new object becomes the selection

    clearSelection();
    objects.push_back(o);
    markSceneDirty();
}

void SceneEditor::deleteSelected() {
    if (std::erase_if(objects, [](const SceneObject& o) { return o.selected; }) > 0)
        markSceneDirty();
}

void SceneEditor::deleteAll() {
    if (objects.empty()) return;
    objects.clear();
    markSceneDirty();
}

// =============================================================================
//  Picking and marquee selection
// =============================================================================
Vec3 SceneEditor::worldCenter(const SceneObject& o) const noexcept {
    return o.pos + o.rot * (mesh(o.mesh).bcenter * o.scale);
}

// CPU ray against each object's bounding sphere; nearest hit wins.
std::optional<std::size_t> SceneEditor::pickObject(int px, int py) const noexcept {
    const Ray r = camera.rayThroughPixel(static_cast<float>(px), static_cast<float>(py));

    std::optional<std::size_t> best;
    float bestT = 1e30f;
    for (std::size_t i = 0; i < objects.size(); ++i) {
        const SceneObject& o = objects[i];
        const float radius = mesh(o.mesh).bradius * o.scale;
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
    const Vec3 right = camera.right();

    if (!additive) clearSelection();
    for (SceneObject& o : objects) {
        const Vec3 c = worldCenter(o);
        const auto sc = camera.project(c);
        if (!sc) continue;

        // Projected radius from a second point offset along the camera's right
        // axis, which stays perpendicular to the view at any pitch.
        float r = 4.0f;   // minimum radius in pixels
        if (const auto se = camera.project(c + right * (mesh(o.mesh).bradius * o.scale)))
            r = std::max(r, std::hypot(se->x - sc->x, se->y - sc->y));

        const float nx = std::clamp(sc->x, x0, x1);   // closest point of the rectangle
        const float ny = std::clamp(sc->y, y0, y1);
        if (std::hypot(sc->x - nx, sc->y - ny) <= r) o.selected = true;
    }
    markSceneDirty();
}

Float4 SceneEditor::marqueeRect() const noexcept {
    if (!marqueeOn) return MARQUEE_OFF;
    return { static_cast<float>(std::min(mq0.x, mq1.x)), static_cast<float>(std::min(mq0.y, mq1.y)),
             static_cast<float>(std::max(mq0.x, mq1.x)), static_cast<float>(std::max(mq0.y, mq1.y)) };
}

#include "scene_editor.h"

#include <numbers>

// One project-wide pi (C++20 <numbers>), instead of per-function literals.
constexpr float PI = std::numbers::pi_v<float>;

#include <array>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <stdexcept>
#include <string>

#include "config.h"
#include "vk_utils.h"
#include "rt_functions.h"

void SceneEditor::run(HINSTANCE hInst) 
{
    createWindow(hInst);
    initVulkan();
    mainLoop();
    cleanup();
}

int SceneEditor::iconIndex(int id) 
{
    if (id >= ID_SHAPE_BASE && id < ID_SHAPE_BASE + ADDABLE_MESHES) return id - ID_SHAPE_BASE;

    switch (id)
    {
	    case ID_TOOL_SELECT: return 8;
	    case ID_TOOL_MOVE:   return 9;
	    case ID_TOOL_ROTATE: return 10;
	    case ID_DELETE:      return 11;
	    case ID_DELETE_ALL:  return 12;
    }

    return -1;
}

LRESULT CALLBACK SceneEditor::mainProc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    SceneEditor* self = reinterpret_cast<SceneEditor*>(GetWindowLongPtr(h, GWLP_USERDATA));
    switch (msg) {
    case WM_CREATE: {
        auto* cs = reinterpret_cast<CREATESTRUCT*>(lp);
        SetWindowLongPtr(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
        return 0;
    }
    case WM_COMMAND:
        if (self) self->onCommand(LOWORD(wp));
        return 0;
    case WM_DRAWITEM:
        if (self) return self->onDrawItem(reinterpret_cast<DRAWITEMSTRUCT*>(lp));
        break;
    case WM_MOUSEWHEEL:
        if (self) self->onWheel((float)GET_WHEEL_DELTA_WPARAM(wp) / 120.0f);
        return 0;
    case WM_KEYDOWN:
        if (self) {
            if (wp == VK_ESCAPE) self->quit = true;
            if (wp == VK_DELETE) self->deleteSelected();
            if (wp == 'D')       self->denoiseEnabled = !self->denoiseEnabled;
        }
        return 0;
    case WM_CLOSE:
        if (self) self->quit = true;
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProc(h, msg, wp, lp);
}

LRESULT CALLBACK SceneEditor::viewProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    SceneEditor* self = reinterpret_cast<SceneEditor*>(GetWindowLongPtr(h, GWLP_USERDATA));
    switch (msg) {
    case WM_CREATE: {
        auto* cs = reinterpret_cast<CREATESTRUCT*>(lp);
        SetWindowLongPtr(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
        return 0;
    }
    case WM_RBUTTONDOWN:
        if (self) {
            self->orbiting = true;
            self->lastMouse.x = (short)LOWORD(lp);
            self->lastMouse.y = (short)HIWORD(lp);
            SetCapture(h);
            SetFocus(h);
        }
        return 0;
    case WM_RBUTTONUP:
        if (self) { self->orbiting = false; if (!self->lmbDown) ReleaseCapture(); }
        return 0;
    case WM_LBUTTONDOWN:
        if (self) self->onLButtonDown((short)LOWORD(lp), (short)HIWORD(lp), h);
        return 0;
    case WM_LBUTTONUP:
        if (self) self->onLButtonUp((short)LOWORD(lp), (short)HIWORD(lp));
        return 0;
    case WM_MOUSEMOVE:
        if (self) self->onMouseMove((short)LOWORD(lp), (short)HIWORD(lp));
        return 0;
    case WM_MOUSEWHEEL:
        if (self) self->onWheel((float)GET_WHEEL_DELTA_WPARAM(wp) / 120.0f);
        return 0;
    case WM_KEYDOWN:
        if (self) {
            if (wp == VK_ESCAPE) self->quit = true;
            if (wp == VK_DELETE) self->deleteSelected();
            if (wp == 'D')       self->denoiseEnabled = !self->denoiseEnabled;
        }
        return 0;
    }
    return DefWindowProc(h, msg, wp, lp);
}

void SceneEditor::onWheel(float delta) {
    camDist *= std::pow(0.9f, delta);
    if (camDist < 3.0f)  camDist = 3.0f;
    if (camDist > 30.0f) camDist = 30.0f;
    cameraDirty = true;
}

void SceneEditor::onLButtonDown(int x, int y, HWND h) {
    lmbDown = true;
    dragMoved = false;
    downMouse.x = x; downMouse.y = y;
    lastMouse.x = x; lastMouse.y = y;
    SetCapture(h);
    SetFocus(h);

    // Anchor this interaction to the half it started in; positional input is
    // then handled in that view's local coordinates (deltas stay raw).
    viewOffsetX = (x >= (int)WIDTH) ? (int)WIDTH : 0;
    int lx = localX(x);

    if (mode == MODE_SELECT) {
        mq0.x = lx; mq0.y = y;
        mq1 = mq0;
        marqueeOn = false;          // becomes true once the drag exceeds the threshold
        return;
    }

    // Move / rotate: grab the object under the cursor. Clicking an
    // unselected object makes it the (sole) selection first.
    int hit = pickObject(lx, y);
    if (hit < 0) return;
    if (!objects[hit].sel) {
        if (!shiftHeld()) clearSelection();
        objects[hit].sel = true;
        markSceneDirty();
    }

    if (mode == MODE_MOVE) {
        grabPlaneY = objects[hit].pos.y;
        Vec3 eye, dir;
        rayThroughPixel(lx, y, eye, dir);
        float t = (std::fabs(dir.y) > 1e-4f) ? (grabPlaneY - eye.y) / dir.y : camDist;
        if (t < 0.1f) t = camDist;
        grabPoint = eye + dir * t;
        dragHoriz = { 0, 0, 0 };
        dragVert  = 0.0f;
        dragOrig.clear();
        for (size_t i = 0; i < objects.size(); ++i)
            if (objects[i].sel) dragOrig.push_back({ (int)i, objects[i].pos });
        movingObjs = true;
    } else { // MODE_ROTATE
        rotatingObjs = true;
    }
}

void SceneEditor::onMouseMove(int x, int y) {
    int dx = x - lastMouse.x;
    int dy = y - lastMouse.y;

    if (orbiting) {
        camYaw   += dx * 0.005f;
        camPitch += dy * 0.005f;
        const float lim = 1.5f; // ~85 degrees
        if (camPitch >  lim) camPitch =  lim;
        if (camPitch < -lim) camPitch = -lim;
        cameraDirty = true;
        lastMouse.x = x; lastMouse.y = y;
        return;
    }
    if (!lmbDown) { lastMouse.x = x; lastMouse.y = y; return; }

    if (std::abs(x - downMouse.x) + std::abs(y - downMouse.y) > 3)
        dragMoved = true;

    if (mode == MODE_SELECT) {
        if (dragMoved) {
            marqueeOn = true;
            mq1.x = localX(x); mq1.y = y;   // picked up by updateUniforms -> drawn by the shader
        }
    } else if (mode == MODE_MOVE && movingObjs) {
        if (shiftHeld()) {
            dragVert -= (float)dy * 0.0015f * camDist;   // vertical while Shift is held
        } else {
            Vec3 eye, dir;
            rayThroughPixel(localX(x), y, eye, dir);
            if (std::fabs(dir.y) > 1e-4f) {
                float t = (grabPlaneY - eye.y) / dir.y;
                if (t > 0.1f) {
                    Vec3 p = eye + dir * t;
                    dragHoriz = { p.x - grabPoint.x, 0.0f, p.z - grabPoint.z };
                }
            }
        }
        for (auto& [i, orig] : dragOrig)
            objects[i].pos = orig + Vec3{ dragHoriz.x, dragVert, dragHoriz.z };
        markSceneDirty();
    } else if (mode == MODE_ROTATE && rotatingObjs && (dx || dy)) {
        // dx spins about the world Y axis, dy about the camera's right axis.
        Vec3 eye; Mat4 vi, pi;
        cameraBasis(eye, vi, pi);
        Vec3 fwd   = (Vec3{ 0, 1, 0 } - eye).normalize();   // toward the orbit centre (0,1,0)
        Vec3 right = (fwd.cross({ 0, 1, 0 })).normalize();
        Mat3 dR = rot_axis({ 0, 1, 0 }, dx * 0.01f);
        if (dy) dR = rot_axis(right, dy * 0.01f) * dR;
        for (auto& o : objects)
            if (o.sel) o.rot = dR * o.rot;
        markSceneDirty();
    }

    lastMouse.x = x; lastMouse.y = y;
}

void SceneEditor::onLButtonUp(int x, int y) {
    if (!lmbDown) return;
    lmbDown = false;
    if (!orbiting) ReleaseCapture();

    if (mode == MODE_SELECT) {
        if (marqueeOn) {
            marqueeSelect(shiftHeld());
            marqueeOn = false;
        } else {
            int hit = pickObject(localX(x), y);
            if (shiftHeld()) {
                if (hit >= 0) { objects[hit].sel = !objects[hit].sel; markSceneDirty(); }
            } else {
                bool changed = false;
                for (auto& o : objects) { if (o.sel) changed = true; o.sel = false; }
                if (hit >= 0) { objects[hit].sel = true; changed = true; }
                if (changed) markSceneDirty();
            }
        }
    }
    movingObjs = false;
    rotatingObjs = false;
    dragOrig.clear();
}

void SceneEditor::createWindow(HINSTANCE hInst) {
    WNDCLASSEX wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = mainProc;
    wc.hInstance = hInst;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = "VkRtEditorMain";
    RegisterClassEx(&wc);

    WNDCLASSEX vc{};
    vc.cbSize = sizeof(vc);
    vc.style = CS_HREDRAW | CS_VREDRAW;
    vc.lpfnWndProc = viewProc;
    vc.hInstance = hInst;
    vc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    vc.lpszClassName = "VkRtEditorView";
    RegisterClassEx(&vc);

    // Fixed-size window (no resize) keeps the swapchain logic simple.
    DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    RECT rect = { 0, 0, (LONG)(WIDTH * VIEWS), (LONG)(TB_H + HEIGHT) };
    AdjustWindowRect(&rect, style, FALSE);

    hwnd = CreateWindowEx(0, wc.lpszClassName,
        "Vulkan Ray Tracing vs Path Tracing - Scene Editor", style,
        CW_USEDEFAULT, CW_USEDEFAULT,
        rect.right - rect.left, rect.bottom - rect.top,
        nullptr, nullptr, hInst, this);

    viewHwnd = CreateWindowEx(0, vc.lpszClassName, "",
        WS_CHILD | WS_VISIBLE,
        0, (int)TB_H, WIDTH * VIEWS, HEIGHT,
        hwnd, nullptr, hInst, this);

    createToolbar(hInst);

    ShowWindow(hwnd, SW_SHOW);
    UpdateWindow(hwnd);
}

void SceneEditor::createToolbar(HINSTANCE hInst) {
    // Load the PNG icons (same relative-path convention as the shaders).
    // A missing or unreadable file falls back to the GDI-drawn icon.
    static const wchar_t* kIconNames[ICON_COUNT] = {
        L"sphere", L"diamond", L"cube", L"pyramid", L"cylinder",
        L"dodecahedron", L"supertoroid", L"supershape",
        L"select", L"move", L"rotate", L"delete", L"delete_all"
    };
    for (int i = 0; i < ICON_COUNT; ++i) {
        std::wstring path = std::wstring(L"icons/") + kIconNames[i] + L".png";
        auto* b = new Gdiplus::Bitmap(path.c_str());
        if (b->GetLastStatus() != Gdiplus::Ok) { delete b; b = nullptr; }
        iconBmp[i] = b;
    }

    const int bs = 36, gap = 4, y = ((int)TB_H - bs) / 2;
    int x = 6;

    auto btn = [&](int id) -> HWND {
        HWND b = CreateWindowEx(0, "BUTTON", "",
            WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
            x, y, bs, bs, hwnd, (HMENU)(INT_PTR)id, hInst, nullptr);
        x += bs + gap;
        return b;
    };
    auto sep = [&]() {
        CreateWindowEx(0, "STATIC", "", WS_CHILD | WS_VISIBLE | SS_ETCHEDVERT,
            x + 2, y + 4, 2, bs - 8, hwnd, nullptr, hInst, nullptr);
        x += 12;
    };

    for (int i = 0; i < ADDABLE_MESHES; ++i) btn(ID_SHAPE_BASE + i);
    sep();
    hToolBtn[0] = btn(ID_TOOL_SELECT);
    hToolBtn[1] = btn(ID_TOOL_MOVE);
    hToolBtn[2] = btn(ID_TOOL_ROTATE);
    sep();
    btn(ID_DELETE);
    btn(ID_DELETE_ALL);
}

void SceneEditor::onCommand(int id) {
    if (id >= ID_SHAPE_BASE && id < ID_SHAPE_BASE + ADDABLE_MESHES) {
        addObject(id - ID_SHAPE_BASE);
    } else if (id == ID_TOOL_SELECT || id == ID_TOOL_MOVE || id == ID_TOOL_ROTATE) {
        mode = (id == ID_TOOL_SELECT) ? MODE_SELECT
             : (id == ID_TOOL_MOVE)   ? MODE_MOVE : MODE_ROTATE;
        for (HWND b : hToolBtn) InvalidateRect(b, nullptr, TRUE);
    } else if (id == ID_DELETE) {
        deleteSelected();
    } else if (id == ID_DELETE_ALL) {
        deleteAll();
    }
    SetFocus(viewHwnd);   // keep Del / Esc working after a button click
}

BOOL SceneEditor::onDrawItem(DRAWITEMSTRUCT* d) {
    int  id = (int)d->CtlID;
    HDC  dc = d->hDC;
    RECT rc = d->rcItem;

    bool activeTool =
        (id == ID_TOOL_SELECT && mode == MODE_SELECT) ||
        (id == ID_TOOL_MOVE   && mode == MODE_MOVE)   ||
        (id == ID_TOOL_ROTATE && mode == MODE_ROTATE);

    HBRUSH bg = CreateSolidBrush(activeTool ? RGB(196, 219, 249) : GetSysColor(COLOR_BTNFACE));
    FillRect(dc, &rc, bg);
    DeleteObject(bg);
    DrawEdge(dc, &rc, ((d->itemState & ODS_SELECTED) || activeTool) ? EDGE_SUNKEN : EDGE_RAISED, BF_RECT);

    int ii = iconIndex(id);
    if (ii >= 0 && iconBmp[ii]) {
        // 32x32 PNG drawn 1:1 inside the 36px button (2px edge each side).
        Gdiplus::Graphics g(dc);
        g.DrawImage(iconBmp[ii], (INT)(rc.left + 2), (INT)(rc.top + 2), 32, 32);
    } else {
        drawIcon(dc, rc, id);
    }
    return TRUE;
}

void SceneEditor::drawIcon(HDC dc, RECT rc, int id) {
    int cx = (rc.left + rc.right) / 2;
    int cy = (rc.top + rc.bottom) / 2;

    bool danger = (id == ID_DELETE || id == ID_DELETE_ALL);
    HPEN pen  = CreatePen(PS_SOLID, 2, danger ? RGB(178, 44, 44) : RGB(45, 45, 45));
    HPEN old  = (HPEN)SelectObject(dc, pen);
    HBRUSH nb = (HBRUSH)GetStockObject(NULL_BRUSH);
    HBRUSH ob = (HBRUSH)SelectObject(dc, nb);
    int bk = SetBkMode(dc, TRANSPARENT);

    auto P = [&](POINT* pts, int n) { Polygon(dc, pts, n); };
    auto L = [&](int x0, int y0, int x1, int y1) { MoveToEx(dc, x0, y0, nullptr); LineTo(dc, x1, y1); };

    switch (id - ID_SHAPE_BASE >= 0 && id - ID_SHAPE_BASE < ADDABLE_MESHES ? id - ID_SHAPE_BASE : -1) {
    case M_SPHERE:
        Ellipse(dc, cx - 9, cy - 9, cx + 9, cy + 9);
        Arc(dc, cx - 9, cy - 4, cx + 9, cy + 4, cx - 9, cy, cx + 9, cy);   // equator (lower half)
        break;
    case M_DIAMOND: {
        POINT g[5] = { {cx - 9, cy - 2}, {cx - 5, cy - 8}, {cx + 5, cy - 8}, {cx + 9, cy - 2}, {cx, cy + 9} };
        P(g, 5);
        L(cx - 5, cy - 8, cx, cy + 9);
        L(cx + 5, cy - 8, cx, cy + 9);
        L(cx - 9, cy - 2, cx + 9, cy - 2);
        break;
    }
    case M_CUBE: {
        POINT top[4]   = { {cx - 8, cy - 2}, {cx, cy - 7}, {cx + 8, cy - 2}, {cx, cy + 3} };
        POINT left[4]  = { {cx - 8, cy - 2}, {cx, cy + 3}, {cx, cy + 10}, {cx - 8, cy + 5} };
        POINT right[4] = { {cx + 8, cy - 2}, {cx, cy + 3}, {cx, cy + 10}, {cx + 8, cy + 5} };
        P(top, 4); P(left, 4); P(right, 4);
        break;
    }
    case M_PYRAMID: {
        POINT t[3] = { {cx, cy - 9}, {cx - 9, cy + 8}, {cx + 9, cy + 8} };
        P(t, 3);
        L(cx, cy - 9, cx + 2, cy + 4);
        L(cx + 2, cy + 4, cx - 9, cy + 8);
        L(cx + 2, cy + 4, cx + 9, cy + 8);
        break;
    }
    case M_CYLINDER:
        Ellipse(dc, cx - 8, cy - 9, cx + 8, cy - 3);
        L(cx - 8, cy - 6, cx - 8, cy + 6);
        L(cx + 8, cy - 6, cx + 8, cy + 6);
        Arc(dc, cx - 8, cy + 3, cx + 8, cy + 9, cx - 8, cy + 6, cx + 8, cy + 6);   // bottom (lower half)
        break;
    case M_DODECAHEDRON: {
        POINT o5[5], i5[5];
        for (int k = 0; k < 5; ++k) {
            double ao = -1.5707963 + k * 1.2566371;
            double ai = ao + 0.6283185;
            o5[k] = { cx + (LONG)std::lround(10 * std::cos(ao)), cy + (LONG)std::lround(10 * std::sin(ao)) };
            i5[k] = { cx + (LONG)std::lround(4.5 * std::cos(ai)), cy + (LONG)std::lround(4.5 * std::sin(ai)) };
        }
        P(o5, 5); P(i5, 5);
        for (int k = 0; k < 5; ++k) L(o5[k].x, o5[k].y, i5[(k + 4) % 5].x, i5[(k + 4) % 5].y);
        break;
    }
    case M_SUPERTOROID:
        Ellipse(dc, cx - 10, cy - 7, cx + 10, cy + 7);
        Ellipse(dc, cx - 4, cy - 3, cx + 4, cy + 3);
        break;
    case M_SUPERSHAPE: {
        POINT s[24];
        for (int k = 0; k < 24; ++k) {
            double a = k * 6.2831853 / 24.0;
            double r = 6.5 + 3.0 * std::cos(6.0 * a);
            s[k] = { cx + (LONG)std::lround(r * std::cos(a)), cy + (LONG)std::lround(r * std::sin(a)) };
        }
        P(s, 24);
        break;
    }
    default:
        switch (id) {
        case ID_TOOL_SELECT: {   // mouse-cursor arrow
            POINT a[7] = { {cx - 4, cy - 9}, {cx - 4, cy + 7}, {cx - 1, cy + 4}, {cx + 2, cy + 10},
                           {cx + 4, cy + 9}, {cx + 1, cy + 3}, {cx + 6, cy + 3} };
            HBRUSH wb = (HBRUSH)GetStockObject(WHITE_BRUSH);
            HBRUSH pb = (HBRUSH)SelectObject(dc, wb);
            P(a, 7);
            SelectObject(dc, pb);
            break;
        }
        case ID_TOOL_MOVE: {     // four-way arrows
            L(cx - 10, cy, cx + 10, cy);
            L(cx, cy - 10, cx, cy + 10);
            POINT ar[3];
            ar[0] = { cx + 10, cy }; ar[1] = { cx + 5, cy - 4 }; ar[2] = { cx + 5, cy + 4 }; P(ar, 3);
            ar[0] = { cx - 10, cy }; ar[1] = { cx - 5, cy - 4 }; ar[2] = { cx - 5, cy + 4 }; P(ar, 3);
            ar[0] = { cx, cy - 10 }; ar[1] = { cx - 4, cy - 5 }; ar[2] = { cx + 4, cy - 5 }; P(ar, 3);
            ar[0] = { cx, cy + 10 }; ar[1] = { cx - 4, cy + 5 }; ar[2] = { cx + 4, cy + 5 }; P(ar, 3);
            break;
        }
        case ID_TOOL_ROTATE: {   // circular arrow
            Arc(dc, cx - 8, cy - 8, cx + 8, cy + 8, cx + 8, cy - 3, cx + 6, cy + 6);
            POINT ar[3] = { {cx + 8, cy - 8}, {cx + 11, cy - 1}, {cx + 4, cy - 2} };
            P(ar, 3);
            break;
        }
        case ID_DELETE: {        // trash can
            Rectangle(dc, cx - 6, cy - 4, cx + 6, cy + 9);
            L(cx - 8, cy - 4, cx + 8, cy - 4);
            L(cx - 3, cy - 7, cx + 3, cy - 7);
            L(cx - 3, cy - 7, cx - 3, cy - 4);
            L(cx + 3, cy - 7, cx + 3, cy - 4);
            L(cx - 2, cy - 1, cx - 2, cy + 6);
            L(cx + 2, cy - 1, cx + 2, cy + 6);
            break;
        }
        case ID_DELETE_ALL: {    // trash can + asterisk = wipe everything
            Rectangle(dc, cx - 8, cy - 3, cx + 4, cy + 10);
            L(cx - 10, cy - 3, cx + 6, cy - 3);
            L(cx - 5, cy - 6, cx + 1, cy - 6);
            L(cx - 5, cy - 6, cx - 5, cy - 3);
            L(cx + 1, cy - 6, cx + 1, cy - 3);
            L(cx - 4, cy, cx - 4, cy + 7);
            L(cx, cy, cx, cy + 7);
            L(cx + 6, cy - 10, cx + 12, cy - 4);
            L(cx + 12, cy - 10, cx + 6, cy - 4);
            break;
        }
        }
        break;
    }

    SetBkMode(dc, bk);
    SelectObject(dc, ob);
    SelectObject(dc, old);
    DeleteObject(pen);
}

void SceneEditor::markSceneDirty() {
    sceneDirty  = true;
    cameraDirty = true;   // restart accumulation so edits show up clean
}

void SceneEditor::clearSelection() {
    for (auto& o : objects) o.sel = false;
}

void SceneEditor::addObject(int meshType) {
    if (FIXED_SLOTS + objects.size() >= MAX_INSTANCES) {
        MessageBoxA(hwnd, "Instance limit reached (64).", "Scene Editor", MB_OK | MB_ICONINFORMATION);
        return;
    }
    static const Vec3 palette[8] = {
        { 0.55f, 0.72f, 1.00f }, { 1.00f, 0.65f, 0.72f }, { 0.55f, 0.95f, 0.65f },
        { 1.00f, 0.85f, 0.45f }, { 0.45f, 0.92f, 0.92f }, { 0.78f, 0.60f, 1.00f },
        { 1.00f, 0.55f, 0.35f }, { 0.90f, 0.90f, 0.98f },
    };

    SceneObject o;
    o.mesh  = (meshType >= 0 && meshType < ADDABLE_MESHES) ? meshType : M_SPHERE;
    o.color = palette[objects.size() % 8];
    o.matId = 2.0f;               // glass, the showcase material
    o.reflectivity = 0.0f;

    // Walk a golden-angle spiral so new objects don't stack on each other.
    spawnAngle += 2.39996f;
    float rad = 4.2f + 1.4f * (float)((objects.size() / 6) % 3);
    o.pos = { std::cos(spawnAngle) * rad,
              -meshes[o.mesh].minY,          // base sits on the floor
              std::sin(spawnAngle) * rad };

    // The new object becomes the selection, ready to be moved.
    clearSelection();
    o.sel = true;
    objects.push_back(o);
    markSceneDirty();
}

void SceneEditor::deleteSelected() {
    size_t before = objects.size();
    objects.erase(std::remove_if(objects.begin(), objects.end(),
        [](const SceneObject& o) { return o.sel; }), objects.end());
    if (objects.size() != before) markSceneDirty();
}

void SceneEditor::deleteAll() {
    if (objects.empty()) return;
    objects.clear();
    markSceneDirty();
}

void SceneEditor::cameraBasis(Vec3& eye, Mat4& viewInv, Mat4& projInv) {
    Vec3 center{ 0.0f, 1.0f, 0.0f };
    Vec3 up{ 0.0f, 1.0f, 0.0f };
    float cp = std::cos(camPitch), sp = std::sin(camPitch);
    float cy = std::cos(camYaw),   sy = std::sin(camYaw);
    eye = { center.x + camDist * cp * cy,
            center.y + camDist * sp,
            center.z + camDist * cp * sy };
    viewInv = lookAtRH(eye, center, up).inverse();
    projInv = perspectiveVk(60.0f * PI / 180.0f,(float)WIDTH / (float)HEIGHT, 0.1f, 100.0f).inverse();
}

void SceneEditor::rayThroughPixel(int px, int py, Vec3& eye, Vec3& dir) {
    Mat4 viewInv, projInv;
    cameraBasis(eye, viewInv, projInv);
    float dx = 2.0f * ((px + 0.5f) / (float)WIDTH)  - 1.0f;
    float dy = 2.0f * ((py + 0.5f) / (float)HEIGHT) - 1.0f;
    float clip[4] = { dx, dy, 1.0f, 1.0f }, tgt[4];
    projInv.multiply(clip, tgt);
    Vec3 t3 = (Vec3{ tgt[0], tgt[1], tgt[2] }).normalize();
    float dir4[4] = { t3.x, t3.y, t3.z, 0.0f }, out4[4];
    viewInv.multiply(dir4, out4);
    dir = (Vec3{ out4[0], out4[1], out4[2] }).normalize();
}

bool SceneEditor::projectToScreen(Vec3 w, float& sx, float& sy) {
    Vec3 center{ 0.0f, 1.0f, 0.0f };
    Vec3 up{ 0.0f, 1.0f, 0.0f };
    float cp = std::cos(camPitch), sp = std::sin(camPitch);
    float cy = std::cos(camYaw),   sy2 = std::sin(camYaw);
    Vec3 eye{ center.x + camDist * cp * cy,
              center.y + camDist * sp,
              center.z + camDist * cp * sy2 };
    Mat4 view = lookAtRH(eye, center, up);
    Mat4 proj = perspectiveVk(60.0f * PI / 180.0f,
                              (float)WIDTH / (float)HEIGHT, 0.1f, 100.0f);
    float v4[4] = { w.x, w.y, w.z, 1.0f }, e4[4], c4[4];
    view.multiply(v4, e4);
    proj.multiply(e4, c4);
    if (c4[3] <= 1e-5f) return false;
    // perspectiveVk bakes the Vulkan Y flip in, so NDC y already points down
    // like window coordinates do.
    sx = (c4[0] / c4[3] * 0.5f + 0.5f) * (float)WIDTH;
    sy = (c4[1] / c4[3] * 0.5f + 0.5f) * (float)HEIGHT;
    return true;
}

int SceneEditor::pickObject(int px, int py) {
    Vec3 eye, dir;
    rayThroughPixel(px, py, eye, dir);

    int   best  = -1;
    float bestT = 1e30f;
    for (size_t i = 0; i < objects.size(); ++i) {
        const SceneObject& o = objects[i];
        const MeshInfo& mi = meshes[o.mesh];
        Vec3  c = o.pos + o.rot * (mi.bcenter * o.scale);
        float r = mi.bradius * o.scale;
        Vec3  oc = eye - c;
        float b  = oc.dot(dir);
        float cc = oc.dot(oc) - r * r;
        float h  = b * b - cc;
        if (h < 0.0f) continue;
        h = std::sqrt(h);
        float t = -b - h; if (t < 0.001f) t = -b + h;
        if (t > 0.001f && t < bestT) { bestT = t; best = (int)i; }
    }
    return best;
}

void SceneEditor::marqueeSelect(bool additive) {
    float x0 = (float)std::min(mq0.x, mq1.x), x1 = (float)std::max(mq0.x, mq1.x);
    float y0 = (float)std::min(mq0.y, mq1.y), y1 = (float)std::max(mq0.y, mq1.y);

    if (!additive) clearSelection();
    for (auto& o : objects) {
        const MeshInfo& mi = meshes[o.mesh];
        Vec3 c = o.pos + o.rot * (mi.bcenter * o.scale);
        float sx, sy;
        if (!projectToScreen(c, sx, sy)) continue;
        // Approximate projected radius from a second projected point.
        float rx, ry;
        float r = 4.0f;   // minimum clickable radius in pixels
        if (projectToScreen(c + Vec3{ 0, mi.bradius * o.scale, 0 }, rx, ry)) {
            float dxp = rx - sx, dyp = ry - sy;
            float pr = std::sqrt(dxp * dxp + dyp * dyp);
            if (pr > r) r = pr;
        }
        float nx = std::max(x0, std::min(sx, x1));   // closest point of the rect
        float ny = std::max(y0, std::min(sy, y1));
        float ddx = sx - nx, ddy = sy - ny;
        if (ddx * ddx + ddy * ddy <= r * r) o.sel = true;
    }
    markSceneDirty();
}

void SceneEditor::initVulkan() {
    createInstance();
    createSurface();
    pickPhysicalDevice();
    createDevice();
    loadRayTracingFunctions(dev);
    createSwapchain();
    createCommandResources();
    createPrototypeMeshes();
    createDefaultScene();
    createAccelStructures();
    createStorageImage();
    createUniformBuffer();
    createDescriptors();
    createRayTracingPipeline();
    createShaderBindingTable();
    createDenoiser();
    startTime = std::chrono::high_resolution_clock::now();
}

VKAPI_ATTR VkBool32 VKAPI_CALL SceneEditor::debugCB(
    VkDebugUtilsMessageSeverityFlagBitsEXT severity,
    VkDebugUtilsMessageTypeFlagsEXT,
    const VkDebugUtilsMessengerCallbackDataEXT* data, void*) {
    if (severity >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT)
        std::fprintf(stderr, "[Vulkan] %s\n", data->pMessage);
    return VK_FALSE;
}

void SceneEditor::createInstance() {
    VkApplicationInfo app{ VK_STRUCTURE_TYPE_APPLICATION_INFO };
    app.pApplicationName = "VkRtSceneEditor";
    app.apiVersion = VK_API_VERSION_1_2;

    std::vector<const char*> exts = {
        VK_KHR_SURFACE_EXTENSION_NAME,
        VK_KHR_WIN32_SURFACE_EXTENSION_NAME,
    };
    std::vector<const char*> layers;
    if (kEnableValidation) {
        exts.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
        layers.push_back("VK_LAYER_KHRONOS_validation");
    }

    VkInstanceCreateInfo ci{ VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO };
    ci.pApplicationInfo = &app;
    ci.enabledExtensionCount = (uint32_t)exts.size();
    ci.ppEnabledExtensionNames = exts.data();
    ci.enabledLayerCount = (uint32_t)layers.size();
    ci.ppEnabledLayerNames = layers.data();
    vkCheck(vkCreateInstance(&ci, nullptr, &instance), "vkCreateInstance");

    if (kEnableValidation) {
        auto fn = (PFN_vkCreateDebugUtilsMessengerEXT)vkGetInstanceProcAddr(
            instance, "vkCreateDebugUtilsMessengerEXT");
        if (fn) {
            VkDebugUtilsMessengerCreateInfoEXT d{ VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT };
            d.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
                                VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
            d.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                            VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                            VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
            d.pfnUserCallback = debugCB;
            fn(instance, &d, nullptr, &debugMessenger);
        }
    }
}

void SceneEditor::createSurface() {
    VkWin32SurfaceCreateInfoKHR ci{ VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR };
    ci.hinstance = GetModuleHandle(nullptr);
    ci.hwnd = viewHwnd;   // swapchain lives in the viewport child window
    vkCheck(vkCreateWin32SurfaceKHR(instance, &ci, nullptr, &surface), "vkCreateWin32SurfaceKHR");
}

bool SceneEditor::deviceSupportsRt(VkPhysicalDevice d) {
    uint32_t n = 0;
    vkEnumerateDeviceExtensionProperties(d, nullptr, &n, nullptr);
    std::vector<VkExtensionProperties> props(n);
    vkEnumerateDeviceExtensionProperties(d, nullptr, &n, props.data());
    auto has = [&](const char* name) {
        for (auto& p : props) if (std::strcmp(p.extensionName, name) == 0) return true;
        return false;
    };
    return has(VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME) &&
           has(VK_KHR_RAY_TRACING_PIPELINE_EXTENSION_NAME) &&
           has(VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME) &&
           has(VK_KHR_SWAPCHAIN_EXTENSION_NAME);
}

void SceneEditor::pickPhysicalDevice() {
    uint32_t n = 0;
    vkEnumeratePhysicalDevices(instance, &n, nullptr);
    if (n == 0) throw std::runtime_error("No Vulkan devices found");
    std::vector<VkPhysicalDevice> devices(n);
    vkEnumeratePhysicalDevices(instance, &n, devices.data());

    for (auto d : devices) {
        if (!deviceSupportsRt(d)) continue;
        // Needs a queue with graphics + present.
        uint32_t qn = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(d, &qn, nullptr);
        std::vector<VkQueueFamilyProperties> qprops(qn);
        vkGetPhysicalDeviceQueueFamilyProperties(d, &qn, qprops.data());
        for (uint32_t i = 0; i < qn; ++i) {
            VkBool32 present = VK_FALSE;
            vkGetPhysicalDeviceSurfaceSupportKHR(d, i, surface, &present);
            if ((qprops[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) && present) {
                phys = d;
                queueFamily = i;
                break;
            }
        }
        if (phys != VK_NULL_HANDLE) break;
    }
    if (phys == VK_NULL_HANDLE)
        throw std::runtime_error("No GPU with VK_KHR_ray_tracing_pipeline support found.\n"
                                 "A ray-tracing capable GPU and recent drivers are required.");

    rtProps.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_PROPERTIES_KHR;
    VkPhysicalDeviceProperties2 props2{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2 };
    props2.pNext = &rtProps;
    vkGetPhysicalDeviceProperties2(phys, &props2);

    VkPhysicalDeviceProperties p;
    vkGetPhysicalDeviceProperties(phys, &p);
    std::printf("GPU: %s\n", p.deviceName);
}

void SceneEditor::createDevice() {
    float priority = 1.0f;
    VkDeviceQueueCreateInfo q{ VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO };
    q.queueFamilyIndex = queueFamily;
    q.queueCount = 1;
    q.pQueuePriorities = &priority;

    const char* deviceExts[] = {
        VK_KHR_SWAPCHAIN_EXTENSION_NAME,
        VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME,
        VK_KHR_RAY_TRACING_PIPELINE_EXTENSION_NAME,
        VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME,
    };

    VkPhysicalDeviceRayTracingPipelineFeaturesKHR rtFeat{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_FEATURES_KHR };
    rtFeat.rayTracingPipeline = VK_TRUE;

    VkPhysicalDeviceAccelerationStructureFeaturesKHR asFeat{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR };
    asFeat.accelerationStructure = VK_TRUE;
    asFeat.pNext = &rtFeat;

    VkPhysicalDeviceVulkan12Features v12{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES };
    v12.bufferDeviceAddress = VK_TRUE;
    v12.pNext = &asFeat;

    VkPhysicalDeviceFeatures2 f2{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2 };
    f2.pNext = &v12;

    VkDeviceCreateInfo ci{ VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO };
    ci.pNext = &f2;
    ci.queueCreateInfoCount = 1;
    ci.pQueueCreateInfos = &q;
    ci.enabledExtensionCount = (uint32_t)(sizeof(deviceExts) / sizeof(deviceExts[0]));
    ci.ppEnabledExtensionNames = deviceExts;
    vkCheck(vkCreateDevice(phys, &ci, nullptr, &dev), "vkCreateDevice");

    vkGetDeviceQueue(dev, queueFamily, 0, &queue);
}

void SceneEditor::createSwapchain() {
    VkSurfaceCapabilitiesKHR caps;
    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(phys, surface, &caps);

    uint32_t fn = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(phys, surface, &fn, nullptr);
    std::vector<VkSurfaceFormatKHR> formats(fn);
    vkGetPhysicalDeviceSurfaceFormatsKHR(phys, surface, &fn, formats.data());

    VkSurfaceFormatKHR chosen = formats[0];
    for (auto& f : formats) {
        if (f.format == VK_FORMAT_B8G8R8A8_UNORM &&
            f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) { chosen = f; break; }
    }
    swapFormat = chosen.format;

    swapExtent = caps.currentExtent;
    if (swapExtent.width == UINT32_MAX) { swapExtent = { WIDTH * VIEWS, HEIGHT }; }

    uint32_t imageCount = caps.minImageCount + 1;
    if (caps.maxImageCount > 0 && imageCount > caps.maxImageCount)
        imageCount = caps.maxImageCount;

    VkSwapchainCreateInfoKHR ci{ VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR };
    ci.surface = surface;
    ci.minImageCount = imageCount;
    ci.imageFormat = chosen.format;
    ci.imageColorSpace = chosen.colorSpace;
    ci.imageExtent = swapExtent;
    ci.imageArrayLayers = 1;
    ci.imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    ci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ci.preTransform = caps.currentTransform;
    ci.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    ci.presentMode = VK_PRESENT_MODE_FIFO_KHR;
    ci.clipped = VK_TRUE;
    vkCheck(vkCreateSwapchainKHR(dev, &ci, nullptr, &swapchain), "vkCreateSwapchainKHR");

    uint32_t n = 0;
    vkGetSwapchainImagesKHR(dev, swapchain, &n, nullptr);
    swapImages.resize(n);
    vkGetSwapchainImagesKHR(dev, swapchain, &n, swapImages.data());
}

void SceneEditor::createCommandResources() {
    VkCommandPoolCreateInfo pci{ VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
    pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pci.queueFamilyIndex = queueFamily;
    vkCheck(vkCreateCommandPool(dev, &pci, nullptr, &cmdPool), "vkCreateCommandPool");

    VkCommandBufferAllocateInfo ai{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
    ai.commandPool = cmdPool;
    ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ai.commandBufferCount = 1;
    vkCheck(vkAllocateCommandBuffers(dev, &ai, &cmd), "vkAllocateCommandBuffers");

    VkSemaphoreCreateInfo sci{ VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
    vkCheck(vkCreateSemaphore(dev, &sci, nullptr, &semImageAvailable), "sem");

    // One render-finished semaphore PER SWAPCHAIN IMAGE. A single shared
    // semaphore gets re-signalled by the next submit while the previous
    // present may still be using it (VUID-vkQueueSubmit-pSignalSemaphores);
    // indexing by the acquired image is the approach the spec recommends.
    semRenderFinished.resize(swapImages.size());
    for (auto& s : semRenderFinished)
        vkCheck(vkCreateSemaphore(dev, &sci, nullptr, &s), "sem");

    VkFenceCreateInfo fci{ VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
    fci.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    vkCheck(vkCreateFence(dev, &fci, nullptr, &inFlight), "fence");
}

uint32_t SceneEditor::findMemoryType(uint32_t typeBits, VkMemoryPropertyFlags props) {
    VkPhysicalDeviceMemoryProperties mp;
    vkGetPhysicalDeviceMemoryProperties(phys, &mp);
    for (uint32_t i = 0; i < mp.memoryTypeCount; ++i) {
        if ((typeBits & (1u << i)) &&
            (mp.memoryTypes[i].propertyFlags & props) == props)
            return i;
    }
    throw std::runtime_error("No suitable memory type");
}

Buffer SceneEditor::createBuffer(VkDeviceSize size, VkBufferUsageFlags usage, VkMemoryPropertyFlags memProps) {
    Buffer b; b.size = size;
    VkBufferCreateInfo ci{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
    ci.size = size;
    ci.usage = usage | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
    ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    vkCheck(vkCreateBuffer(dev, &ci, nullptr, &b.buf), "vkCreateBuffer");

    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(dev, b.buf, &req);

    VkMemoryAllocateFlagsInfo flags{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO };
    flags.flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT;

    VkMemoryAllocateInfo ai{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    ai.pNext = &flags;
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = findMemoryType(req.memoryTypeBits, memProps);
    vkCheck(vkAllocateMemory(dev, &ai, nullptr, &b.mem), "vkAllocateMemory");
    vkCheck(vkBindBufferMemory(dev, b.buf, b.mem, 0), "vkBindBufferMemory");
    return b;
}

void SceneEditor::uploadToBuffer(Buffer& b, const void* data, VkDeviceSize size) {
    void* mapped = nullptr;
    vkCheck(vkMapMemory(dev, b.mem, 0, size, 0, &mapped), "vkMapMemory");
    std::memcpy(mapped, data, (size_t)size);
    vkUnmapMemory(dev, b.mem);
}

VkDeviceAddress SceneEditor::bufferAddress(VkBuffer buf) {
    VkBufferDeviceAddressInfo ai{ VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO };
    ai.buffer = buf;
    return pvkGetBufferDeviceAddress(dev, &ai);
}

VkCommandBuffer SceneEditor::beginOneShot() {
    VkCommandBufferAllocateInfo ai{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
    ai.commandPool = cmdPool;
    ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ai.commandBufferCount = 1;
    VkCommandBuffer c;
    vkAllocateCommandBuffers(dev, &ai, &c);
    VkCommandBufferBeginInfo bi{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(c, &bi);
    return c;
}

void SceneEditor::endOneShot(VkCommandBuffer c) {
    vkEndCommandBuffer(c);
    VkSubmitInfo si{ VK_STRUCTURE_TYPE_SUBMIT_INFO };
    si.commandBufferCount = 1;
    si.pCommandBuffers = &c;
    vkQueueSubmit(queue, 1, &si, VK_NULL_HANDLE);
    vkQueueWaitIdle(queue);
    vkFreeCommandBuffers(dev, cmdPool, 1, &c);
}

void SceneEditor::beginMesh() {
    meshStartV = (uint32_t)protoVerts.size();
    meshStartI = (uint32_t)protoIndices.size();
}

void SceneEditor::endMesh(int type) {
    MeshInfo& m = meshes[type];
    m.firstVert  = meshStartV;
    m.vertCount  = (uint32_t)protoVerts.size()   - meshStartV;
    m.firstIndex = meshStartI;
    m.indexCount = (uint32_t)protoIndices.size() - meshStartI;

    // Bounding sphere (bbox centre + max distance) and lowest point.
    Vec3 lo{ 1e9f, 1e9f, 1e9f }, hi{ -1e9f, -1e9f, -1e9f };
    for (uint32_t i = m.firstVert; i < m.firstVert + m.vertCount; ++i) {
        const float* p = protoVerts[i].pos;
        if (p[0] < lo.x) lo.x = p[0];
        if (p[0] > hi.x) hi.x = p[0];
        if (p[1] < lo.y) lo.y = p[1];
        if (p[1] > hi.y) hi.y = p[1];
        if (p[2] < lo.z) lo.z = p[2];
        if (p[2] > hi.z) hi.z = p[2];
    }
    m.bcenter = (lo + hi) * 0.5f;
    m.minY = lo.y;
    float r2 = 0.0f;
    for (uint32_t i = m.firstVert; i < m.firstVert + m.vertCount; ++i) {
        Vec3 p{ protoVerts[i].pos[0], protoVerts[i].pos[1], protoVerts[i].pos[2] };
        Vec3 d = p - m.bcenter;
        float dd = d.dot(d);
        if (dd > r2) r2 = dd;
    }
    m.bradius = std::sqrt(r2);
}

void SceneEditor::pushVert(Vec3 p, Vec3 n) {
    Vertex v{};
    v.pos[0] = p.x; v.pos[1] = p.y; v.pos[2] = p.z;
    v.nrm[0] = n.x; v.nrm[1] = n.y; v.nrm[2] = n.z;
    protoVerts.push_back(v);
}

void SceneEditor::genSphere(float radius, int stacks, int slices) {
    uint32_t base = (uint32_t)protoVerts.size();
    for (int i = 0; i <= stacks; ++i) {
        float v = (float)i / stacks;
        float phi = v * PI;              // 0..pi
        for (int j = 0; j <= slices; ++j) {
            float u = (float)j / slices;
            float theta = u * 2.0f * PI; // 0..2pi
            Vec3 n{ std::sin(phi) * std::cos(theta),
                    std::cos(phi),
                    std::sin(phi) * std::sin(theta) };
            pushVert(n * radius, n);
        }
    }
    int ring = slices + 1;
    for (int i = 0; i < stacks; ++i) {
        for (int j = 0; j < slices; ++j) {
            uint32_t a = base + i * ring + j;
            uint32_t b = base + (i + 1) * ring + j;
            protoIndices.push_back(a);     protoIndices.push_back(b);     protoIndices.push_back(a + 1);
            protoIndices.push_back(a + 1); protoIndices.push_back(b);     protoIndices.push_back(b + 1);
        }
    }
}

void SceneEditor::flatTri(Vec3 a, Vec3 b, Vec3 c, Vec3 interior) {
    Vec3 nn = ((b - a).cross(c - a)).normalize();
    Vec3 cen = (a + b + c) * (1.0f / 3.0f);
    if (nn.dot(cen - interior) < 0.0f) nn = nn * -1.0f;
    uint32_t t = (uint32_t)protoVerts.size();
    pushVert(a, nn); pushVert(b, nn); pushVert(c, nn);
    protoIndices.push_back(t); protoIndices.push_back(t + 1); protoIndices.push_back(t + 2);
}

void SceneEditor::genDiamond(float s, int N) {
    const float rt = 0.50f, ht = 0.42f, rg = 1.00f, hp = 1.15f;
    Vec3 c0{ 0, (ht - hp) * 0.5f * s, 0 };
    auto T = [&](int i) { float a = (float)((i % N + N) % N) * 2 * PI / N;
                          return Vec3{ rt * std::cos(a) * s, ht * s, rt * std::sin(a) * s }; };
    auto G = [&](int i) { float a = ((float)((i % N + N) % N) + 0.5f) * 2 * PI / N;
                          return Vec3{ rg * std::cos(a) * s, 0.0f, rg * std::sin(a) * s }; };
    Vec3 Tc{ 0, ht * s, 0 }, C{ 0, -hp * s, 0 };
    for (int i = 0; i < N; ++i) {
        flatTri(Tc, T(i + 1), T(i), c0);
        flatTri(T(i), T(i + 1), G(i), c0);
        flatTri(T(i + 1), G(i + 1), G(i), c0);
        flatTri(G(i), G(i + 1), C, c0);
    }
}

void SceneEditor::genCube(float s) {
    Vec3 nrm[6] = { {1,0,0},{-1,0,0},{0,1,0},{0,-1,0},{0,0,1},{0,0,-1} };
    float h = s * 0.5f;
    for (int f = 0; f < 6; ++f) {
        Vec3 N = nrm[f], u, w;
        if (std::fabs(N.y) > 0.5f) { u = { 1,0,0 }; w = { 0,0,1 }; }
        else { u = { 0,1,0 }; w = N.cross(u); }
        Vec3 c = N * h;
        Vec3 p0 = c - u * h - w * h, p1 = c + u * h - w * h, p2 = c + u * h + w * h, p3 = c - u * h + w * h;
        uint32_t b = (uint32_t)protoVerts.size();
        pushVert(p0, N); pushVert(p1, N); pushVert(p2, N); pushVert(p3, N);
        protoIndices.push_back(b); protoIndices.push_back(b + 1); protoIndices.push_back(b + 2);
        protoIndices.push_back(b); protoIndices.push_back(b + 2); protoIndices.push_back(b + 3);
    }
}

void SceneEditor::genPyramid(float s, float h) {
    float hs = s * 0.5f;
    Vec3 interior{ 0, h * 0.30f, 0 };
    Vec3 b0{ -hs,0,-hs }, b1{ hs,0,-hs }, b2{ hs,0,hs }, b3{ -hs,0,hs };
    Vec3 ap{ 0,h,0 };
    flatTri(b0, b1, ap, interior);
    flatTri(b1, b2, ap, interior);
    flatTri(b2, b3, ap, interior);
    flatTri(b3, b0, ap, interior);
    flatTri(b0, b2, b1, interior);   // base
    flatTri(b0, b3, b2, interior);
}

void SceneEditor::genCylinder(float r, float h, int N) {
    Vec3 base{ 0,0,0 }, top{ 0,h,0 };
    for (int i = 0; i < N; ++i) {
        float a0 = 2 * PI * i / N, a1 = 2 * PI * (i + 1) / N;
        Vec3 d0{ std::cos(a0),0,std::sin(a0) }, d1{ std::cos(a1),0,std::sin(a1) };
        Vec3 bl = base + d0 * r, br = base + d1 * r;
        Vec3 tl = top + d0 * r, tr = top + d1 * r;
        uint32_t b = (uint32_t)protoVerts.size();
        pushVert(bl, d0); pushVert(br, d1); pushVert(tr, d1); pushVert(tl, d0);
        protoIndices.push_back(b); protoIndices.push_back(b + 1); protoIndices.push_back(b + 2);
        protoIndices.push_back(b); protoIndices.push_back(b + 2); protoIndices.push_back(b + 3);
        // caps (flat)
        uint32_t cb = (uint32_t)protoVerts.size();
        pushVert(base, { 0,-1,0 }); pushVert(base + d1 * r, { 0,-1,0 }); pushVert(base + d0 * r, { 0,-1,0 });
        protoIndices.push_back(cb); protoIndices.push_back(cb + 1); protoIndices.push_back(cb + 2);
        uint32_t ct = (uint32_t)protoVerts.size();
        pushVert(top, { 0,1,0 }); pushVert(top + d0 * r, { 0,1,0 }); pushVert(top + d1 * r, { 0,1,0 });
        protoIndices.push_back(ct); protoIndices.push_back(ct + 1); protoIndices.push_back(ct + 2);
    }
}

void SceneEditor::genDodecahedron(float s) {
    const float P = 1.6180339887f, a = 1.0f / P;
    const Vec3 DV[20] = {
        {-1,-1,-1},{-1,-1,1},{-1,1,-1},{-1,1,1},{1,-1,-1},{1,-1,1},{1,1,-1},{1,1,1},
        {0,-a,-P},{0,-a,P},{0,a,-P},{0,a,P},
        {-a,-P,0},{-a,P,0},{a,-P,0},{a,P,0},
        {-P,0,-a},{-P,0,a},{P,0,-a},{P,0,a}
    };
    static const int FACES[12][5] = {
        {14,12,0,8,4},{10,8,0,16,2},{17,16,0,12,1},{5,9,1,12,14},
        {3,17,1,9,11},{6,10,2,13,15},{3,13,2,16,17},{15,13,3,11,7},
        {6,18,4,8,10},{5,14,4,18,19},{11,9,5,19,7},{19,18,6,15,7}
    };
    float scale = s / 1.7320508f;   // circumradius of DV is sqrt(3)
    Vec3 center{ 0,0,0 };
    for (int f = 0; f < 12; ++f) {
        Vec3 v[5];
        for (int k = 0; k < 5; ++k) v[k] = DV[FACES[f][k]] * scale;
        flatTri(v[0], v[1], v[2], center);   // fan
        flatTri(v[0], v[2], v[3], center);
        flatTri(v[0], v[3], v[4], center);
    }
}

void SceneEditor::genSupertoroid(int U, int Vr) {
    const float n = 2.2f, twist = 3.0f, a = 2.2f;
    genGrid(U, Vr, false, [&](int i, int j) -> Vec3 {
        float u = 2 * PI * (float)((i % U + U) % U) / U;
        float v = 2 * PI * (float)((j % Vr + Vr) % Vr) / Vr;
        float cv = std::fabs(std::cos(v)); if (cv < 1e-9f) cv = 1e-9f;
        float sv = std::fabs(std::sin(v)); if (sv < 1e-9f) sv = 1e-9f;
        float Rr = std::pow(std::pow(cv, n) + std::pow(sv, n), -1.0f / n);
        float phi = twist * u + v;
        float r = a + Rr * std::cos(phi);
        return Vec3{ r * std::cos(u), Rr * std::sin(phi), r * std::sin(u) };
    });
}

float SceneEditor::superformula(float ang, float m, float n1, float n2, float n3) {
    float t = m * ang * 0.25f;
    float c = std::pow(std::fabs(std::cos(t)), n2);
    float s = std::pow(std::fabs(std::sin(t)), n3);
    float b = c + s; if (b < 1e-9f) b = 1e-9f;
    float r = std::pow(b, -1.0f / n1);
    return r > 6.0f ? 6.0f : r;
}

void SceneEditor::genSupershape(int U, int Vr) {
    const float m1 = 6, n1 = 5, n2 = 5, n3 = 5;
    auto raw = [&](int i, int j) -> Vec3 {
        float th = -PI + 2 * PI * (float)((i % U + U) % U) / U;
        float ph = -0.5f * PI + PI * (float)((j % Vr + Vr) % Vr) / Vr;
        float r1 = superformula(th, m1, n1, n2, n3);
        float r2 = superformula(ph, m1, n1, n2, n3);
        return Vec3{ r1 * std::cos(th) * r2 * std::cos(ph),
                     r2 * std::sin(ph),
                     r1 * std::sin(th) * r2 * std::cos(ph) };
    };
    float maxR = 1e-6f;
    for (int i = 0; i <= U; ++i)
        for (int j = 0; j <= Vr; ++j) { Vec3 q = raw(i, j); float d = std::sqrt(q.dot(q)); if (d > maxR) maxR = d; }
    float scale = 1.6f / maxR;
    genGrid(U, Vr, true, [&](int i, int j) -> Vec3 { return raw(i, j) * scale; });
}

void SceneEditor::genFloor(float half) {
    uint32_t base = (uint32_t)protoVerts.size();
    Vec3 n{ 0, 1, 0 };
    Vec3 corners[4] = {
        { -half, 0, -half }, {  half, 0, -half },
        {  half, 0,  half }, { -half, 0,  half }
    };
    for (auto& c : corners) pushVert(c, n);
    protoIndices.push_back(base + 0); protoIndices.push_back(base + 1); protoIndices.push_back(base + 2);
    protoIndices.push_back(base + 0); protoIndices.push_back(base + 2); protoIndices.push_back(base + 3);
}

void SceneEditor::createPrototypeMeshes() {
    beginMesh(); genSphere(1.3f);            endMesh(M_SPHERE);
    beginMesh(); genDiamond(1.25f);          endMesh(M_DIAMOND);
    beginMesh(); genCube(2.0f);              endMesh(M_CUBE);
    beginMesh(); genPyramid(2.2f, 2.4f);     endMesh(M_PYRAMID);
    beginMesh(); genCylinder(1.05f, 2.4f, 56); endMesh(M_CYLINDER);
    beginMesh(); genDodecahedron(1.55f);     endMesh(M_DODECAHEDRON);
    beginMesh(); genSupertoroid();           endMesh(M_SUPERTOROID);
    beginMesh(); genSupershape();            endMesh(M_SUPERSHAPE);
    beginMesh(); genFloor(20.0f);            endMesh(M_FLOOR);

    VkBufferUsageFlags geomUsage =
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
        VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR;

    VkDeviceSize vsize = protoVerts.size() * sizeof(Vertex);
    VkDeviceSize isize = protoIndices.size() * sizeof(uint32_t);

    vertexBuffer = createBuffer(vsize, geomUsage,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    indexBuffer = createBuffer(isize, geomUsage,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);

    uploadToBuffer(vertexBuffer, protoVerts.data(), vsize);
    uploadToBuffer(indexBuffer, protoIndices.data(), isize);

    std::printf("Prototype meshes: %zu vertices, %zu triangles across %d BLASes\n",
        protoVerts.size(), protoIndices.size() / 3, (int)MESH_COUNT);
}

void SceneEditor::createDefaultScene() {
    auto add = [&](int mesh, Vec3 pos, Vec3 color, Mat3 rot = Mat3{}) {
        SceneObject o;
        o.mesh = mesh; o.pos = pos; o.color = color; o.rot = rot;
        o.matId = 2.0f;            // glass
        o.reflectivity = 0.0f;
        objects.push_back(o);
    };
    const float ring = 6.2f;
    auto ringPos = [&](int k) {
        float ang = (float)k / 6.0f * 2.0f * PI;
        return Vec3{ std::cos(ang) * ring, 0.0f, std::sin(ang) * ring };
    };

    add(M_SUPERTOROID, { 0.0f, 2.7f, 0.0f }, { 1.0f, 1.0f, 1.0f }, rot_axis({ 1.0f, 0.25f, 0.35f }, 0.7f));

    Vec3 p;
    p = ringPos(0); add(M_SPHERE,       { p.x, 1.3f,  p.z }, { 0.55f, 0.72f, 1.00f });
    p = ringPos(1); add(M_DIAMOND,      { p.x, 1.4375f, p.z }, { 1.00f, 0.65f, 0.72f });
    p = ringPos(2); add(M_CUBE,         { p.x, 1.0f,  p.z }, { 0.55f, 0.95f, 0.65f });
    p = ringPos(3); add(M_PYRAMID,      { p.x, 0.0f,  p.z }, { 1.00f, 0.85f, 0.45f });
    p = ringPos(4); add(M_CYLINDER,     { p.x, 0.0f,  p.z }, { 0.45f, 0.92f, 0.92f });
    p = ringPos(5); add(M_DODECAHEDRON, { p.x, 1.55f, p.z }, { 0.78f, 0.60f, 1.00f });

    sceneDirty = true;
}

void SceneEditor::createAccelStructures() {
    VkDeviceAddress vtxAddr = bufferAddress(vertexBuffer.buf);
    VkDeviceAddress idxAddr = bufferAddress(indexBuffer.buf);

    VkDeviceSize maxScratch = 0;

    // ---- size + create every BLAS ----
    std::array<VkAccelerationStructureGeometryKHR, MESH_COUNT> geoms{};
    std::array<VkAccelerationStructureBuildGeometryInfoKHR, MESH_COUNT> infos{};
    std::array<VkDeviceSize, MESH_COUNT> scratchSizes{};

    for (int m = 0; m < MESH_COUNT; ++m) {
        MeshInfo& mi = meshes[m];

        VkAccelerationStructureGeometryKHR& g = geoms[m];
        g = { VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR };
        g.geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR;
        g.flags = VK_GEOMETRY_OPAQUE_BIT_KHR;
        g.geometry.triangles.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR;
        g.geometry.triangles.vertexFormat = VK_FORMAT_R32G32B32_SFLOAT;
        g.geometry.triangles.vertexData.deviceAddress = vtxAddr;
        g.geometry.triangles.vertexStride = sizeof(Vertex);
        g.geometry.triangles.maxVertex = (uint32_t)protoVerts.size() - 1;
        g.geometry.triangles.indexType = VK_INDEX_TYPE_UINT32;
        g.geometry.triangles.indexData.deviceAddress = idxAddr + mi.firstIndex * sizeof(uint32_t);

        VkAccelerationStructureBuildGeometryInfoKHR& bi = infos[m];
        bi = { VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR };
        bi.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
        bi.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;   // static, built once
        bi.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
        bi.geometryCount = 1; bi.pGeometries = &g;

        uint32_t triCount = mi.indexCount / 3;
        VkAccelerationStructureBuildSizesInfoKHR bs{ VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR };
        pvkGetAccelerationStructureBuildSizes(dev, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR,
            &bi, &triCount, &bs);

        mi.blasBuf = createBuffer(bs.accelerationStructureSize,
            VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        VkAccelerationStructureCreateInfoKHR bci{ VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR };
        bci.buffer = mi.blasBuf.buf; bci.size = bs.accelerationStructureSize;
        bci.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
        vkCheck(pvkCreateAccelerationStructure(dev, &bci, nullptr, &mi.blas), "create BLAS");

        scratchSizes[m] = bs.buildScratchSize;
        if (bs.buildScratchSize > maxScratch) maxScratch = bs.buildScratchSize;
    }

    // ---- TLAS sized for the maximum instance count ----
    instanceBuffer = createBuffer(MAX_INSTANCES * sizeof(VkAccelerationStructureInstanceKHR),
        VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);

    objDataBuffer = createBuffer(MAX_INSTANCES * sizeof(GpuObj),
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);

    tlasGeom = { VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR };
    tlasGeom.geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR;
    tlasGeom.flags = VK_GEOMETRY_OPAQUE_BIT_KHR;
    tlasGeom.geometry.instances.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR;
    tlasGeom.geometry.instances.arrayOfPointers = VK_FALSE;
    tlasGeom.geometry.instances.data.deviceAddress = bufferAddress(instanceBuffer.buf);

    VkAccelerationStructureBuildGeometryInfoKHR ti{ VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR };
    ti.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
    ti.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_BUILD_BIT_KHR;
    ti.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
    ti.geometryCount = 1; ti.pGeometries = &tlasGeom;

    uint32_t maxInst = MAX_INSTANCES;
    VkAccelerationStructureBuildSizesInfoKHR ts{ VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR };
    pvkGetAccelerationStructureBuildSizes(dev, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR,
        &ti, &maxInst, &ts);

    tlasBuffer = createBuffer(ts.accelerationStructureSize,
        VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    VkAccelerationStructureCreateInfoKHR tci{ VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR };
    tci.buffer = tlasBuffer.buf; tci.size = ts.accelerationStructureSize;
    tci.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
    vkCheck(pvkCreateAccelerationStructure(dev, &tci, nullptr, &tlas), "create TLAS");

    if (ts.buildScratchSize > maxScratch) maxScratch = ts.buildScratchSize;
    asScratch = createBuffer(maxScratch, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

    // ---- build every BLAS once (sequential, shared scratch) ----
    VkDeviceAddress scratch = bufferAddress(asScratch.buf);
    for (int m = 0; m < MESH_COUNT; ++m) {
        infos[m].dstAccelerationStructure = meshes[m].blas;
        infos[m].scratchData.deviceAddress = scratch;
        VkAccelerationStructureBuildRangeInfoKHR br{};
        br.primitiveCount = meshes[m].indexCount / 3;
        const VkAccelerationStructureBuildRangeInfoKHR* pbr = &br;
        VkCommandBuffer c = beginOneShot();
        pvkCmdBuildAccelerationStructures(c, 1, &infos[m], &pbr);
        endOneShot(c);   // waits for the queue, so the shared scratch can be reused

        VkAccelerationStructureDeviceAddressInfoKHR ai{ VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR };
        ai.accelerationStructure = meshes[m].blas;
        meshes[m].blasAddr = pvkGetAccelerationStructureDeviceAddress(dev, &ai);
    }

    updateSceneBuffers();   // initial instances + objdata + TLAS build
    sceneDirty = false;
}

void SceneEditor::fillTransform(VkTransformMatrixKHR& t, const Mat3& R, float s, Vec3 T) {
    const Vec3* cols[3] = { &R.c0, &R.c1, &R.c2 };
    for (int c = 0; c < 3; ++c) {
        t.matrix[0][c] = (&cols[c]->x)[0] * s;
        t.matrix[1][c] = (&cols[c]->x)[1] * s;
        t.matrix[2][c] = (&cols[c]->x)[2] * s;
    }
    t.matrix[0][3] = T.x; t.matrix[1][3] = T.y; t.matrix[2][3] = T.z;
}

void SceneEditor::updateSceneBuffers() {
    uint32_t count = FIXED_SLOTS + (uint32_t)objects.size();

    std::vector<VkAccelerationStructureInstanceKHR> inst(count);
    std::vector<GpuObj> objData(count);

    auto writeInst = [&](uint32_t slot, int mesh, const Mat3& R, float s, Vec3 T) {
        VkAccelerationStructureInstanceKHR& I = inst[slot];
        I = {};
        fillTransform(I.transform, R, s, T);
        I.instanceCustomIndex = slot;
        I.mask = 0xFF;
        I.flags = VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR;
        I.accelerationStructureReference = meshes[mesh].blasAddr;
        objData[slot].mesh[0] = meshes[mesh].firstIndex;
    };

    // Slot 0: floor (checker pattern comes from the shader, matId 0).
    writeInst(0, M_FLOOR, Mat3{}, 1.0f, { 0, 0, 0 });
    objData[0].color[0] = 0.8f; objData[0].color[1] = 0.8f; objData[0].color[2] = 0.8f;
    objData[0].color[3] = 0.0f;
    objData[0].params[0] = 0.0f;   // matId floor
    objData[0].params[1] = 0.18f;  // floor reflectivity (low gloss keeps the checker contrast)

    // Slot 1: visible emissive area light. Radius must match lightPos[3]
    // in updateUniforms: sphere prototype radius 1.3 * scale = 1.2.
    writeInst(1, M_SPHERE, Mat3{}, 1.2f / 1.3f, { 4.0f, 7.0f, -2.0f });
    objData[1].color[0] = 1.0f; objData[1].color[1] = 0.95f; objData[1].color[2] = 0.85f;
    objData[1].color[3] = 0.0f;
    objData[1].params[0] = 3.0f;   // matId emissive
    objData[1].params[1] = 0.0f;

    // Editable objects.
    for (size_t i = 0; i < objects.size(); ++i) {
        const SceneObject& o = objects[i];
        uint32_t slot = FIXED_SLOTS + (uint32_t)i;
        writeInst(slot, o.mesh, o.rot, o.scale, o.pos);
        objData[slot].color[0] = o.color.x;
        objData[slot].color[1] = o.color.y;
        objData[slot].color[2] = o.color.z;
        objData[slot].color[3] = o.sel ? 1.0f : 0.0f;
        objData[slot].params[0] = o.matId;
        objData[slot].params[1] = o.reflectivity;
    }

    uploadToBuffer(instanceBuffer, inst.data(), count * sizeof(VkAccelerationStructureInstanceKHR));
    uploadToBuffer(objDataBuffer, objData.data(), count * sizeof(GpuObj));

    rebuildTlas(count);
}

void SceneEditor::rebuildTlas(uint32_t instanceCount) {
    VkAccelerationStructureBuildGeometryInfoKHR ti{ VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR };
    ti.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
    ti.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_BUILD_BIT_KHR;
    ti.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
    ti.dstAccelerationStructure = tlas;
    ti.geometryCount = 1; ti.pGeometries = &tlasGeom;
    ti.scratchData.deviceAddress = bufferAddress(asScratch.buf);

    VkAccelerationStructureBuildRangeInfoKHR tr{};
    tr.primitiveCount = instanceCount;
    const VkAccelerationStructureBuildRangeInfoKHR* ptr = &tr;

    VkCommandBuffer c = beginOneShot();
    pvkCmdBuildAccelerationStructures(c, 1, &ti, &ptr);
    endOneShot(c);
}

void SceneEditor::createStorageImage() {
    // One helper creates any of the per-view / denoiser images: 2D, one mip,
    // optimal tiling, device-local, plus a matching view.
    auto makeImage = [&](VkFormat fmt, VkImageUsageFlags usage,
                         VkImage& img, VkDeviceMemory& mem, VkImageView& view) {
        VkImageCreateInfo ci{ VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
        ci.imageType = VK_IMAGE_TYPE_2D;
        ci.format = fmt;
        ci.extent = { WIDTH, HEIGHT, 1 };       // per-view size, not the window size
        ci.mipLevels = 1;
        ci.arrayLayers = 1;
        ci.samples = VK_SAMPLE_COUNT_1_BIT;
        ci.tiling = VK_IMAGE_TILING_OPTIMAL;
        ci.usage = usage;
        ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        vkCheck(vkCreateImage(dev, &ci, nullptr, &img), "create image");

        VkMemoryRequirements req;
        vkGetImageMemoryRequirements(dev, img, &req);
        VkMemoryAllocateInfo ai{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
        ai.allocationSize = req.size;
        ai.memoryTypeIndex = findMemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        vkCheck(vkAllocateMemory(dev, &ai, nullptr, &mem), "alloc image");
        vkBindImageMemory(dev, img, mem, 0);

        VkImageViewCreateInfo vci{ VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
        vci.image = img;
        vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vci.format = fmt;
        vci.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        vkCheck(vkCreateImageView(dev, &vci, nullptr, &view), "image view");
    };

    // Per-view output + accumulation + history.
    for (RendererView& v : views) {
        makeImage(VK_FORMAT_R8G8B8A8_UNORM,
                  VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                  v.storageImage, v.storageMem, v.storageView);
        makeImage(VK_FORMAT_R32G32B32A32_SFLOAT, VK_IMAGE_USAGE_STORAGE_BIT,
                  v.accumImage, v.accumMem, v.accumView);
        makeImage(VK_FORMAT_R32G32B32A32_SFLOAT, VK_IMAGE_USAGE_STORAGE_BIT,
                  v.histImage, v.histMem, v.histView);
    }

    // Denoiser images (path-traced view only): guide, albedo, ping, pong.
    makeImage(VK_FORMAT_R32G32B32A32_SFLOAT, VK_IMAGE_USAGE_STORAGE_BIT, guideImage,  guideMem,  guideView);
    makeImage(VK_FORMAT_R8G8B8A8_UNORM,      VK_IMAGE_USAGE_STORAGE_BIT, albedoImage, albedoMem, albedoView);
    makeImage(VK_FORMAT_R32G32B32A32_SFLOAT, VK_IMAGE_USAGE_STORAGE_BIT, pingImage,   pingMem,   pingView);
    makeImage(VK_FORMAT_R32G32B32A32_SFLOAT, VK_IMAGE_USAGE_STORAGE_BIT, pongImage,   pongMem,   pongView);

    // One-time transition to GENERAL; these images then stay in GENERAL.
    VkCommandBuffer c = beginOneShot();
    for (RendererView& v : views)
        for (VkImage img : { v.accumImage, v.histImage })
            imageBarrier(c, img,
                VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
                0, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT,
                VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR);
    for (VkImage img : { guideImage, albedoImage, pingImage, pongImage })
        imageBarrier(c, img,
            VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
            0, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT,
            VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
    endOneShot(c);
}

void SceneEditor::createUniformBuffer() {
    ubo = createBuffer(sizeof(UBO), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
}

void SceneEditor::createDescriptors() {
    // One layout serves both views. The Whitted raygen simply does not declare
    // bindings 8/9 (guide/albedo), which is legal: a set layout may contain
    // bindings a pipeline never statically uses.
    std::array<VkDescriptorSetLayoutBinding, 10> bindings{};
    bindings[0] = { 0, VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, 1, VK_SHADER_STAGE_RAYGEN_BIT_KHR, nullptr };
    bindings[1] = { 1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_RAYGEN_BIT_KHR, nullptr };
    bindings[2] = { 2, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_RAYGEN_BIT_KHR, nullptr };
    bindings[3] = { 3, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR, nullptr };
    bindings[4] = { 4, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR, nullptr };
    bindings[5] = { 5, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_RAYGEN_BIT_KHR, nullptr };
    bindings[6] = { 6, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_RAYGEN_BIT_KHR, nullptr };
    bindings[7] = { 7, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR, nullptr };
    bindings[8] = { 8, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_RAYGEN_BIT_KHR, nullptr };
    bindings[9] = { 9, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_RAYGEN_BIT_KHR, nullptr };

    VkDescriptorSetLayoutCreateInfo lci{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
    lci.bindingCount = (uint32_t)bindings.size();
    lci.pBindings = bindings.data();
    vkCheck(vkCreateDescriptorSetLayout(dev, &lci, nullptr, &descLayout), "descriptor layout");

    // Two RT sets (5 storage images each) + three denoiser sets (5 each).
    std::array<VkDescriptorPoolSize, 4> sizes{ {
        { VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, 2 },
        { VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 25 },
        { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 2 },
        { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 6 },
    } };
    VkDescriptorPoolCreateInfo pci{ VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
    pci.maxSets = 5;
    pci.poolSizeCount = (uint32_t)sizes.size();
    pci.pPoolSizes = sizes.data();
    vkCheck(vkCreateDescriptorPool(dev, &pci, nullptr, &descPool), "descriptor pool");

    VkDescriptorSetLayout layouts[2] = { descLayout, descLayout };
    VkDescriptorSet sets[2] = {};
    VkDescriptorSetAllocateInfo ai{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
    ai.descriptorPool = descPool;
    ai.descriptorSetCount = 2;
    ai.pSetLayouts = layouts;
    vkCheck(vkAllocateDescriptorSets(dev, &ai, sets), "alloc descriptor sets");
    views[VIEW_RT].descSet = sets[VIEW_RT];
    views[VIEW_PT].descSet = sets[VIEW_PT];

    // TLAS (shared by both sets; it is rebuilt in place on edits, so the
    // handle written here stays valid for the whole run).
    VkWriteDescriptorSetAccelerationStructureKHR asWrite{
        VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR };
    asWrite.accelerationStructureCount = 1;
    asWrite.pAccelerationStructures = &tlas;

    VkDescriptorBufferInfo uboInfo{ ubo.buf, 0, sizeof(UBO) };
    VkDescriptorBufferInfo vInfo{ vertexBuffer.buf, 0, VK_WHOLE_SIZE };
    VkDescriptorBufferInfo iInfo{ indexBuffer.buf, 0, VK_WHOLE_SIZE };
    VkDescriptorBufferInfo oInfo{ objDataBuffer.buf, 0, VK_WHOLE_SIZE };
    VkDescriptorImageInfo guideInfo{ VK_NULL_HANDLE, guideView,  VK_IMAGE_LAYOUT_GENERAL };
    VkDescriptorImageInfo albInfo  { VK_NULL_HANDLE, albedoView, VK_IMAGE_LAYOUT_GENERAL };

    std::vector<VkDescriptorImageInfo> imgInfos;   // stable addresses for pImageInfo
    imgInfos.reserve(2 * 3);
    std::vector<VkWriteDescriptorSet> writes;
    writes.reserve(2 * 10);

    for (RendererView& v : views) {
        auto write = [&](uint32_t binding, VkDescriptorType type,
                         const VkDescriptorImageInfo* img, const VkDescriptorBufferInfo* buf,
                         const void* next = nullptr) {
            VkWriteDescriptorSet w{ VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
            w.pNext = next;
            w.dstSet = v.descSet; w.dstBinding = binding; w.descriptorCount = 1;
            w.descriptorType = type; w.pImageInfo = img; w.pBufferInfo = buf;
            writes.push_back(w);
        };
        auto imgInfo = [&](VkImageView view) {
            imgInfos.push_back({ VK_NULL_HANDLE, view, VK_IMAGE_LAYOUT_GENERAL });
            return &imgInfos.back();
        };
        write(0, VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, nullptr, nullptr, &asWrite);
        write(1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, imgInfo(v.storageView), nullptr);
        write(2, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, nullptr, &uboInfo);
        write(3, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, nullptr, &vInfo);
        write(4, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, nullptr, &iInfo);
        write(5, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, imgInfo(v.accumView), nullptr);
        write(6, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, imgInfo(v.histView), nullptr);
        write(7, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, nullptr, &oInfo);
        write(8, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, &guideInfo, nullptr);
        write(9, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, &albInfo, nullptr);
    }
    vkUpdateDescriptorSets(dev, (uint32_t)writes.size(), writes.data(), 0, nullptr);
}

VkShaderModule SceneEditor::loadShader(const char* path) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) throw std::runtime_error(std::string("Cannot open shader: ") + path);
    size_t size = (size_t)f.tellg();
    std::vector<char> code(size);
    f.seekg(0);
    f.read(code.data(), size);

    VkShaderModuleCreateInfo ci{ VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
    ci.codeSize = size;
    ci.pCode = reinterpret_cast<const uint32_t*>(code.data());
    VkShaderModule mod;
    vkCheck(vkCreateShaderModule(dev, &ci, nullptr, &mod), "create shader module");
    return mod;
}

void SceneEditor::createRayTracingPipeline() {
    VkPipelineLayoutCreateInfo lci{ VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
    lci.setLayoutCount = 1;
    lci.pSetLayouts = &descLayout;
    vkCheck(vkCreatePipelineLayout(dev, &lci, nullptr, &pipelineLayout), "pipeline layout");

    // The two views differ only in the ray-gen shader; the miss, shadow-miss
    // and closest-hit stages are shared modules.
    const char* rgenPaths[2] = {
        "shaders/raygen_rt.rgen.spv",   // VIEW_RT: Whitted-style ray tracing
        "shaders/raygen_pt.rgen.spv",   // VIEW_PT: Monte Carlo path tracing
    };
    VkShaderModule miss  = loadShader("shaders/miss.rmiss.spv");
    VkShaderModule smiss = loadShader("shaders/shadow.rmiss.spv");
    VkShaderModule chit  = loadShader("shaders/closesthit.rchit.spv");

    for (int vi = 0; vi < 2; ++vi) {
        VkShaderModule rgen = loadShader(rgenPaths[vi]);

        std::array<VkPipelineShaderStageCreateInfo, 4> stages{};
        auto stage = [](VkShaderStageFlagBits st, VkShaderModule m) {
            VkPipelineShaderStageCreateInfo ci{ VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO };
            ci.stage = st; ci.module = m; ci.pName = "main"; return ci;
        };
        stages[0] = stage(VK_SHADER_STAGE_RAYGEN_BIT_KHR, rgen);
        stages[1] = stage(VK_SHADER_STAGE_MISS_BIT_KHR, miss);
        stages[2] = stage(VK_SHADER_STAGE_MISS_BIT_KHR, smiss);
        stages[3] = stage(VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR, chit);

        std::array<VkRayTracingShaderGroupCreateInfoKHR, 4> groups{};
        auto general = [](uint32_t idx) {
            VkRayTracingShaderGroupCreateInfoKHR g{ VK_STRUCTURE_TYPE_RAY_TRACING_SHADER_GROUP_CREATE_INFO_KHR };
            g.type = VK_RAY_TRACING_SHADER_GROUP_TYPE_GENERAL_KHR;
            g.generalShader = idx;
            g.closestHitShader = VK_SHADER_UNUSED_KHR;
            g.anyHitShader = VK_SHADER_UNUSED_KHR;
            g.intersectionShader = VK_SHADER_UNUSED_KHR;
            return g;
        };
        groups[0] = general(0); // raygen
        groups[1] = general(1); // miss
        groups[2] = general(2); // shadow miss
        groups[3] = { VK_STRUCTURE_TYPE_RAY_TRACING_SHADER_GROUP_CREATE_INFO_KHR };
        groups[3].type = VK_RAY_TRACING_SHADER_GROUP_TYPE_TRIANGLES_HIT_GROUP_KHR;
        groups[3].generalShader = VK_SHADER_UNUSED_KHR;
        groups[3].closestHitShader = 3;
        groups[3].anyHitShader = VK_SHADER_UNUSED_KHR;
        groups[3].intersectionShader = VK_SHADER_UNUSED_KHR;

        VkRayTracingPipelineCreateInfoKHR pci{ VK_STRUCTURE_TYPE_RAY_TRACING_PIPELINE_CREATE_INFO_KHR };
        pci.stageCount = (uint32_t)stages.size();
        pci.pStages = stages.data();
        pci.groupCount = (uint32_t)groups.size();
        pci.pGroups = groups.data();
        pci.maxPipelineRayRecursionDepth = 1; // all traceRay calls happen in ray-gen
        pci.layout = pipelineLayout;
        vkCheck(pvkCreateRayTracingPipelines(dev, VK_NULL_HANDLE, VK_NULL_HANDLE,
            1, &pci, nullptr, &views[vi].pipeline), "create RT pipeline");

        vkDestroyShaderModule(dev, rgen, nullptr);
    }

    vkDestroyShaderModule(dev, miss, nullptr);
    vkDestroyShaderModule(dev, smiss, nullptr);
    vkDestroyShaderModule(dev, chit, nullptr);
}

void SceneEditor::createShaderBindingTable() {
    const uint32_t handleSize = rtProps.shaderGroupHandleSize;
    const uint32_t handleAligned = alignUp(handleSize, rtProps.shaderGroupHandleAlignment);
    const uint32_t baseAlign = rtProps.shaderGroupBaseAlignment;

    const uint32_t rgenCount = 1, missCount = 2, hitCount = 1;
    const uint32_t totalGroups = rgenCount + missCount + hitCount;

    // One SBT per view: group handles are pipeline-specific.
    for (RendererView& v : views) {
        // Region strides/sizes (each region base must be baseAlignment-aligned;
        // for ray-gen, size must equal stride).
        v.rgenRegion.stride = alignUp(handleAligned, baseAlign);
        v.rgenRegion.size   = v.rgenRegion.stride;
        v.missRegion.stride = handleAligned;
        v.missRegion.size   = alignUp(missCount * handleAligned, baseAlign);
        v.hitRegion.stride  = handleAligned;
        v.hitRegion.size    = alignUp(hitCount * handleAligned, baseAlign);

        VkDeviceSize sbtSize = v.rgenRegion.size + v.missRegion.size + v.hitRegion.size;

        v.sbtBuffer = createBuffer(sbtSize,
            VK_BUFFER_USAGE_SHADER_BINDING_TABLE_BIT_KHR | VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);

        std::vector<uint8_t> handles(totalGroups * handleSize);
        vkCheck(pvkGetRayTracingShaderGroupHandles(dev, v.pipeline, 0, totalGroups,
            handles.size(), handles.data()), "get shader group handles");

        VkDeviceAddress base = bufferAddress(v.sbtBuffer.buf);
        v.rgenRegion.deviceAddress = base;
        v.missRegion.deviceAddress = base + v.rgenRegion.size;
        v.hitRegion.deviceAddress  = base + v.rgenRegion.size + v.missRegion.size;

        uint8_t* mapped = nullptr;
        vkMapMemory(dev, v.sbtBuffer.mem, 0, sbtSize, 0, (void**)&mapped);
        auto getHandle = [&](uint32_t i) { return handles.data() + i * handleSize; };

        // ray-gen (group 0)
        std::memcpy(mapped, getHandle(0), handleSize);
        // miss (groups 1, 2)
        uint8_t* missDst = mapped + v.rgenRegion.size;
        std::memcpy(missDst, getHandle(1), handleSize);
        std::memcpy(missDst + v.missRegion.stride, getHandle(2), handleSize);
        // hit (group 3)
        uint8_t* hitDst = mapped + v.rgenRegion.size + v.missRegion.size;
        std::memcpy(hitDst, getHandle(3), handleSize);

        vkUnmapMemory(dev, v.sbtBuffer.mem);
    }
}

// -----------------------------------------------------------------------------
//  Denoiser (edge-avoiding a-trous, SVGF-style) -- compute pipeline
// -----------------------------------------------------------------------------
void SceneEditor::createDenoiser() {
    // Layout matches shaders/atrous.comp:
    //   0 colorIn, 1 colorOut, 2 guide, 3 outLDR, 4 albedo -- all storage images.
    std::array<VkDescriptorSetLayoutBinding, 5> bindings{};
    for (uint32_t i = 0; i < 5; ++i)
        bindings[i] = { i, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr };

    VkDescriptorSetLayoutCreateInfo lci{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
    lci.bindingCount = (uint32_t)bindings.size();
    lci.pBindings = bindings.data();
    vkCheck(vkCreateDescriptorSetLayout(dev, &lci, nullptr, &denoiseLayout), "denoise layout");

    VkDescriptorSetLayout layouts[3] = { denoiseLayout, denoiseLayout, denoiseLayout };
    VkDescriptorSetAllocateInfo ai{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
    ai.descriptorPool = descPool;
    ai.descriptorSetCount = 3;
    ai.pSetLayouts = layouts;
    vkCheck(vkAllocateDescriptorSets(dev, &ai, denoiseSets), "alloc denoise sets");

    // Ping-pong wiring. The first pass reads the accumulation buffer (its
    // alpha carries the per-pixel sample count); guide, albedo and the
    // display image are shared by all three sets.
    // The denoiser filters the path-traced view only.
    VkImageView ins [3] = { views[VIEW_PT].accumView, pingView, pongView };
    VkImageView outs[3] = { pingView,  pongView, pingView };

    std::vector<VkDescriptorImageInfo> infos;
    infos.reserve(15);
    std::vector<VkWriteDescriptorSet> writes;
    writes.reserve(15);
    for (int s = 0; s < 3; ++s) {
        VkImageView bound[5] = { ins[s], outs[s], guideView, views[VIEW_PT].storageView, albedoView };
        for (uint32_t b = 0; b < 5; ++b) {
            infos.push_back({ VK_NULL_HANDLE, bound[b], VK_IMAGE_LAYOUT_GENERAL });
            VkWriteDescriptorSet w{ VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
            w.dstSet = denoiseSets[s]; w.dstBinding = b; w.descriptorCount = 1;
            w.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
            w.pImageInfo = &infos.back();
            writes.push_back(w);
        }
    }
    vkUpdateDescriptorSets(dev, (uint32_t)writes.size(), writes.data(), 0, nullptr);

    VkPushConstantRange pc{ VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(DenoisePC) };
    VkPipelineLayoutCreateInfo pli{ VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
    pli.setLayoutCount = 1;
    pli.pSetLayouts = &denoiseLayout;
    pli.pushConstantRangeCount = 1;
    pli.pPushConstantRanges = &pc;
    vkCheck(vkCreatePipelineLayout(dev, &pli, nullptr, &denoisePipeLayout), "denoise pipe layout");

    VkShaderModule comp = loadShader("shaders/atrous.comp.spv");
    VkComputePipelineCreateInfo cpi{ VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO };
    cpi.stage = { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO };
    cpi.stage.stage  = VK_SHADER_STAGE_COMPUTE_BIT;
    cpi.stage.module = comp;
    cpi.stage.pName  = "main";
    cpi.layout = denoisePipeLayout;
    vkCheck(vkCreateComputePipelines(dev, VK_NULL_HANDLE, 1, &cpi, nullptr, &denoisePipeline),
        "denoise pipeline");
    vkDestroyShaderModule(dev, comp, nullptr);
}

void SceneEditor::updateUniforms() {
    auto now = std::chrono::high_resolution_clock::now();
    float t = std::chrono::duration<float>(now - startTime).count();

    // Reset accumulation whenever the camera or scene changed; otherwise keep adding samples.
    if (cameraDirty) { accumFrame = 0; cameraDirty = false; }
    else             { accumFrame++; }
    totalFrame++;   // always advances, so the RNG keeps moving even during motion

    Vec3 eye; Mat4 viewInv, projInv;
    cameraBasis(eye, viewInv, projInv);

    UBO data{};
    data.viewInverse = viewInv;
    data.projInverse = projInv;
    data.lightPos[0] = 4.0f; data.lightPos[1] = 7.0f; data.lightPos[2] = -2.0f;
    data.lightPos[3] = 1.2f;      // light sphere radius (bigger = softer shadows)
    data.params[0] = t;
    data.params[1] = 8.0f;        // max path length (glass needs a few bounces)
    data.params[2] = 45.0f;       // light emission (HDR radiance; the key light of the scene)
    data.params[3] = 24.0f;       // samples/frame (multiple of 3 for even dispersion; lower on weaker GPUs)
    data.frame[0]  = accumFrame;
    data.frame[1]  = totalFrame;
    if (marqueeOn) {
        data.marquee[0] = (float)std::min(mq0.x, mq1.x);
        data.marquee[1] = (float)std::min(mq0.y, mq1.y);
        data.marquee[2] = (float)std::max(mq0.x, mq1.x);
        data.marquee[3] = (float)std::max(mq0.y, mq1.y);
    } else {
        data.marquee[0] = 0.0f; data.marquee[1] = 0.0f;
        data.marquee[2] = -1.0f; data.marquee[3] = -1.0f;   // disabled
    }
    uploadToBuffer(ubo, &data, sizeof(data));
}

void SceneEditor::imageBarrier(VkCommandBuffer c, VkImage img,
    VkImageLayout oldL, VkImageLayout newL,
    VkAccessFlags srcA, VkAccessFlags dstA,
    VkPipelineStageFlags srcS, VkPipelineStageFlags dstS) {
    VkImageMemoryBarrier b{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
    b.oldLayout = oldL; b.newLayout = newL;
    b.srcAccessMask = srcA; b.dstAccessMask = dstA;
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = img;
    b.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    vkCmdPipelineBarrier(c, srcS, dstS, 0, 0, nullptr, 0, nullptr, 1, &b);
}

void SceneEditor::drawFrame() {
    vkWaitForFences(dev, 1, &inFlight, VK_TRUE, UINT64_MAX);

    // Apply pending scene edits: rewrite instances + object data, rebuild
    // the TLAS. The BLASes are never touched, so this is cheap.
    if (sceneDirty) {
        updateSceneBuffers();
        sceneDirty = false;
    }

    uint32_t imageIndex = 0;
    VkResult acq = vkAcquireNextImageKHR(dev, swapchain, UINT64_MAX,
        semImageAvailable, VK_NULL_HANDLE, &imageIndex);
    if (acq != VK_SUCCESS && acq != VK_SUBOPTIMAL_KHR) return;

    // Reset only once we are committed to submitting, so an early return
    // above can never leave the fence unsignalled (frozen next frame).
    vkResetFences(dev, 1, &inFlight);

    updateUniforms();

    vkResetCommandBuffer(cmd, 0);
    VkCommandBufferBeginInfo bi{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    vkBeginCommandBuffer(cmd, &bi);

    // ---- Ray trace both views: Whitted on the left, path traced on the right.
    for (int vi = 0; vi < 2; ++vi) {
        RendererView& v = views[vi];

        // Storage image -> GENERAL for ray tracing writes.
        imageBarrier(cmd, v.storageImage,
            VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
            0, VK_ACCESS_SHADER_WRITE_BIT,
            VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR);

        // Make the previous frame's accumulation writes visible to this frame.
        imageBarrier(cmd, v.accumImage,
            VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL,
            VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT,
            VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR, VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR);

        // Same for the history image (read this frame, written last frame).
        imageBarrier(cmd, v.histImage,
            VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL,
            VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT,
            VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR, VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR);

        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR, v.pipeline);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR,
            pipelineLayout, 0, 1, &v.descSet, 0, nullptr);

        pvkCmdTraceRays(cmd, &v.rgenRegion, &v.missRegion, &v.hitRegion, &v.callRegion,
            WIDTH, HEIGHT, 1);
    }

    // ---- Denoise the path-traced view.
    if (denoiseEnabled) {
        RendererView& pt = views[VIEW_PT];

        // RT writes (accum, guide, albedo) -> compute reads.
        for (VkImage img : { pt.accumImage, guideImage, albedoImage })
            imageBarrier(cmd, img,
                VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL,
                VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
                VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR,
                VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);

        // The raygen shader wrote its raw tone-mapped image into the display
        // image; the denoiser's final pass overwrites it (write-after-write).
        imageBarrier(cmd, pt.storageImage,
            VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL,
            VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_WRITE_BIT,
            VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);

        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, denoisePipeline);

        // Four a-trous iterations with growing step: accum -> ping -> pong
        // -> ping -> (pong + tone-mapped display image with marquee overlay).
        const int      steps[4] = { 1, 2, 4, 8 };
        const int      sets [4] = { 0, 1, 2, 1 };   // ping-pong wiring, see createDenoiser
        const VkImage  outs [4] = { pingImage, pongImage, pingImage, pongImage };
        const uint32_t gx = (WIDTH  + 7) / 8;
        const uint32_t gy = (HEIGHT + 7) / 8;

        // Same marquee state that updateUniforms just wrote into the UBO.
        DenoisePC dpc{};
        dpc.sizeX = (int32_t)WIDTH;
        dpc.sizeY = (int32_t)HEIGHT;
        dpc.exposure = 1.0f;
        if (marqueeOn) {
            dpc.marquee[0] = (float)std::min(mq0.x, mq1.x);
            dpc.marquee[1] = (float)std::min(mq0.y, mq1.y);
            dpc.marquee[2] = (float)std::max(mq0.x, mq1.x);
            dpc.marquee[3] = (float)std::max(mq0.y, mq1.y);
        } else {
            dpc.marquee[0] = 0.0f; dpc.marquee[1] = 0.0f;
            dpc.marquee[2] = -1.0f; dpc.marquee[3] = -1.0f;   // disabled
        }

        for (int i = 0; i < 4; ++i) {
            dpc.step      = steps[i];
            dpc.finalPass = (i == 3) ? 1 : 0;
            dpc.firstPass = (i == 0) ? 1 : 0;
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                denoisePipeLayout, 0, 1, &denoiseSets[sets[i]], 0, nullptr);
            vkCmdPushConstants(cmd, denoisePipeLayout,
                VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(dpc), &dpc);
            vkCmdDispatch(cmd, gx, gy, 1);

            if (i < 3)  // this pass's output is the next pass's input
                imageBarrier(cmd, outs[i],
                    VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL,
                    VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
                    VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                    VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
        }
    }

    // ---- Blit both views into their halves of the swapchain image.
    imageBarrier(cmd, swapImages[imageIndex],
        VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        0, VK_ACCESS_TRANSFER_WRITE_BIT,
        VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);

    for (int vi = 0; vi < 2; ++vi) {
        RendererView& v = views[vi];

        // Storage image -> TRANSFER_SRC (written by the denoiser's final pass
        // for the path-traced view, by the raygen shader otherwise).
        bool computeWrote = denoiseEnabled && vi == VIEW_PT;
        imageBarrier(cmd, v.storageImage,
            VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT,
            computeWrote ? VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT
                         : VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR,
            VK_PIPELINE_STAGE_TRANSFER_BIT);

        // Blit (component-aware: keeps colours correct across RGBA/BGRA).
        VkImageBlit blit{};
        blit.srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
        blit.dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
        blit.srcOffsets[1] = { (int32_t)WIDTH, (int32_t)HEIGHT, 1 };
        blit.dstOffsets[0] = { (int32_t)(vi * WIDTH), 0, 0 };
        blit.dstOffsets[1] = { (int32_t)((vi + 1) * WIDTH), (int32_t)HEIGHT, 1 };
        vkCmdBlitImage(cmd,
            v.storageImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            swapImages[imageIndex], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            1, &blit, VK_FILTER_NEAREST);
    }

    // Swapchain image -> PRESENT.
    imageBarrier(cmd, swapImages[imageIndex],
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
        VK_ACCESS_TRANSFER_WRITE_BIT, 0,
        VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);

    vkEndCommandBuffer(cmd);

    VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
    VkSubmitInfo si{ VK_STRUCTURE_TYPE_SUBMIT_INFO };
    si.waitSemaphoreCount = 1;
    si.pWaitSemaphores = &semImageAvailable;
    si.pWaitDstStageMask = &waitStage;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cmd;
    si.signalSemaphoreCount = 1;
    si.pSignalSemaphores = &semRenderFinished[imageIndex];
    vkCheck(vkQueueSubmit(queue, 1, &si, inFlight), "queue submit");

    VkPresentInfoKHR pi{ VK_STRUCTURE_TYPE_PRESENT_INFO_KHR };
    pi.waitSemaphoreCount = 1;
    pi.pWaitSemaphores = &semRenderFinished[imageIndex];
    pi.swapchainCount = 1;
    pi.pSwapchains = &swapchain;
    pi.pImageIndices = &imageIndex;
    vkQueuePresentKHR(queue, &pi);
}

void SceneEditor::mainLoop() {
    MSG msg{};
    while (!quit) {
        while (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) quit = true;
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }
        if (quit) break;
        drawFrame();
    }
    vkDeviceWaitIdle(dev);
}

void SceneEditor::destroyBuffer(Buffer& b) {
    if (b.buf) vkDestroyBuffer(dev, b.buf, nullptr);
    if (b.mem) vkFreeMemory(dev, b.mem, nullptr);
    b.buf = VK_NULL_HANDLE; b.mem = VK_NULL_HANDLE;
}

void SceneEditor::cleanup() {
    for (auto*& b : iconBmp) { delete b; b = nullptr; }   // before GdiplusShutdown

    for (RendererView& v : views) {
        destroyBuffer(v.sbtBuffer);
        if (v.pipeline) vkDestroyPipeline(dev, v.pipeline, nullptr);
    }
    if (denoisePipeline)   vkDestroyPipeline(dev, denoisePipeline, nullptr);
    if (denoisePipeLayout) vkDestroyPipelineLayout(dev, denoisePipeLayout, nullptr);
    if (denoiseLayout)     vkDestroyDescriptorSetLayout(dev, denoiseLayout, nullptr);
    if (pipelineLayout) vkDestroyPipelineLayout(dev, pipelineLayout, nullptr);
    if (descPool)       vkDestroyDescriptorPool(dev, descPool, nullptr);
    if (descLayout)     vkDestroyDescriptorSetLayout(dev, descLayout, nullptr);

    for (RendererView& v : views) {
        if (v.storageView)  vkDestroyImageView(dev, v.storageView, nullptr);
        if (v.storageImage) vkDestroyImage(dev, v.storageImage, nullptr);
        if (v.storageMem)   vkFreeMemory(dev, v.storageMem, nullptr);
        if (v.accumView)    vkDestroyImageView(dev, v.accumView, nullptr);
        if (v.accumImage)   vkDestroyImage(dev, v.accumImage, nullptr);
        if (v.accumMem)     vkFreeMemory(dev, v.accumMem, nullptr);
        if (v.histView)     vkDestroyImageView(dev, v.histView, nullptr);
        if (v.histImage)    vkDestroyImage(dev, v.histImage, nullptr);
        if (v.histMem)      vkFreeMemory(dev, v.histMem, nullptr);
    }

    if (guideView)   vkDestroyImageView(dev, guideView, nullptr);
    if (guideImage)  vkDestroyImage(dev, guideImage, nullptr);
    if (guideMem)    vkFreeMemory(dev, guideMem, nullptr);
    if (albedoView)  vkDestroyImageView(dev, albedoView, nullptr);
    if (albedoImage) vkDestroyImage(dev, albedoImage, nullptr);
    if (albedoMem)   vkFreeMemory(dev, albedoMem, nullptr);
    if (pingView)    vkDestroyImageView(dev, pingView, nullptr);
    if (pingImage)   vkDestroyImage(dev, pingImage, nullptr);
    if (pingMem)     vkFreeMemory(dev, pingMem, nullptr);
    if (pongView)    vkDestroyImageView(dev, pongView, nullptr);
    if (pongImage)   vkDestroyImage(dev, pongImage, nullptr);
    if (pongMem)     vkFreeMemory(dev, pongMem, nullptr);

    if (tlas) pvkDestroyAccelerationStructure(dev, tlas, nullptr);
    destroyBuffer(tlasBuffer);
    for (int m = 0; m < MESH_COUNT; ++m) {
        if (meshes[m].blas) pvkDestroyAccelerationStructure(dev, meshes[m].blas, nullptr);
        destroyBuffer(meshes[m].blasBuf);
    }
    destroyBuffer(asScratch);
    destroyBuffer(instanceBuffer);
    destroyBuffer(objDataBuffer);
    destroyBuffer(ubo);
    destroyBuffer(vertexBuffer);
    destroyBuffer(indexBuffer);

    if (inFlight) vkDestroyFence(dev, inFlight, nullptr);
    if (semImageAvailable) vkDestroySemaphore(dev, semImageAvailable, nullptr);
    for (auto s : semRenderFinished)
        if (s) vkDestroySemaphore(dev, s, nullptr);
    semRenderFinished.clear();
    if (cmdPool) vkDestroyCommandPool(dev, cmdPool, nullptr);

    if (swapchain) vkDestroySwapchainKHR(dev, swapchain, nullptr);
    if (dev) vkDestroyDevice(dev, nullptr);

    if (debugMessenger) {
        auto fn = (PFN_vkDestroyDebugUtilsMessengerEXT)vkGetInstanceProcAddr(
            instance, "vkDestroyDebugUtilsMessengerEXT");
        if (fn) fn(instance, debugMessenger, nullptr);
    }
    if (surface) vkDestroySurfaceKHR(instance, surface, nullptr);
    if (instance) vkDestroyInstance(instance, nullptr);
}

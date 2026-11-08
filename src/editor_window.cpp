// Win32 window procedures, main window and viewport, the owner-drawn toolbar.
#include "scene_editor.h"

#include <cmath>
#include <numbers>
#include <span>
#include <stdexcept>
#include <string>

namespace {
int xParam(LPARAM lp) noexcept { return static_cast<short>(LOWORD(lp)); }
int yParam(LPARAM lp) noexcept { return static_cast<short>(HIWORD(lp)); }

// Stores the object passed to CreateWindowEx and returns it on later messages.
template <class T>
T* windowSelf(HWND h, UINT msg, LPARAM lp) noexcept {
    if (msg == WM_NCCREATE) {
        const auto* cs = reinterpret_cast<const CREATESTRUCTW*>(lp);
        SetWindowLongPtrW(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
    }
    return reinterpret_cast<T*>(GetWindowLongPtrW(h, GWLP_USERDATA));
}

constexpr const wchar_t* MAIN_CLASS = L"VkRtEditorMain";
constexpr const wchar_t* VIEW_CLASS = L"VkRtEditorView";
} // namespace

// =============================================================================
//  Window procedures. They run inside DispatchMessageW, so an exception must
//  not unwind through user32: it is parked and rethrown by the main loop.
// =============================================================================
LRESULT CALLBACK SceneEditor::mainProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    if (SceneEditor* self = windowSelf<SceneEditor>(h, msg, lp)) {
        try {
            if (const auto result = self->onMainMessage(msg, wp, lp)) return *result;
        } catch (...) {
            self->pendingError_ = std::current_exception();
            return 0;
        }
    }
    if (msg == WM_DESTROY) { PostQuitMessage(0); return 0; }
    return DefWindowProcW(h, msg, wp, lp);
}

LRESULT CALLBACK SceneEditor::viewProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    if (SceneEditor* self = windowSelf<SceneEditor>(h, msg, lp)) {
        try {
            if (const auto result = self->onViewMessage(msg, wp, lp)) return *result;
        } catch (...) {
            self->pendingError_ = std::current_exception();
            return 0;
        }
    }
    return DefWindowProcW(h, msg, wp, lp);
}

std::optional<LRESULT> SceneEditor::onMainMessage(UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_COMMAND:    onCommand(LOWORD(wp)); return 0;
    case WM_DRAWITEM:   return onDrawItem(*reinterpret_cast<const DRAWITEMSTRUCT*>(lp));
    case WM_MOUSEWHEEL: onWheel(GET_WHEEL_DELTA_WPARAM(wp) / static_cast<float>(WHEEL_DELTA)); return 0;
    case WM_KEYDOWN:    onKeyDown(wp, lp); return 0;
    case WM_CLOSE:      quit_ = true; return 0;   // the editor tears the window down after the loop
    default:            return std::nullopt;
    }
}

std::optional<LRESULT> SceneEditor::onViewMessage(UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_RBUTTONDOWN:    onRButtonDown(xParam(lp), yParam(lp)); return 0;
    case WM_RBUTTONUP:      onRButtonUp(); return 0;
    case WM_LBUTTONDOWN:    onLButtonDown(xParam(lp), yParam(lp)); return 0;
    case WM_LBUTTONUP:      onLButtonUp(xParam(lp), yParam(lp)); return 0;
    case WM_MOUSEMOVE:      onMouseMove(xParam(lp), yParam(lp)); return 0;
    case WM_MOUSEWHEEL:     onWheel(GET_WHEEL_DELTA_WPARAM(wp) / static_cast<float>(WHEEL_DELTA)); return 0;
    case WM_KEYDOWN:        onKeyDown(wp, lp); return 0;
    case WM_CAPTURECHANGED: if (reinterpret_cast<HWND>(lp) != viewHwnd_) onCaptureLost(); return 0;
    default:                return std::nullopt;
    }
}

// =============================================================================
//  Window + toolbar
// =============================================================================
void SceneEditor::createWindow(HINSTANCE hInst) {
    const WNDCLASSEXW mainClass{
        .cbSize        = sizeof(WNDCLASSEXW),
        .style         = CS_HREDRAW | CS_VREDRAW,
        .lpfnWndProc   = mainProc,
        .hInstance     = hInst,
        .hCursor       = LoadCursorW(nullptr, IDC_ARROW),
        .hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1),
        .lpszClassName = MAIN_CLASS,
    };
    const WNDCLASSEXW viewClass{
        .cbSize        = sizeof(WNDCLASSEXW),
        .style         = CS_HREDRAW | CS_VREDRAW,
        .lpfnWndProc   = viewProc,
        .hInstance     = hInst,
        .hCursor       = LoadCursorW(nullptr, IDC_ARROW),
        .lpszClassName = VIEW_CLASS,
    };
    if (!RegisterClassExW(&mainClass) || !RegisterClassExW(&viewClass))
        throw std::runtime_error("RegisterClassEx failed");

    // Fixed-size window (no resize) keeps the swapchain logic simple.
    constexpr DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    RECT rect{ 0, 0, static_cast<LONG>(WIDTH * VIEWS), static_cast<LONG>(TOOLBAR_H + HEIGHT) };
    AdjustWindowRect(&rect, style, FALSE);

    hwnd_ = CreateWindowExW(0, MAIN_CLASS, L"Vulkan Ray Tracing vs Path Tracing - Scene Editor", style,
        CW_USEDEFAULT, CW_USEDEFAULT, rect.right - rect.left, rect.bottom - rect.top,
        nullptr, nullptr, hInst, this);
    if (!hwnd_) throw std::runtime_error("CreateWindowEx failed (main window)");

    viewHwnd_ = CreateWindowExW(0, VIEW_CLASS, L"", WS_CHILD | WS_VISIBLE,
        0, static_cast<int>(TOOLBAR_H), static_cast<int>(WIDTH * VIEWS), static_cast<int>(HEIGHT),
        hwnd_, nullptr, hInst, this);
    if (!viewHwnd_) throw std::runtime_error("CreateWindowEx failed (viewport)");

    createToolbar(hInst);

    ShowWindow(hwnd_, SW_SHOW);
    UpdateWindow(hwnd_);
}

void SceneEditor::destroyWindow() noexcept {
    if (!hwnd_) return;
    // Detach first: DestroyWindow sends messages while this object is being destroyed.
    SetWindowLongPtrW(hwnd_, GWLP_USERDATA, 0);
    if (viewHwnd_) SetWindowLongPtrW(viewHwnd_, GWLP_USERDATA, 0);
    DestroyWindow(hwnd_);   // also destroys the child viewport and the toolbar buttons
    hwnd_ = viewHwnd_ = nullptr;
}

void SceneEditor::loadIcons() {
    // A missing or unreadable PNG leaves nullptr, and the button falls back
    // to a GDI-drawn icon.
    static constexpr std::array<const wchar_t*, ICON_COUNT> names{
        L"sphere", L"diamond", L"cube", L"pyramid", L"cylinder",
        L"dodecahedron", L"supertoroid", L"supershape",
        L"select", L"move", L"rotate", L"delete", L"delete_all",
    };
    const std::filesystem::path dir = assetDir(L"icons");
    for (std::size_t i = 0; i < names.size(); ++i) {
        const std::filesystem::path file = dir / (std::wstring(names[i]) + L".png");
        auto bmp = std::make_unique<Gdiplus::Bitmap>(file.c_str());
        if (bmp->GetLastStatus() == Gdiplus::Ok) icons_[i] = std::move(bmp);
    }
}

void SceneEditor::createToolbar(HINSTANCE hInst) {
    loadIcons();

    constexpr int size = 36, gap = 4, y = (static_cast<int>(TOOLBAR_H) - size) / 2;
    int x = 6;

    auto button = [&](int id) {
        HWND b = CreateWindowExW(0, L"BUTTON", L"", WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
            x, y, size, size, hwnd_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), hInst, nullptr);
        x += size + gap;
        return b;
    };
    auto separator = [&] {
        CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE | SS_ETCHEDVERT,
            x + 2, y + 4, 2, size - 8, hwnd_, nullptr, hInst, nullptr);
        x += 12;
    };

    for (int i = 0; i < ADDABLE_MESHES; ++i) button(ID_SHAPE_BASE + i);
    separator();
    toolButtons_ = { button(ID_TOOL_SELECT), button(ID_TOOL_MOVE), button(ID_TOOL_ROTATE) };
    separator();
    button(ID_DELETE);
    button(ID_DELETE_ALL);
}

void SceneEditor::onCommand(int id) {
    if (const auto shape = addableMesh(id - ID_SHAPE_BASE)) {
        addObject(*shape);
    } else {
        switch (id) {
        case ID_TOOL_SELECT: mode_ = EditMode::Select; break;
        case ID_TOOL_MOVE:   mode_ = EditMode::Move;   break;
        case ID_TOOL_ROTATE: mode_ = EditMode::Rotate; break;
        case ID_DELETE:      deleteSelected();         break;
        case ID_DELETE_ALL:  deleteAll();              break;
        default:             break;
        }
        for (HWND b : toolButtons_) InvalidateRect(b, nullptr, TRUE);
    }
    SetFocus(viewHwnd_);   // keep Del / Esc / D working after a button click
}

// =============================================================================
//  Owner-drawn toolbar buttons
// =============================================================================
int SceneEditor::iconIndex(int id) noexcept {
    if (addableMesh(id - ID_SHAPE_BASE)) return id - ID_SHAPE_BASE;
    switch (id) {
    case ID_TOOL_SELECT: return ADDABLE_MESHES + 0;
    case ID_TOOL_MOVE:   return ADDABLE_MESHES + 1;
    case ID_TOOL_ROTATE: return ADDABLE_MESHES + 2;
    case ID_DELETE:      return ADDABLE_MESHES + 3;
    case ID_DELETE_ALL:  return ADDABLE_MESHES + 4;
    default:             return -1;
    }
}

BOOL SceneEditor::onDrawItem(const DRAWITEMSTRUCT& d) {
    const int  id = static_cast<int>(d.CtlID);
    const RECT rc = d.rcItem;

    const bool activeTool =
        (id == ID_TOOL_SELECT && mode_ == EditMode::Select) ||
        (id == ID_TOOL_MOVE   && mode_ == EditMode::Move)   ||
        (id == ID_TOOL_ROTATE && mode_ == EditMode::Rotate);

    HBRUSH bg = CreateSolidBrush(activeTool ? RGB(196, 219, 249) : GetSysColor(COLOR_BTNFACE));
    FillRect(d.hDC, &rc, bg);
    DeleteObject(bg);

    RECT edge = rc;
    DrawEdge(d.hDC, &edge, ((d.itemState & ODS_SELECTED) || activeTool) ? EDGE_SUNKEN : EDGE_RAISED, BF_RECT);

    if (const int i = iconIndex(id); i >= 0 && icons_[i]) {
        // 32x32 PNG drawn 1:1 inside the 36 px button (2 px edge on each side).
        Gdiplus::Graphics g(d.hDC);
        g.DrawImage(icons_[i].get(), static_cast<INT>(rc.left + 2), static_cast<INT>(rc.top + 2), 32, 32);
    } else {
        drawFallbackIcon(d.hDC, rc, id);
    }
    return TRUE;
}

void SceneEditor::drawFallbackIcon(HDC dc, const RECT& rc, int id) const {
    const int cx = (rc.left + rc.right) / 2;
    const int cy = (rc.top + rc.bottom) / 2;

    const bool danger = id == ID_DELETE || id == ID_DELETE_ALL;
    HPEN    pen      = CreatePen(PS_SOLID, 2, danger ? RGB(178, 44, 44) : RGB(45, 45, 45));
    HGDIOBJ oldPen   = SelectObject(dc, pen);
    HGDIOBJ oldBrush = SelectObject(dc, GetStockObject(NULL_BRUSH));
    const int oldBk  = SetBkMode(dc, TRANSPARENT);

    auto poly = [&](std::span<const POINT> pts) { Polygon(dc, pts.data(), static_cast<int>(pts.size())); };
    auto line = [&](int x0, int y0, int x1, int y1) { MoveToEx(dc, x0, y0, nullptr); LineTo(dc, x1, y1); };
    auto at   = [&](double r, double a) {
        return POINT{ cx + std::lround(r * std::cos(a)), cy + std::lround(r * std::sin(a)) };
    };

    if (const auto shape = addableMesh(id - ID_SHAPE_BASE)) {
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
        default: break;
        }
    }

    SetBkMode(dc, oldBk);
    SelectObject(dc, oldBrush);
    SelectObject(dc, oldPen);
    DeleteObject(pen);
}

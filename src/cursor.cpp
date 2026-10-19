// Software mouse cursor for side-by-side and top-and-bottom output ([Overlay] cursor = 1).
//
// Windows draws the mouse cursor once, at the mouse position on the physical screen. In half SBS each
// eye's picture is squeezed into one half, so a game point at x shows at x/2 and W/2 + x/2 - and the
// cursor Windows draws at x sits in neither picture. Games that leave the cursor to Windows (LÖVE/SDL
// menus: Intravenous 2) had no cursor in 3D at all. Here the cursor the game asks for is drawn into
// each eye's area at the game's mouse position (screen depth, like the overlay), and Windows' own
// cursor is a blank one while it is over the game's client area.
//
// What the game asks for: its SetCursor calls (imports of the game's own modules are redirected, so
// changing the cursor never shows Windows' one in between) and, after each WM_SETCURSOR, the thread's
// cursor (DefWindowProc puts up the class cursor there). Both run on the game window's thread; the
// renderer only reads the result. The blank cursor being up (GetCursorInfo) means the mouse is over
// the game window, as any other window puts up its own; a game that hides its cursor (ShowCursor,
// SetCursor(NULL)) gets none drawn.
#include "common.h"

#include <psapi.h>
#include <algorithm>
#include <atomic>
#include <cstring>
#include <string>
#include <vector>

namespace {

struct CursorState {
    std::atomic<HWND> wnd{nullptr};         // game window the hooks serve
    HHOOK retHook = nullptr, msgHook = nullptr;
    UINT msg = 0;                           // posted to the window's thread: apply the hide state now
    HCURSOR blank = nullptr;
    std::atomic<HCURSOR> game{nullptr};     // the cursor the game asked for
    std::atomic<bool> hide{false};          // Windows shows the blank cursor over the client area
    bool patched = false;
    // renderer
    HCURSOR shape = nullptr;                // cursor in the textures
    bool shapeOk = false, invert = false, uploaded = false;
    int w = 0, h = 0, hx = 0, hy = 0;
    std::vector<uint8_t> rgba, inv;         // premultiplied colour; white where the cursor inverts
    GLuint tex = 0, invTex = 0;
    bool show = false;                      // this frame: drawn at (x, y), sw x sh eye pixels
    float x = 0, y = 0, sw = 0, sh = 0;
} g;

// Window coordinates in the game window's own DPI awareness (upscale mode: the game window stays
// DPI-unaware while the process default is aware), like its client size and the GL back buffer.
struct WindowDpiScope {
    using GetWndCtxFn = HANDLE(WINAPI*)(HWND);
    using SetThrCtxFn = HANDLE(WINAPI*)(HANDLE);
    static inline GetWndCtxFn getWnd =
        (GetWndCtxFn)GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetWindowDpiAwarenessContext");
    static inline SetThrCtxFn setThr =
        (SetThrCtxFn)GetProcAddress(GetModuleHandleW(L"user32.dll"), "SetThreadDpiAwarenessContext");
    HANDLE prev = nullptr;
    explicit WindowDpiScope(HWND wnd)
    {
        if (getWnd && setThr) prev = setThr(getWnd(wnd));
    }
    ~WindowDpiScope()
    {
        if (prev) setThr(prev);
    }
};

// The mouse position in wnd's client area (and the client size); true when it is inside.
bool MouseInClient(HWND wnd, POINT* at = nullptr, SIZE* size = nullptr)
{
    if (!wnd) return false;
    WindowDpiScope dpi(wnd);
    POINT p;
    RECT rc;
    if (!GetCursorPos(&p) || !ScreenToClient(wnd, &p) || !GetClientRect(wnd, &rc)) return false;
    if (at) *at = p;
    if (size) *size = SIZE{rc.right, rc.bottom};
    return p.x >= 0 && p.y >= 0 && p.x < rc.right && p.y < rc.bottom;
}

// On the game window's thread. GetCursor() is the thread's cursor: the game's, or the blank one.
void Apply(bool inClient)
{
    const HCURSOR cur = GetCursor();
    if (inClient && cur != g.blank) g.game = cur;
    if (!g.hide) {
        if (cur == g.blank) SetCursor(g.game);   // (our own import: the real SetCursor)
    } else if (inClient && cur && cur != g.blank) {
        SetCursor(g.blank);
    }
}

// After a window procedure returned: WM_SETCURSOR in the client area has the game's cursor up.
LRESULT CALLBACK RetHook(int code, WPARAM wp, LPARAM lp)
{
    const CWPRETSTRUCT* m = (const CWPRETSTRUCT*)lp;
    if (code == HC_ACTION && m && m->message == WM_SETCURSOR && LOWORD(m->lParam) == HTCLIENT &&
        (HWND)m->wParam == m->hwnd) {   // the window under the mouse (parents see it first)
        const HWND wnd = g.wnd;
        if (m->hwnd == wnd || IsChild(wnd, m->hwnd)) Apply(true);
    }
    return CallNextHookEx(nullptr, code, wp, lp);
}

// Our posted message: the hide state changed, apply it without waiting for the mouse to move.
LRESULT CALLBACK MsgHook(int code, WPARAM remove, LPARAM lp)
{
    MSG* m = (MSG*)lp;
    if (code == HC_ACTION && m && g.msg && m->message == g.msg) {
        if (remove == PM_REMOVE) Apply(MouseInClient(g.wnd));
        m->message = WM_NULL;
    }
    return CallNextHookEx(nullptr, code, remove, lp);
}

// The game's own SetCursor calls (exe and DLLs in its folder: SDL, WineD3D's hardware cursor).
HCURSOR WINAPI HookSetCursor(HCURSOR h)
{
    const HCURSOR prev = g.game.exchange(h);
    const HCURSOR old = SetCursor(g.hide && h && MouseInClient(g.wnd) ? g.blank : h);
    return old == g.blank ? prev : old;
}

void PatchGameModules()
{
    wchar_t gameDir[MAX_PATH];
    GetModuleFileNameW(nullptr, gameDir, MAX_PATH);
    if (wchar_t* s = wcsrchr(gameDir, L'\\')) s[1] = 0;
    const size_t dirLen = wcslen(gameDir);
    HMODULE self = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       (LPCWSTR)&PatchGameModules, &self);
    std::string patchedIn;
    HMODULE mods[512];
    DWORD bytes = 0;
    if (K32EnumProcessModules(GetCurrentProcess(), mods, sizeof(mods), &bytes)) {
        for (DWORD i = 0; i < bytes / sizeof(HMODULE) && i < 512; ++i) {
            if (mods[i] == self) continue;
            wchar_t path[MAX_PATH];
            GetModuleFileNameW(mods[i], path, MAX_PATH);
            if (_wcsnicmp(path, gameDir, dirLen) != 0) continue;
            if (!PatchModuleImport(mods[i], "user32.dll", "SetCursor", (void*)&HookSetCursor)) continue;
            char name[MAX_PATH];
            WideCharToMultiByte(CP_UTF8, 0, path + dirLen, -1, name, MAX_PATH, nullptr, nullptr);
            patchedIn += std::string(patchedIn.empty() ? "" : ", ") + name;
        }
    }
    Log("cursor: SetCursor imports redirected in %s", patchedIn.empty() ? "nothing" : patchedIn.c_str());
}

// Hooks the thread that owns the game window (again when the game makes a new window).
void Install(HWND wnd)
{
    if (!g.blank) {
        BYTE andMask[32 * 32 / 8], xorMask[32 * 32 / 8];
        memset(andMask, 0xFF, sizeof(andMask));   // keep the screen ...
        memset(xorMask, 0, sizeof(xorMask));      // ... unchanged
        HMODULE self = nullptr;
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCWSTR)&Install, &self);
        g.blank = CreateCursor(self, 0, 0, 32, 32, andMask, xorMask);
        g.msg = RegisterWindowMessageW(L"OGL3D_Cursor");
    }
    if (g.retHook) UnhookWindowsHookEx(g.retHook);
    if (g.msgHook) UnhookWindowsHookEx(g.msgHook);
    const DWORD tid = GetWindowThreadProcessId(wnd, nullptr);
    g.wnd = wnd;
    g.retHook = SetWindowsHookExW(WH_CALLWNDPROCRET, RetHook, nullptr, tid);
    g.msgHook = SetWindowsHookExW(WH_GETMESSAGE, MsgHook, nullptr, tid);
    if (!g.patched) {
        PatchGameModules();
        g.patched = true;
    }
    Log("cursor: hooks on window thread %lu%s", tid, g.retHook && g.msgHook && g.blank ? "" : " - failed");
}

// The cursor's pixels, drawn by Windows on black and on white: that gives coverage and colour (alpha
// cursors) and the pixels that invert what is below (monochrome cursors, e.g. the classic I-beam).
bool Rasterize(HCURSOR cur)
{
    ICONINFO ii{};
    if (!GetIconInfo(cur, &ii)) return false;
    BITMAP bm{};
    const bool color = ii.hbmColor != nullptr;
    GetObjectW(color ? ii.hbmColor : ii.hbmMask, sizeof(bm), &bm);
    if (ii.hbmColor) DeleteObject(ii.hbmColor);
    if (ii.hbmMask) DeleteObject(ii.hbmMask);
    const int w = bm.bmWidth, h = color ? bm.bmHeight : bm.bmHeight / 2;   // monochrome: AND mask over XOR mask
    if (w <= 0 || h <= 0 || w > 512 || h > 512) return false;
    HDC dc = CreateCompatibleDC(nullptr);
    if (!dc) return false;
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;   // top-down
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    std::vector<uint8_t> on[2];   // BGRA on black, on white
    bool ok = true;
    for (int k = 0; k < 2 && ok; ++k) {
        void* bits = nullptr;
        HBITMAP bmp = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
        ok = bmp && bits;
        if (!ok) break;
        HGDIOBJ old = SelectObject(dc, bmp);
        memset(bits, k ? 0xFF : 0x00, (size_t)w * h * 4);
        ok = DrawIconEx(dc, 0, 0, cur, w, h, 0, nullptr, DI_NORMAL) != 0;
        GdiFlush();
        on[k].assign((const uint8_t*)bits, (const uint8_t*)bits + (size_t)w * h * 4);
        SelectObject(dc, old);
        DeleteObject(bmp);
    }
    DeleteDC(dc);
    if (!ok) return false;
    g.rgba.assign((size_t)w * h * 4, 0);
    g.inv.assign((size_t)w * h * 4, 0);
    g.invert = false;
    for (size_t i = 0; i < (size_t)w * h; ++i) {
        const uint8_t* b = &on[0][i * 4];
        const uint8_t* wh = &on[1][i * 4];
        // on white minus on black = 3 * 255 * (1 - alpha); negative: black became white = inverted
        const int d = (wh[0] - b[0]) + (wh[1] - b[1]) + (wh[2] - b[2]);
        if (d < 0) {
            memset(&g.inv[i * 4], 255, 4);
            g.invert = true;
            continue;
        }
        const int a = std::clamp(255 - d / 3, 0, 255);
        uint8_t* o = &g.rgba[i * 4];
        o[0] = (uint8_t)std::min<int>(b[2], a);   // on black = colour * alpha (premultiplied)
        o[1] = (uint8_t)std::min<int>(b[1], a);
        o[2] = (uint8_t)std::min<int>(b[0], a);
        o[3] = (uint8_t)a;
    }
    g.w = w;
    g.h = h;
    g.hx = (int)ii.xHotspot;
    g.hy = (int)ii.yHotspot;
    return true;
}

}   // namespace

// Render thread, before the frame is composed: is the cursor drawn this frame, where and how big
// (eye image W x H; the client area maps onto it).
void CursorPrepare(HWND wnd, int W, int H)
{
    g.show = false;
    if (!g_cfg.cursor || !wnd || wnd != g.wnd || !g.blank || !g.hide) return;
    CURSORINFO ci{sizeof(ci)};
    if (!GetCursorInfo(&ci) || !(ci.flags & CURSOR_SHOWING) || ci.hCursor != g.blank) return;
    const HCURSOR want = g.game;
    if (!want) return;
    if (want != g.shape) {
        g.shape = want;
        g.shapeOk = Rasterize(want);
        g.uploaded = false;
        static int logged;
        if (logged++ < 20) {
            if (g.shapeOk) Log("cursor: shape %p %dx%d, hotspot %d,%d%s", want, g.w, g.h, g.hx, g.hy, g.invert ? ", inverting" : "");
            else Log("cursor: shape %p could not be read", want);
        }
    }
    POINT p;
    SIZE s;
    if (!g.shapeOk || !MouseInClient(wnd, &p, &s) || s.cx <= 0 || s.cy <= 0) return;
    const float kx = (float)W / s.cx, ky = (float)H / s.cy;
    g.x = (p.x - g.hx) * kx;
    g.y = (p.y - g.hy) * ky;
    g.sw = g.w * kx;
    g.sh = g.h * ky;
    g.show = true;
}

bool CursorActive()
{
    return g.show;
}

// Draws the cursor into viewport (vx, vy, vw, vh) of fbo, laid out for an eye image of W x H (like the
// overlay). The caller has saved GL state.
void CursorRender(GLuint fbo, int vx, int vy, int vw, int vh, int W, int H, bool compat)
{
    if (!g.show) return;
    if (!g.uploaded) {
        OverlayUpload(g.tex, g.w, g.h, g.rgba.data());
        if (g.invert) OverlayUpload(g.invTex, g.w, g.h, g.inv.data());
        g.uploaded = true;
    }
    const float x0 = -1.0f + 2.0f * g.x / W, x1 = -1.0f + 2.0f * (g.x + g.sw) / W;
    const float y1 = 1.0f - 2.0f * g.y / H, y0 = 1.0f - 2.0f * (g.y + g.sh) / H;
    OverlayDraw(fbo, vx, vy, vw, vh, compat, g.tex, x0, y0, x1, y1, kBlendPremultiplied);
    if (g.invert) OverlayDraw(fbo, vx, vy, vw, vh, compat, g.invTex, x0, y0, x1, y1, kBlendInvert);
}

// Render thread, after composing: hide = this output shows both eyes at once with the cursor drawn
// in (else Windows' cursor is right where the game expects it: weaving, one eye only).
void CursorHide(HWND wnd, bool hide)
{
    hide = hide && g_cfg.cursor && wnd;
    if (hide && wnd != g.wnd) Install(wnd);
    if (!g.wnd || hide == g.hide) return;
    g.hide = hide;
    PostMessageW(g.wnd, g.msg, 0, 0);
    Log("cursor: %s", hide ? "drawn into each eye (Windows' cursor hidden over the game)" : "left to Windows");
}

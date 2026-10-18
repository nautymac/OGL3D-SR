// Upscale mode ([Display] upscale = 1): the game stays DPI-unaware and renders at its own (lower)
// resolution, so its UI keeps a readable size, while both eyes are woven at the panel's physical
// resolution in a window of our own.
//
// Why a second window: Windows stretches a DPI-unaware game's window to the scaled display, and
// that stretch destroys the pixel-exact SR weave pattern. Our window is created per-monitor DPI
// aware (thread awareness), sits exactly over the game window (owned by it, so it stays above it),
// never activates, and hit-tests as transparent: keyboard and mouse still reach the game window.
//
// Why Direct3D 11 for it: presenting a second, DPI-aware OpenGL window from a DPI-unaware process
// crashed inside NVIDIA's DXGI-based GL present path. A D3D11 swap chain has an explicit size; the
// eyes go from GL to D3D11 through WGL_NV_DX_interop2 and the SR SDK's DX11 weaver weaves there.
#include "common.h"

#include <d3d11.h>
#include <dxgi1_2.h>
#include <psapi.h>
#include <algorithm>
#include <iterator>
#include <string>
#include <vector>

// srout.cpp
bool SrAvailableDx11(HWND hwnd, ID3D11DeviceContext* ctx);
bool SrWeaveDx11(ID3D11ShaderResourceView* srv, int eyeW, int eyeH, DXGI_FORMAT fmt);
void SrPreloadRuntime();

namespace {

template <class T> void SafeRelease(T*& p)
{
    if (p) {
        p->Release();
        p = nullptr;
    }
}

using SetThreadCtxFn = HANDLE(WINAPI*)(HANDLE);
SetThreadCtxFn SetThreadCtx()
{
    static SetThreadCtxFn fn = (SetThreadCtxFn)GetProcAddress(GetModuleHandleW(L"user32.dll"), "SetThreadDpiAwarenessContext");
    return fn;
}

// Runs a block with this thread per-monitor DPI aware: window coordinates are physical pixels.
struct PhysicalDpiScope {
    HANDLE prev = nullptr;
    PhysicalDpiScope()
    {
        if (SetThreadCtx()) prev = SetThreadCtx()((HANDLE)-4 /* PER_MONITOR_AWARE_V2 */);
    }
    ~PhysicalDpiScope()
    {
        if (prev && SetThreadCtx()) SetThreadCtx()(prev);
    }
};

// WGL_NV_DX_interop2
using DXOpenDeviceFn = HANDLE(WINAPI*)(void*);
using DXRegisterObjectFn = HANDLE(WINAPI*)(HANDLE, void*, GLuint, GLenum, GLenum);
using DXUnregisterObjectFn = BOOL(WINAPI*)(HANDLE, HANDLE);
using DXLockObjectsFn = BOOL(WINAPI*)(HANDLE, GLint, HANDLE*);
constexpr GLenum WGL_ACCESS_WRITE_DISCARD_NV = 0x0002;

struct InteropTex {
    ID3D11Texture2D* tex = nullptr;
    ID3D11ShaderResourceView* srv = nullptr;
    GLuint gl = 0, fbo = 0;
    HANDLE handle = nullptr;
    int w = 0, h = 0;
};

struct PresentState {
    HWND game = nullptr, wnd = nullptr;
    int x = 0, y = 0, w = 0, h = 0;   // physical client rect of the game window
    ID3D11Device* dev = nullptr;
    ID3D11DeviceContext* ctx = nullptr;
    IDXGISwapChain1* sc = nullptr;
    ID3D11Texture2D* back = nullptr;
    ID3D11RenderTargetView* rtv = nullptr;
    HANDLE interop = nullptr;
    InteropTex in;    // both eyes side by side at the game's resolution (SR weaver input)
    InteropTex out;   // composed layout at the window's resolution (SBS / TAB / fallback)
    DXOpenDeviceFn open = nullptr;
    DXRegisterObjectFn reg = nullptr;
    DXUnregisterObjectFn unreg = nullptr;
    DXLockObjectsFn lock = nullptr, unlock = nullptr;
    bool failed = false;
    bool shown = false;
    bool owned = false;   // owned by the game window (else a topmost window)
    bool foreign = false; // the window belongs to the game window's thread, not the one presenting
    bool requested = false;
    volatile LONG made = 0;   // window creation: 0 pending, 1 done, -1 failed
    HHOOK makeHook = nullptr;
    UINT makeMsg = 0;
    unsigned waited = 0;  // frames presented in the game's window while the window is being made
    unsigned frames = 0;
} g;

LRESULT CALLBACK WndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_NCHITTEST: return HTTRANSPARENT;   // to the game window below (same thread)
    case WM_MOUSEACTIVATE: return MA_NOACTIVATE;
    case WM_ERASEBKGND: return 1;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

// The game window's client area in physical pixels, clipped to its monitor.
bool GameClientRect(HWND game, RECT& r)
{
    PhysicalDpiScope dpi;
    RECT c{};
    if (!GetClientRect(game, &c)) return false;
    POINT tl{c.left, c.top}, br{c.right, c.bottom};
    ClientToScreen(game, &tl);
    ClientToScreen(game, &br);
    r = RECT{tl.x, tl.y, br.x, br.y};
    MONITORINFO mi{sizeof(mi)};
    if (GetMonitorInfoW(MonitorFromWindow(game, MONITOR_DEFAULTTONEAREST), &mi)) {
        RECT clipped;
        if (IntersectRect(&clipped, &r, &mi.rcMonitor)) r = clipped;
    }
    return r.right > r.left && r.bottom > r.top;
}

bool CreateBackBufferView()
{
    if (FAILED(g.sc->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&g.back))) return false;
    return SUCCEEDED(g.dev->CreateRenderTargetView(g.back, nullptr, &g.rtv));
}

// D3D11 device on the NVIDIA adapter (interop needs the GPU that runs the game's GL context).
bool CreateDevice()
{
    HMODULE d3d = LoadLibraryW(L"d3d11.dll");
    HMODULE dxgi = LoadLibraryW(L"dxgi.dll");
    auto createDevice = d3d ? (PFN_D3D11_CREATE_DEVICE)GetProcAddress(d3d, "D3D11CreateDevice") : nullptr;
    using CreateFactory1Fn = HRESULT(WINAPI*)(REFIID, void**);
    auto createFactory = dxgi ? (CreateFactory1Fn)GetProcAddress(dxgi, "CreateDXGIFactory1") : nullptr;
    if (!createDevice || !createFactory) return false;
    IDXGIFactory1* f = nullptr;
    IDXGIAdapter1* chosen = nullptr;
    if (SUCCEEDED(createFactory(__uuidof(IDXGIFactory1), (void**)&f))) {
        IDXGIAdapter1* a = nullptr;
        for (UINT i = 0; f->EnumAdapters1(i, &a) != DXGI_ERROR_NOT_FOUND; ++i) {
            DXGI_ADAPTER_DESC1 d{};
            a->GetDesc1(&d);
            if (!chosen && d.VendorId == 0x10DE) chosen = a;   // NVIDIA
            else a->Release();
        }
        f->Release();
    }
    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
    HRESULT hr = createDevice(chosen, chosen ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_HARDWARE, nullptr,
                              D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels, 2, D3D11_SDK_VERSION, &g.dev, nullptr, &g.ctx);
    if (chosen) chosen->Release();
    if (FAILED(hr)) {
        Log("upscale: D3D11CreateDevice failed (0x%08lx)", hr);
        return false;
    }
    return true;
}

bool CreateSwapChain()
{
    IDXGIDevice* dd = nullptr;
    IDXGIAdapter* ad = nullptr;
    IDXGIFactory2* f = nullptr;
    bool ok = SUCCEEDED(g.dev->QueryInterface(__uuidof(IDXGIDevice), (void**)&dd)) && SUCCEEDED(dd->GetAdapter(&ad)) &&
              SUCCEEDED(ad->GetParent(__uuidof(IDXGIFactory2), (void**)&f));
    if (ok) {
        DXGI_SWAP_CHAIN_DESC1 d{};
        d.Width = (UINT)g.w;
        d.Height = (UINT)g.h;
        d.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        d.SampleDesc.Count = 1;
        d.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        d.BufferCount = 2;
        d.Scaling = DXGI_SCALING_STRETCH;
        d.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        d.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
        HRESULT hr = f->CreateSwapChainForHwnd(g.dev, g.wnd, &d, nullptr, nullptr, &g.sc);
        if (FAILED(hr)) Log("upscale: CreateSwapChainForHwnd failed (0x%08lx)", hr);
        ok = SUCCEEDED(hr);
        if (ok) f->MakeWindowAssociation(g.wnd, DXGI_MWA_NO_ALT_ENTER | DXGI_MWA_NO_WINDOW_CHANGES);
    }
    SafeRelease(f);
    SafeRelease(ad);
    SafeRelease(dd);
    return ok && CreateBackBufferView();
}

// The presentation window, made on the calling thread.
void MakeWindow()
{
    {
        PhysicalDpiScope dpi;
        WNDCLASSEXW wc{sizeof(wc)};
        wc.lpfnWndProc = WndProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = L"OGL3D_Present";
        RegisterClassExW(&wc);
        using GetWndCtxFn = HANDLE(WINAPI*)(HWND);
        using AwarenessFn = int(WINAPI*)(HANDLE);
        HMODULE u32 = GetModuleHandleW(L"user32.dll");
        auto getWndCtx = (GetWndCtxFn)GetProcAddress(u32, "GetWindowDpiAwarenessContext");
        auto awareness = (AwarenessFn)GetProcAddress(u32, "GetAwarenessFromDpiAwarenessContext");
        // Owned by the game window it stays right above it in the z-order (and below any other app
        // the user switches to) without being topmost. Windows may give an owned window its owner's
        // DPI awareness; then it is recreated without owner as a topmost window.
        for (HWND owner : {g.game, (HWND) nullptr}) {
            g.wnd = CreateWindowExW((owner ? 0 : WS_EX_TOPMOST) | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, wc.lpszClassName,
                                    L"OGL3D", WS_POPUP, g.x, g.y, g.w, g.h, owner, nullptr, wc.hInstance, nullptr);
            if (!g.wnd) break;
            RECT wr{};
            GetWindowRect(g.wnd, &wr);
            int aw = getWndCtx && awareness ? awareness(getWndCtx(g.wnd)) : -1;
            Log("upscale: window %s owner: %d,%d-%d,%d physical, DPI awareness %d (2 = per monitor)", owner ? "with" : "without",
                wr.left, wr.top, wr.right, wr.bottom, aw);
            g.owned = owner != nullptr;
            if (aw == 2 || aw == -1) break;
            DestroyWindow(g.wnd);
            g.wnd = nullptr;
        }
    }
    InterlockedExchange(&g.made, g.wnd ? 1 : -1);
}

// Runs on the game window's thread when it takes our message from its queue.
LRESULT CALLBACK MakeWindowHook(int code, WPARAM remove, LPARAM lp)
{
    MSG* m = (MSG*)lp;
    if (code == HC_ACTION && m && g.makeMsg && m->message == g.makeMsg) {
        if (g.made == 0) MakeWindow();
        m->message = WM_NULL;
    }
    return CallNextHookEx(nullptr, code, remove, lp);
}

// The presentation window belongs to the game window's thread: HTTRANSPARENT passes the mouse only to
// windows of the same thread, and that thread's message loop serves it. A game that renders on another
// thread (WineD3D's command stream thread) has the window made on the game window's thread through a
// message hook, without waiting for it here (that thread may be waiting for this one); until then the
// frames go to the game's window. Returns 1 ready, 0 not yet, -1 failed.
int OpenPresenter(HWND game)
{
    if (!g.requested) {
        RECT r;
        if (!GameClientRect(game, r)) return -1;
        g.requested = true;
        g.game = game;
        g.x = r.left;
        g.y = r.top;
        g.w = r.right - r.left;
        g.h = r.bottom - r.top;
        const DWORD tid = GetWindowThreadProcessId(game, nullptr);
        if (tid == GetCurrentThreadId()) {
            MakeWindow();
        } else {
            g.foreign = true;
            g.makeMsg = RegisterWindowMessageW(L"OGL3D_MakePresentWindow");
            g.makeHook = SetWindowsHookExW(WH_GETMESSAGE, MakeWindowHook, nullptr, tid);
            if (!g.makeHook || !PostMessageW(game, g.makeMsg, 0, 0)) {
                Log("upscale: could not reach the game window's thread %lu (%lu)", tid, GetLastError());
                return -1;
            }
            Log("upscale: the game renders on another thread than its window's (%lu): window made there", tid);
        }
    }
    if (g.made == 0) {
        if (++g.waited == 600) Log("upscale: the game window's thread has not taken the request yet");
        return g.waited > 3600 ? -1 : 0;
    }
    if (g.makeHook) {
        UnhookWindowsHookEx(g.makeHook);
        g.makeHook = nullptr;
    }
    if (g.made < 0 || !g.wnd) {
        Log("upscale: could not create a per-monitor DPI aware window (%lu)", GetLastError());
        return -1;
    }
    if (!CreateDevice() || !CreateSwapChain()) return -1;
    g.open = (DXOpenDeviceFn)R.wglGetProcAddress("wglDXOpenDeviceNV");
    g.reg = (DXRegisterObjectFn)R.wglGetProcAddress("wglDXRegisterObjectNV");
    g.unreg = (DXUnregisterObjectFn)R.wglGetProcAddress("wglDXUnregisterObjectNV");
    g.lock = (DXLockObjectsFn)R.wglGetProcAddress("wglDXLockObjectsNV");
    g.unlock = (DXLockObjectsFn)R.wglGetProcAddress("wglDXUnlockObjectsNV");
    if (!g.open || !g.reg || !g.unreg || !g.lock || !g.unlock || !(g.interop = g.open(g.dev))) {
        Log("upscale: WGL_NV_DX_interop2 unavailable");
        return -1;
    }
    Log("upscale: D3D11 presentation window %dx%d at %d,%d (game renders at its own resolution)", g.w, g.h, g.x, g.y);
    return 1;
}

void ReleaseInterop(InteropTex& t)
{
    if (t.handle) g.unreg(g.interop, t.handle);
    if (t.gl) R.DeleteTextures(1, &t.gl);
    if (t.fbo) R.DeleteFramebuffers(1, &t.fbo);
    SafeRelease(t.srv);
    SafeRelease(t.tex);
    t = InteropTex{};
}

// A D3D11 texture that GL can render into (game context; GL framebuffer bindings are changed).
bool EnsureInterop(InteropTex& t, int w, int h, bool srv)
{
    if (t.tex && t.w == w && t.h == h) return true;
    ReleaseInterop(t);
    D3D11_TEXTURE2D_DESC d{};
    d.Width = (UINT)w;
    d.Height = (UINT)h;
    d.MipLevels = 1;
    d.ArraySize = 1;
    d.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    d.SampleDesc.Count = 1;
    d.Usage = D3D11_USAGE_DEFAULT;
    d.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    if (FAILED(g.dev->CreateTexture2D(&d, nullptr, &t.tex))) return false;
    if (srv && FAILED(g.dev->CreateShaderResourceView(t.tex, nullptr, &t.srv))) return false;
    R.GenTextures(1, &t.gl);
    t.handle = g.reg(g.interop, t.tex, t.gl, GL_TEXTURE_2D, WGL_ACCESS_WRITE_DISCARD_NV);
    if (!t.handle) {
        Log("upscale: wglDXRegisterObjectNV failed (%lu)", GetLastError());
        return false;
    }
    R.GenFramebuffers(1, &t.fbo);
    R.BindFramebuffer(GL_DRAW_FRAMEBUFFER, t.fbo);
    R.FramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, t.gl, 0);
    t.w = w;
    t.h = h;
    return true;
}

bool GameInForeground()
{
    HWND fg = GetForegroundWindow();
    return fg && (fg == g.game || GetAncestor(fg, GA_ROOTOWNER) == GetAncestor(g.game, GA_ROOTOWNER));
}

// Owned: shown whenever the game window is (the z-order keeps it right above the game, below other
// apps), so a game that starts without focus is still woven. Topmost fallback: only while the
// game is in the foreground, or it would cover other apps.
bool ShouldShow()
{
    if (!IsWindowVisible(g.game) || IsIconic(g.game)) return false;
    return g.owned || GameInForeground();
}

void Show(bool on)
{
    if (on == g.shown) return;
    if (g.foreign) ShowWindowAsync(g.wnd, on ? SW_SHOWNOACTIVATE : SW_HIDE);   // its thread may be waiting for this one
    else ShowWindow(g.wnd, on ? SW_SHOWNOACTIVATE : SW_HIDE);
    g.shown = on;
    Log("upscale: presentation window %s", on ? "shown" : "hidden (game minimized or in the background)");
}

void FollowGameWindow()
{
    RECT r;
    if (!GameClientRect(g.game, r)) return;
    const int w = r.right - r.left, h = r.bottom - r.top;
    if (r.left == g.x && r.top == g.y && w == g.w && h == g.h) return;
    {
        PhysicalDpiScope dpi;
        SetWindowPos(g.wnd, g.owned ? nullptr : HWND_TOPMOST, r.left, r.top, w, h,
                     SWP_NOACTIVATE | (g.owned ? SWP_NOZORDER : 0) | (g.foreign ? SWP_ASYNCWINDOWPOS : 0));
    }
    if (w != g.w || h != g.h) {
        SafeRelease(g.rtv);
        SafeRelease(g.back);
        g.ctx->ClearState();
        g.sc->ResizeBuffers(0, (UINT)w, (UINT)h, DXGI_FORMAT_UNKNOWN, 0);
        CreateBackBufferView();
    }
    g.x = r.left;
    g.y = r.top;
    g.w = w;
    g.h = h;
    Log("upscale: presentation window moved to %dx%d at %d,%d", g.w, g.h, g.x, g.y);
}

// ---- display mode: keep the panel at its native mode ------------------------------------------

// The game's own modules (exe and the DLLs in its folder, e.g. SDL) get their user32 imports of the
// display mode functions redirected (import table patch, not an inline hook: user32's internal calls
// between these functions and the GPU driver's own calls stay untouched). A blocked mode change is
// reported back to them as the current mode, so the game sizes its fullscreen window to it:
// 1920x1080 in a DPI-unaware game on a 200% 4K panel covers it exactly.
using CdsExWFn = LONG(WINAPI*)(LPCWSTR, DEVMODEW*, HWND, DWORD, LPVOID);
using CdsExAFn = LONG(WINAPI*)(LPCSTR, DEVMODEA*, HWND, DWORD, LPVOID);
using EdsExWFn = BOOL(WINAPI*)(LPCWSTR, DWORD, DEVMODEW*, DWORD);
using EdsExAFn = BOOL(WINAPI*)(LPCSTR, DWORD, DEVMODEA*, DWORD);
CdsExWFn g_cdsExW = ChangeDisplaySettingsExW;
CdsExAFn g_cdsExA = ChangeDisplaySettingsExA;
EdsExWFn g_edsExW = EnumDisplaySettingsExW;
EdsExAFn g_edsExA = EnumDisplaySettingsExA;
struct FakeMode {
    bool active = false;
    DWORD w = 0, h = 0, bpp = 0, hz = 0;
} g_fake;

template <class DM>
void RememberMode(const DM* mode)
{
    if (!mode) {   // restore to the registry mode: nothing was ever changed
        g_fake.active = false;
        return;
    }
    if (mode->dmFields & DM_PELSWIDTH) g_fake.w = mode->dmPelsWidth;
    if (mode->dmFields & DM_PELSHEIGHT) g_fake.h = mode->dmPelsHeight;
    if (mode->dmFields & DM_BITSPERPEL) g_fake.bpp = mode->dmBitsPerPel;
    if (mode->dmFields & DM_DISPLAYFREQUENCY) g_fake.hz = mode->dmDisplayFrequency;
    g_fake.active = g_fake.w && g_fake.h;
    Log("upscale: blocked display mode change to %lux%lu (game renders at that size, output stays native)",
        g_fake.w, g_fake.h);
}

template <class DM>
void ReportMode(DWORD modeNum, DM* mode)
{
    if (modeNum != ENUM_CURRENT_SETTINGS || !mode || !g_fake.active || !g_cfg.upscaleReportMode) return;
    mode->dmPelsWidth = g_fake.w;
    mode->dmPelsHeight = g_fake.h;
    if (g_fake.bpp) mode->dmBitsPerPel = g_fake.bpp;
    if (g_fake.hz) mode->dmDisplayFrequency = g_fake.hz;
}

LONG WINAPI HookCdsExW(LPCWSTR dev, DEVMODEW* mode, HWND wnd, DWORD flags, LPVOID param)
{
    RememberMode(mode);
    return mode ? DISP_CHANGE_SUCCESSFUL : g_cdsExW(dev, mode, wnd, flags, param);
}

LONG WINAPI HookCdsExA(LPCSTR dev, DEVMODEA* mode, HWND wnd, DWORD flags, LPVOID param)
{
    RememberMode(mode);
    return mode ? DISP_CHANGE_SUCCESSFUL : g_cdsExA(dev, mode, wnd, flags, param);
}

LONG WINAPI HookCdsW(DEVMODEW* mode, DWORD flags) { return HookCdsExW(nullptr, mode, nullptr, flags, nullptr); }
LONG WINAPI HookCdsA(DEVMODEA* mode, DWORD flags) { return HookCdsExA(nullptr, mode, nullptr, flags, nullptr); }

BOOL WINAPI HookEdsExW(LPCWSTR dev, DWORD modeNum, DEVMODEW* mode, DWORD flags)
{
    BOOL ok = g_edsExW(dev, modeNum, mode, flags);
    if (ok) ReportMode(modeNum, mode);
    return ok;
}

BOOL WINAPI HookEdsExA(LPCSTR dev, DWORD modeNum, DEVMODEA* mode, DWORD flags)
{
    BOOL ok = g_edsExA(dev, modeNum, mode, flags);
    if (ok) ReportMode(modeNum, mode);
    return ok;
}

BOOL WINAPI HookEdsW(LPCWSTR dev, DWORD modeNum, DEVMODEW* mode) { return HookEdsExW(dev, modeNum, mode, 0); }
BOOL WINAPI HookEdsA(LPCSTR dev, DWORD modeNum, DEVMODEA* mode) { return HookEdsExA(dev, modeNum, mode, 0); }

struct IatHook {
    const char* name;
    void* hook;
};

// ---- SR runtime in a DPI-unaware process -------------------------------------------------------
// The SR DLLs read window and monitor geometry (also from a subclassed window procedure, i.e. on
// the game's DPI-unaware thread) and would get scaled sizes: the weave then only covers part of
// the window. Their imports of these functions run with the thread per-monitor aware instead.
BOOL WINAPI Phys_GetClientRect(HWND h, LPRECT r) { PhysicalDpiScope s; return GetClientRect(h, r); }
BOOL WINAPI Phys_GetWindowRect(HWND h, LPRECT r) { PhysicalDpiScope s; return GetWindowRect(h, r); }
BOOL WINAPI Phys_ClientToScreen(HWND h, LPPOINT p) { PhysicalDpiScope s; return ClientToScreen(h, p); }
BOOL WINAPI Phys_ScreenToClient(HWND h, LPPOINT p) { PhysicalDpiScope s; return ScreenToClient(h, p); }
HMONITOR WINAPI Phys_MonitorFromWindow(HWND h, DWORD f) { PhysicalDpiScope s; return MonitorFromWindow(h, f); }
BOOL WINAPI Phys_GetMonitorInfoA(HMONITOR m, LPMONITORINFO i) { PhysicalDpiScope s; return GetMonitorInfoA(m, i); }
BOOL WINAPI Phys_GetMonitorInfoW(HMONITOR m, LPMONITORINFO i) { PhysicalDpiScope s; return GetMonitorInfoW(m, i); }
BOOL WINAPI Phys_EnumDisplayMonitors(HDC dc, LPCRECT r, MONITORENUMPROC p, LPARAM l)
{
    PhysicalDpiScope s;
    return EnumDisplayMonitors(dc, r, p, l);
}
BOOL WINAPI Phys_EnumDisplaySettingsA(LPCSTR d, DWORD n, DEVMODEA* m) { PhysicalDpiScope s; return EnumDisplaySettingsA(d, n, m); }
BOOL WINAPI Phys_EnumDisplaySettingsW(LPCWSTR d, DWORD n, DEVMODEW* m) { PhysicalDpiScope s; return EnumDisplaySettingsW(d, n, m); }
int WINAPI Phys_GetSystemMetrics(int i) { PhysicalDpiScope s; return GetSystemMetrics(i); }

const IatHook kSrHooks[] = {
    {"GetClientRect", (void*)Phys_GetClientRect},
    {"GetWindowRect", (void*)Phys_GetWindowRect},
    {"ClientToScreen", (void*)Phys_ClientToScreen},
    {"ScreenToClient", (void*)Phys_ScreenToClient},
    {"MonitorFromWindow", (void*)Phys_MonitorFromWindow},
    {"GetMonitorInfoA", (void*)Phys_GetMonitorInfoA},
    {"GetMonitorInfoW", (void*)Phys_GetMonitorInfoW},
    {"EnumDisplayMonitors", (void*)Phys_EnumDisplayMonitors},
    {"EnumDisplaySettingsA", (void*)Phys_EnumDisplaySettingsA},
    {"EnumDisplaySettingsW", (void*)Phys_EnumDisplaySettingsW},
    {"GetSystemMetrics", (void*)Phys_GetSystemMetrics},
};

const IatHook kIatHooks[] = {
    {"ChangeDisplaySettingsExW", (void*)HookCdsExW}, {"ChangeDisplaySettingsExA", (void*)HookCdsExA},
    {"ChangeDisplaySettingsW", (void*)HookCdsW},     {"ChangeDisplaySettingsA", (void*)HookCdsA},
    {"EnumDisplaySettingsExW", (void*)HookEdsExW},   {"EnumDisplaySettingsExA", (void*)HookEdsExA},
    {"EnumDisplaySettingsW", (void*)HookEdsW},       {"EnumDisplaySettingsA", (void*)HookEdsA},
};

// Redirects module m's user32 imports listed in hooks; returns the number patched.
int PatchImports(HMODULE m, const IatHook* hooks, size_t hookCount)
{
    auto base = (uint8_t*)m;
    auto dos = (IMAGE_DOS_HEADER*)base;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return 0;
    auto nt = (IMAGE_NT_HEADERS*)(base + dos->e_lfanew);
    const IMAGE_DATA_DIRECTORY& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dir.VirtualAddress) return 0;
    int patched = 0;
    for (auto imp = (IMAGE_IMPORT_DESCRIPTOR*)(base + dir.VirtualAddress); imp->Name; ++imp) {
        if (_stricmp((const char*)(base + imp->Name), "user32.dll") != 0) continue;
        if (!imp->OriginalFirstThunk) continue;   // no name table: cannot match by name
        auto names = (IMAGE_THUNK_DATA*)(base + imp->OriginalFirstThunk);
        auto slots = (IMAGE_THUNK_DATA*)(base + imp->FirstThunk);
        for (; names->u1.AddressOfData; ++names, ++slots) {
            if (IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal)) continue;
            auto byName = (IMAGE_IMPORT_BY_NAME*)(base + names->u1.AddressOfData);
            for (size_t k = 0; k < hookCount; ++k) {
                const IatHook& h = hooks[k];
                if (strcmp((const char*)byName->Name, h.name) != 0) continue;
                DWORD old;
                if (VirtualProtect(&slots->u1.Function, sizeof(void*), PAGE_READWRITE, &old)) {
                    slots->u1.Function = (ULONG_PTR)h.hook;
                    VirtualProtect(&slots->u1.Function, sizeof(void*), old, &old);
                    ++patched;
                }
            }
        }
    }
    return patched;
}

// Applies kSrHooks to every module loaded from an SR runtime folder (new ones only).
void PatchSrModules()
{
    static std::vector<HMODULE> done;
    HMODULE mods[512];
    DWORD bytes = 0;
    if (!K32EnumProcessModules(GetCurrentProcess(), mods, sizeof(mods), &bytes)) return;
    for (DWORD i = 0; i < bytes / sizeof(HMODULE); ++i) {
        if (std::find(done.begin(), done.end(), mods[i]) != done.end()) continue;
        wchar_t path[MAX_PATH];
        GetModuleFileNameW(mods[i], path, MAX_PATH);
        _wcslwr_s(path);
        if (!wcsstr(path, L"\\simulated reality\\") && !wcsstr(path, L"\\spatiallabs\\") && !wcsstr(path, L"\\leiasr\\"))
            continue;
        done.push_back(mods[i]);
        int n = PatchImports(mods[i], kSrHooks, std::size(kSrHooks));
        if (n) Log("upscale: %ls: %d geometry imports use physical pixels", wcsrchr(path, L'\\') + 1, n);
    }
}

}   // namespace

// Patches the exe and every DLL loaded from the game's folder (except this one).
void InstallDisplayModeHooks()
{
    wchar_t gameDir[MAX_PATH];
    GetModuleFileNameW(nullptr, gameDir, MAX_PATH);
    if (wchar_t* s = wcsrchr(gameDir, L'\\')) s[1] = 0;
    const size_t dirLen = wcslen(gameDir);
    HMODULE self = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       (LPCWSTR)&InstallDisplayModeHooks, &self);
    int total = 0;
    std::string patchedIn;
    HMODULE mods[512];
    DWORD bytes = 0;
    if (K32EnumProcessModules(GetCurrentProcess(), mods, sizeof(mods), &bytes)) {
        for (DWORD i = 0; i < bytes / sizeof(HMODULE); ++i) {
            if (mods[i] == self) continue;
            wchar_t path[MAX_PATH];
            GetModuleFileNameW(mods[i], path, MAX_PATH);
            if (_wcsnicmp(path, gameDir, dirLen) != 0) continue;
            int n = PatchImports(mods[i], kIatHooks, std::size(kIatHooks));
            if (!n) continue;
            total += n;
            char name[MAX_PATH];
            WideCharToMultiByte(CP_UTF8, 0, path + dirLen, -1, name, MAX_PATH, nullptr, nullptr);
            patchedIn += std::string(patchedIn.empty() ? "" : ", ") + name;
        }
    }
    Log("upscale: display mode imports redirected: %d in %s (report blocked mode: %d)", total,
        patchedIn.empty() ? "nothing" : patchedIn.c_str(), g_cfg.upscaleReportMode);
}

// Presents both eyes (sbsFbo: left [0, eyeW), right [eyeW, 2*eyeW), game's GL context, neutral
// scissor/sRGB/mask state; GL framebuffer bindings are left changed for the caller to restore) in
// our window at native resolution. Returns false when the frame should go to the game's window.
bool PresentFrame(HWND game, GLuint sbsFbo, int eyeW, int eyeH, bool weave)
{
    if (g.failed) return false;
    if (!g.sc) {
        const int open = OpenPresenter(game);
        if (open < 0) {
            g.failed = true;
            Log("upscale: unavailable, the game's own window is used");
        }
        if (open <= 0) return false;
    }
    const bool show = ShouldShow();
    Show(show);
    if (!show) return false;
    FollowGameWindow();
    // The weaver only fills an area the size of one input view, so the views are upscaled to the
    // window size here (left [0, w), right [w, 2w)) instead of being passed at the game's size.
    if (!EnsureInterop(g.in, g.w * 2, g.h, true) || !EnsureInterop(g.out, g.w, g.h, false)) {
        g.failed = true;
        Log("upscale: interop textures failed, the game's own window is used");
        return false;
    }

    // GL -> D3D11. GL rows run bottom-up and D3D rows top-down, so every blit flips vertically.
    HANDLE objs[2] = {g.in.handle, g.out.handle};
    if (!g.lock(g.interop, 2, objs)) return false;
    R.BindFramebuffer(GL_READ_FRAMEBUFFER, sbsFbo);
    const int L0 = g_cfg.swapEyes ? eyeW : 0, R0 = g_cfg.swapEyes ? 0 : eyeW;
    if (weave) {
        R.BindFramebuffer(GL_DRAW_FRAMEBUFFER, g.in.fbo);
        R.BlitFramebuffer(L0, 0, L0 + eyeW, eyeH, 0, g.h, g.w, 0, GL_COLOR_BUFFER_BIT, GL_LINEAR);
        R.BlitFramebuffer(R0, 0, R0 + eyeW, eyeH, g.w, g.h, 2 * g.w, 0, GL_COLOR_BUFFER_BIT, GL_LINEAR);
    }
    R.BindFramebuffer(GL_DRAW_FRAMEBUFFER, g.out.fbo);
    auto blit = [&](int sx, int dx0, int dy0, int dx1, int dy1) {   // destination in D3D (top-down) pixels
        R.BlitFramebuffer(sx, 0, sx + eyeW, eyeH, dx0, g.h - dy0, dx1, g.h - dy1, GL_COLOR_BUFFER_BIT, GL_LINEAR);
    };
    switch (g_cfg.output) {
    case 1: blit(L0, 0, 0, g.w, g.h / 2); blit(R0, 0, g.h / 2, g.w, g.h); break;   // left eye on top
    case 2: blit(L0, 0, 0, g.w, g.h); break;
    case 3: blit(R0, 0, 0, g.w, g.h); break;
    default: blit(L0, 0, 0, g.w / 2, g.h); blit(R0, g.w / 2, 0, g.w, g.h); break;
    }
    g.unlock(g.interop, 2, objs);

    bool woven = false;
    if (weave) {
        PhysicalDpiScope dpi;   // the weaver reads our window position: physical coordinates
        static bool srPatched;
        if (!srPatched) {   // before the SR runtime runs: load it and make its geometry calls physical
            SrPreloadRuntime();
            PatchSrModules();
        }
        const bool ready = SrAvailableDx11(g.wnd, g.ctx);
        if (!srPatched) {
            PatchSrModules();   // DLLs the weaver loaded on creation
            srPatched = true;
        }
        if (ready) {
            g.ctx->OMSetRenderTargets(1, &g.rtv, nullptr);
            D3D11_VIEWPORT vp{0, 0, (float)g.w, (float)g.h, 0, 1};
            g.ctx->RSSetViewports(1, &vp);
            D3D11_RECT sr{0, 0, g.w, g.h};
            g.ctx->RSSetScissorRects(1, &sr);
            woven = SrWeaveDx11(g.in.srv, g.w, g.h, DXGI_FORMAT_R8G8B8A8_UNORM);
        }
    }
    SrSetLens(woven);
    if (!woven) g.ctx->CopyResource(g.back, g.out.tex);
    g.sc->Present(1, 0);
    if (++g.frames == 1)
        Log("upscale: first frame presented (each eye %dx%d -> window %dx%d, %s)", eyeW, eyeH, g.w, g.h,
            woven ? "SR woven" : "SBS");
    return true;
}

void PresentShutdown()
{
    if (g.makeHook) UnhookWindowsHookEx(g.makeHook);
    g.makeHook = nullptr;
    if (g.wnd) {
        if (g.foreign) PostMessageW(g.wnd, WM_CLOSE, 0, 0);   // destroyed by its own thread
        else DestroyWindow(g.wnd);
        g.wnd = nullptr;
    }
}

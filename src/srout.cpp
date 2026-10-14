// SR output (Simulated Reality: Acer SpatialLabs, Leia SR panels): both eyes are handed to the SR
// SDK's OpenGL weaver, which interlaces them for the eye-tracked lenticular display, inside the
// game's own GL context right before SwapBuffers.
//
// Sequence (as SRWeave for geo-11): SRContext::create -> CreateGLWeaver(ctx, hwnd)
//   -> setLatencyInFrames -> SwitchableLensHint::create -> context->initialize()
// The SR runtime DLLs are delay-loaded from the SR Platform folders, so the wrapper still works
// (as plain SBS) on PCs without SR.
#include "common.h"
#include <delayimp.h>
#include <exception>

#include <d3d11.h>

#include "sr/management/srcontext.h"
#include "sr/sense/display/switchablehint.h"
#include "sr/utility/exception.h"
#include "sr/weaver/dx11weaver.h"
#include "sr/weaver/glweaver.h"

namespace {

// 64-bit runtime (SimulatedRealityCore.dll ...) under Program Files, 32-bit runtime
// (SimulatedRealityCore32.dll ...) under Program Files (x86).
const wchar_t* const kRuntimeDirs[] = {
#if defined(_WIN64)
    L"C:\\Program Files\\Acer\\SpatialLabs\\Platform\\bin",
    L"C:\\Program Files\\Simulated Reality\\Platform\\bin",
    L"C:\\Program Files\\LeiaSR\\Platform\\bin",
#else
    L"C:\\Program Files (x86)\\Acer\\SpatialLabs\\Platform\\bin",
    L"C:\\Program Files (x86)\\Simulated Reality\\Platform\\bin",
    L"C:\\Program Files (x86)\\LeiaSR\\Platform\\bin",
#endif
};

FARPROC WINAPI DliHook(unsigned reason, DelayLoadInfo* info)
{
    if (reason != dliNotePreLoadLibrary || !info || !info->szDll) return nullptr;
    wchar_t name[MAX_PATH] = {};
    MultiByteToWideChar(CP_ACP, 0, info->szDll, -1, name, MAX_PATH);
    for (const wchar_t* dir : kRuntimeDirs) {
        std::wstring cand = std::wstring(dir) + L"\\" + name;
        if (HMODULE m = LoadLibraryExW(cand.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH)) return (FARPROC)m;
    }
    return nullptr;   // normal search (PATH)
}

struct SrState {
    SR::SRContext* ctx = nullptr;
    SR::SwitchableLensHint* lens = nullptr;
    SR::IGLWeaver1* weaver = nullptr;
    HWND hwnd = nullptr;
    bool initialized = false;
    bool dead = false;        // SR unavailable for this process: stay on SBS
    bool lensOn = false;
    ULONGLONG lastTry = 0;
    int tries = 0;
    GLuint inputTex = 0;
    int inputW = 0, inputH = 0;
    unsigned frames = 0;
    // upscale mode presents through Direct3D 11 (present.cpp) and weaves with the DX11 weaver
    SR::IDX11Weaver1* dxWeaver = nullptr;
    ID3D11ShaderResourceView* dxInput = nullptr;
    int dxW = 0, dxH = 0;
} g;

int DelayLoadFilter(unsigned code)
{
    return (code == VcppException(ERROR_SEVERITY_ERROR, ERROR_MOD_NOT_FOUND) ||
            code == VcppException(ERROR_SEVERITY_ERROR, ERROR_PROC_NOT_FOUND))
               ? EXCEPTION_EXECUTE_HANDLER
               : EXCEPTION_CONTINUE_SEARCH;
}

void DoCreateContext(SR::SRContext** out) { *out = SR::SRContext::create(); }

bool CreateContextSEH(SR::SRContext** out)
{
    __try {
        DoCreateContext(out);
        return true;
    } __except (DelayLoadFilter(GetExceptionCode())) {
        return false;
    }
}

WeaverErrorCode DoCreateWeaver(HWND hwnd, SR::IGLWeaver1** out) { return SR::CreateGLWeaver(*g.ctx, hwnd, out); }

WeaverErrorCode DoCreateDx11Weaver(HWND hwnd, ID3D11DeviceContext* dctx, SR::IDX11Weaver1** out)
{
    return SR::CreateDX11Weaver(g.ctx, dctx, hwnd, out);
}

bool CreateDx11WeaverSEH(HWND hwnd, ID3D11DeviceContext* dctx, SR::IDX11Weaver1** out, WeaverErrorCode* code)
{
    __try {
        *code = DoCreateDx11Weaver(hwnd, dctx, out);
        return true;
    } __except (DelayLoadFilter(GetExceptionCode())) {
        return false;
    }
}

bool CreateWeaverSEH(HWND hwnd, SR::IGLWeaver1** out, WeaverErrorCode* code)
{
    __try {
        *code = DoCreateWeaver(hwnd, out);
        return true;
    } __except (DelayLoadFilter(GetExceptionCode())) {
        return false;
    }
}

bool EnsureContext()
{
    if (g.ctx) return true;
    if (g.dead) return false;
    ULONGLONG now = GetTickCount64();
    if (now - g.lastTry < 2000) return false;   // don't hammer the service from the render thread
    g.lastTry = now;
    g.tries++;
    try {
        if (!CreateContextSEH(&g.ctx)) {
            Log("SR runtime DLLs not found - SR output unavailable, using SBS");
            g.dead = true;
            return false;
        }
    } catch (const SR::ServerNotAvailableException&) {
        g.ctx = nullptr;
        if (g.tries == 1 || g.tries % 10 == 0) Log("SR Service not available (try %d)", g.tries);
        if (g.tries >= 30) {
            Log("giving up on SR Service - using SBS");
            g.dead = true;
        }
        return false;
    } catch (const std::exception& e) {
        Log("SRContext::create failed: %s", e.what());
        g.ctx = nullptr;
        g.dead = true;
        return false;
    } catch (...) {
        Log("SRContext::create failed (unknown exception)");
        g.ctx = nullptr;
        g.dead = true;
        return false;
    }
    Log("SR context created (try %d)", g.tries);
    return g.ctx != nullptr;
}

void FinishInit()
{
    if (!g.ctx || g.initialized) return;
    try {
        g.lens = SR::SwitchableLensHint::create(*g.ctx);   // owned by the context
        g.ctx->initialize();
        g.initialized = true;
        Log("SR context initialized");
    } catch (const std::exception& e) {
        Log("SR initialize failed: %s", e.what());
        g.dead = true;
    } catch (...) {
        Log("SR initialize failed (unknown exception)");
        g.dead = true;
    }
}

bool EnsureWeaver(HWND hwnd)
{
    if (g.weaver) return true;
    if (g.dead || !EnsureContext()) return false;
    WeaverErrorCode code = WeaverErrorCode::WeaverSuccess;
    bool loaded = false;
    try {
        loaded = CreateWeaverSEH(g_cfg.srWeaverHwnd ? hwnd : nullptr, &g.weaver, &code);
    } catch (const std::exception& e) {
        Log("CreateGLWeaver threw: %s", e.what());
        g.weaver = nullptr;
    } catch (...) {
        Log("CreateGLWeaver threw (unknown)");
        g.weaver = nullptr;
    }
    if (!loaded) Log("SimulatedRealityOpenGL DLL not found");
    if (!loaded || code != WeaverErrorCode::WeaverSuccess || !g.weaver) {
        Log("CreateGLWeaver failed (code %d) - SR output unavailable, using SBS", (int)code);
        g.weaver = nullptr;
        g.dead = true;
        return false;
    }
    try {
        g.weaver->setLatencyInFrames((uint64_t)g_cfg.srLatencyFrames);
    } catch (...) {
    }
    g.hwnd = hwnd;
    Log("SR OpenGL weaver created (hwnd %p, latency %d frames)", hwnd, g_cfg.srLatencyFrames);
    FinishInit();
    return !g.dead;
}

}   // namespace

extern "C" const PfnDliHook __pfnDliNotifyHook2 = DliHook;

// Loads the SR runtime DLLs now (the delay-load helper then finds them), so their imports can be
// adjusted before any of their code runs (present.cpp, upscale mode).
void SrPreloadRuntime()
{
#if defined(_WIN64)
    const wchar_t* const names[] = {L"SimulatedRealityCore.dll", L"SimulatedRealityDisplays.dll",
                                    L"SimulatedRealityDirectX.dll"};
#else
    const wchar_t* const names[] = {L"SimulatedRealityCore32.dll", L"SimulatedRealityDisplays32.dll",
                                    L"SimulatedRealityDirectX32.dll"};
#endif
    for (const wchar_t* name : names)
        for (const wchar_t* dir : kRuntimeDirs)
            if (LoadLibraryExW((std::wstring(dir) + L"\\" + name).c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH)) break;
}

bool SrAvailable(HWND hwnd)
{
    return EnsureWeaver(hwnd);
}

void SrSetLens(bool on)
{
    if (!g.lens || g.lensOn == on) return;
    try {
        if (on) g.lens->enable();
        else g.lens->disable();
        g.lensOn = on;
        Log("SR lens %s", on ? "on" : "off");
    } catch (...) {
        Log("SR lens hint call failed");
    }
}

bool SrEyePositions(float left[3], float right[3])
{
    try {
        if (g.weaver) g.weaver->getPredictedEyePositions(left, right);
        else if (g.dxWeaver) g.dxWeaver->getPredictedEyePositions(left, right);
        else return false;
        return true;
    } catch (...) {
        return false;
    }
}

// sbsTex holds the left eye in [0, eyeW) and the right eye in [eyeW, 2*eyeW); the woven image is
// drawn into the currently bound draw framebuffer and viewport.
bool SrWeave(GLuint sbsTex, int eyeW, int eyeH)
{
    if (!g.weaver) return false;
    try {
        if (sbsTex != g.inputTex || eyeW != g.inputW || eyeH != g.inputH) {
            g.weaver->setInputViewTexture(sbsTex, eyeW, eyeH, GL_RGBA8);
            g.inputTex = sbsTex;
            g.inputW = eyeW;
            g.inputH = eyeH;
        }
        g.weaver->weave();
    } catch (const std::exception& e) {
        Log("weave() threw: %s - SR output disabled", e.what());
        g.weaver = nullptr;
        g.dead = true;
        return false;
    } catch (...) {
        Log("weave() threw (unknown) - SR output disabled");
        g.weaver = nullptr;
        g.dead = true;
        return false;
    }
    if (++g.frames == 1) Log("first woven frame (each eye %dx%d)", eyeW, eyeH);
    return true;
}

// ---- Direct3D 11 weaver (upscale presentation window) -----------------------------------------

bool SrAvailableDx11(HWND hwnd, ID3D11DeviceContext* dctx)
{
    if (g.dxWeaver) return true;
    if (g.dead || !EnsureContext()) return false;
    WeaverErrorCode code = WeaverErrorCode::WeaverSuccess;
    bool loaded = false;
    try {
        loaded = CreateDx11WeaverSEH(g_cfg.srWeaverHwnd ? hwnd : nullptr, dctx, &g.dxWeaver, &code);
    } catch (...) {
        g.dxWeaver = nullptr;
    }
    if (!loaded || code != WeaverErrorCode::WeaverSuccess || !g.dxWeaver) {
        Log("CreateDX11Weaver failed (code %d%s) - SR output unavailable", (int)code, loaded ? "" : ", DLL not found");
        g.dxWeaver = nullptr;
        g.dead = true;
        return false;
    }
    try {
        g.dxWeaver->setLatencyInFrames((uint64_t)g_cfg.srLatencyFrames);
        g.dxWeaver->setContext(dctx);
    } catch (...) {
    }
    Log("SR DX11 weaver created (hwnd %p, latency %d frames)", hwnd, g_cfg.srLatencyFrames);
    FinishInit();
    return !g.dead;
}

// Weaves srv (left eye [0, eyeW), right eye [eyeW, 2*eyeW)) into the bound render target.
bool SrWeaveDx11(ID3D11ShaderResourceView* srv, int eyeW, int eyeH, DXGI_FORMAT fmt)
{
    if (!g.dxWeaver) return false;
    try {
        if (srv != g.dxInput || eyeW != g.dxW || eyeH != g.dxH) {
            g.dxWeaver->setInputViewTexture(srv, eyeW, eyeH, fmt);
            g.dxInput = srv;
            g.dxW = eyeW;
            g.dxH = eyeH;
        }
        g.dxWeaver->weave();
    } catch (...) {
        Log("DX11 weave() threw - SR output disabled");
        g.dxWeaver = nullptr;
        g.dead = true;
        return false;
    }
    if (++g.frames == 1) Log("first woven frame (DX11, each eye %dx%d)", eyeW, eyeH);
    return true;
}

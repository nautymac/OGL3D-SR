// opengl32.dll proxy: loads System32\opengl32.dll, forwards every export it does not implement,
// and routes the WGL entry points into the stereo engine.
#include "common.h"
#include <psapi.h>
#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <mutex>
#include <vector>

#if defined(_M_X64)
#include "gen/forward_names_x64.inc"
#else
#include "gen/forward_names_x86.inc"
#endif

RealGL R;
Config g_cfg;

static HMODULE g_self;
static HMODULE g_real;
static std::once_flag g_realOnce;

extern "C" void* g_s3dForward[S3D_FORWARD_COUNT] = {};

std::wstring ModuleDir()
{
    wchar_t path[MAX_PATH];
    GetModuleFileNameW(g_self, path, MAX_PATH);
    std::wstring s(path);
    return s.substr(0, s.find_last_of(L"\\/") + 1);
}

std::wstring IniPath() { return ModuleDir() + L"OGL3D.ini"; }

// ---------------------------------------------------------------------------------------------
// log

static FILE* g_log;
static std::mutex g_logLock;

void Log(const char* fmt, ...)
{
    std::lock_guard<std::mutex> lk(g_logLock);
    if (!g_log) {
        g_log = _wfsopen((ModuleDir() + L"OGL3D.log").c_str(), L"w", _SH_DENYWR);
        if (!g_log) return;
    }
    SYSTEMTIME t;
    GetLocalTime(&t);
    fprintf(g_log, "%02d:%02d:%02d.%03d [%5lu] ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds,
            GetCurrentThreadId());
    va_list ap;
    va_start(ap, fmt);
    vfprintf(g_log, fmt, ap);
    va_end(ap);
    fputc('\n', g_log);
    fflush(g_log);
}

// ---------------------------------------------------------------------------------------------
// config

static std::wstring IniStr(const wchar_t* sec, const wchar_t* key, const wchar_t* def)
{
    wchar_t buf[512];
    GetPrivateProfileStringW(sec, key, def, buf, 512, IniPath().c_str());
    std::wstring s(buf);
    if (size_t p = s.find(L';'); p != std::wstring::npos) s.resize(p);   // trailing comment
    while (!s.empty() && iswspace(s.back())) s.pop_back();
    return s;
}

static float IniFloat(const wchar_t* sec, const wchar_t* key, float def)
{
    std::wstring s = IniStr(sec, key, L"");
    return s.empty() ? def : (float)_wtof(s.c_str());
}

static int IniInt(const wchar_t* sec, const wchar_t* key, int def)
{
    std::wstring s = IniStr(sec, key, L"");
    return s.empty() ? def : _wtoi(s.c_str());
}

// "ctrl F3", "ctrl+shift+T", "F12", "no_modifiers 0x70"
static Hotkey ParseKey(const std::wstring& text, Hotkey def)
{
    if (text.empty()) return def;
    Hotkey k;
    std::wstring tok;
    auto flush = [&] {
        if (tok.empty()) return;
        std::wstring t;
        for (wchar_t c : tok) t += (wchar_t)towlower(c);
        if (t == L"ctrl" || t == L"control") k.ctrl = true;
        else if (t == L"shift") k.shift = true;
        else if (t == L"alt") k.alt = true;
        else if (t.size() >= 2 && t[0] == L'f' && iswdigit(t[1])) k.vk = VK_F1 + _wtoi(t.c_str() + 1) - 1;
        else if (t.size() > 2 && t[0] == L'0' && t[1] == L'x') k.vk = (int)wcstol(t.c_str(), nullptr, 16);
        else if (t.size() == 1) k.vk = towupper(t[0]);
        else if (t.size() == 4 && t.compare(0, 3, L"num") == 0 && iswdigit(t[3])) k.vk = VK_NUMPAD0 + (t[3] - L'0');
        else if (t == L"numplus") k.vk = VK_ADD;
        else if (t == L"numminus") k.vk = VK_SUBTRACT;
        else if (t == L"nummul") k.vk = VK_MULTIPLY;
        else if (t == L"numdiv") k.vk = VK_DIVIDE;
        else if (t == L"home") k.vk = VK_HOME;
        else if (t == L"end") k.vk = VK_END;
        else if (t == L"insert") k.vk = VK_INSERT;
        else if (t == L"delete") k.vk = VK_DELETE;
        else if (t == L"pageup") k.vk = VK_PRIOR;
        else if (t == L"pagedown") k.vk = VK_NEXT;
        else if (t == L"left") k.vk = VK_LEFT;
        else if (t == L"right") k.vk = VK_RIGHT;
        else if (t == L"up") k.vk = VK_UP;
        else if (t == L"down") k.vk = VK_DOWN;
        else if (t == L"comma") k.vk = VK_OEM_COMMA;
        else if (t == L"period") k.vk = VK_OEM_PERIOD;
        tok.clear();
    };
    for (wchar_t c : text) {
        if (c == L'+' || iswspace(c)) flush();
        else tok += c;
    }
    flush();
    return k.vk ? k : def;
}

void LoadConfig()
{
    Config& c = g_cfg;
    c.enable = IniInt(L"Stereo", L"enable", c.enable) != 0;
    c.separation = IniFloat(L"Stereo", L"separation", c.separation);
    c.maxSeparation = IniFloat(L"Stereo", L"max_separation", c.maxSeparation);
    std::wstring conv = IniStr(L"Stereo", L"convergence", L"");
    c.autoConvergence = conv == L"auto";
    if (!c.autoConvergence && !conv.empty()) c.convergence = (float)_wtof(conv.c_str());
    c.autoPopout = IniFloat(L"Stereo", L"auto_popout", c.autoPopout);
    c.autoSpeed = std::max(0.1f, IniFloat(L"Stereo", L"auto_speed", c.autoSpeed));
    c.autoStart = IniFloat(L"Stereo", L"auto_start", 0.0f);
    if (c.autoConvergence && c.autoStart > 0.0f) c.convergence = c.autoStart;
    c.swapEyes = IniInt(L"Stereo", L"swap_eyes", c.swapEyes) != 0;
    std::wstring out = IniStr(L"Stereo", L"output", L"auto");
    c.output = out == L"sbs" ? 0 : out == L"tab" ? 1 : out == L"left" ? 2 : out == L"right" ? 3 : 4;   // auto/sr = 4
    c.monoW1 = IniInt(L"Stereo", L"mono_w1", c.monoW1) != 0;
    c.fixedFunction = IniInt(L"Stereo", L"fixed_function", c.fixedFunction) != 0;
    c.orthoDepth = IniFloat(L"Stereo", L"ortho_depth", c.orthoDepth);
    c.orthoConvergence = IniFloat(L"Stereo", L"ortho_convergence", c.orthoConvergence);
    c.flatEnable = IniInt(L"Flat", L"enable", c.flatEnable) != 0;
    c.flatRule = IniStr(L"Flat", L"rule", L"feet") == L"order" ? 1 : 0;
    c.flatTop = IniFloat(L"Flat", L"depth_top", c.flatTop);
    c.flatZero = IniFloat(L"Flat", L"depth_screen", c.flatZero);
    c.flatPlane = IniFloat(L"Flat", L"plane_size", c.flatPlane);
    c.flatPlaneHeight = IniFloat(L"Flat", L"plane_height", c.flatPlaneHeight);
    c.flatSpriteMin = IniFloat(L"Flat", L"sprite_min", c.flatSpriteMin);
    c.flatScreenBatch = IniInt(L"Flat", L"screen_batch", c.flatScreenBatch);
    c.flatCacheScale = IniFloat(L"Flat", L"cache_scale", c.flatCacheScale);
    c.flatGroupByDraw = _wcsicmp(IniStr(L"Flat", L"group", L"texture").c_str(), L"draw") == 0;
    c.flatOrderCurve = IniFloat(L"Flat", L"order_curve", c.flatOrderCurve);
    c.silhouette.clear();
    {
        std::wstring list = IniStr(L"Flat", L"silhouette", L"");
        for (const wchar_t* p = list.c_str(); *p;) {
            wchar_t* end = nullptr;
            uint32_t hash = (uint32_t)wcstoul(p, &end, 16);
            if (end == p) { ++p; continue; }
            if (hash) c.silhouette.push_back(hash);
            p = end;
        }
    }
    auto ascii = [](const std::wstring& w) { std::string s; for (wchar_t ch : w) s += (char)(ch < 128 ? ch : '?'); return s; };
    c.silUv = ascii(IniStr(L"Flat", L"silhouette_uv", L"texcoordOut"));
    c.silTex = ascii(IniStr(L"Flat", L"silhouette_tex", L"texture1"));
    c.silDepth = IniFloat(L"Flat", L"silhouette_depth", c.silDepth);
    c.silThreshold = IniFloat(L"Flat", L"silhouette_threshold", c.silThreshold);
    c.inject =IniInt(L"Inject", L"enable", c.inject) != 0;
    c.blockProgramBinary = IniInt(L"Inject", L"block_program_binary", c.blockProgramBinary) != 0;
    c.cgGlsl = IniInt(L"Inject", L"cg_glsl", c.cgGlsl);
    c.arbPrograms = IniInt(L"Inject", L"arb_programs", c.arbPrograms) != 0;
    c.dumpShaders = IniInt(L"Inject", L"dump_shaders", c.dumpShaders) != 0;
    c.minTargetWidth = IniInt(L"Targets", L"min_width", c.minTargetWidth);
    c.aspectTolerance = IniFloat(L"Targets", L"aspect_tolerance", c.aspectTolerance);
    c.keySepDown = ParseKey(IniStr(L"Keys", L"separation_down", L""), c.keySepDown);
    c.keySepUp = ParseKey(IniStr(L"Keys", L"separation_up", L""), c.keySepUp);
    c.keyConvDown = ParseKey(IniStr(L"Keys", L"convergence_down", L""), c.keyConvDown);
    c.keyConvUp = ParseKey(IniStr(L"Keys", L"convergence_up", L""), c.keyConvUp);
    c.keySave = ParseKey(IniStr(L"Keys", L"save", L""), c.keySave);
    c.keyToggle = ParseKey(IniStr(L"Keys", L"toggle", L""), c.keyToggle);
    c.keyDump = ParseKey(IniStr(L"Keys", L"dump_frame", L""), c.keyDump);
    c.keyWeave = ParseKey(IniStr(L"Keys", L"weave_toggle", L""), c.keyWeave);
    c.keyShowValues = ParseKey(IniStr(L"Keys", L"show_values", L""), c.keyShowValues);
    c.keyAutoConv = ParseKey(IniStr(L"Keys", L"auto_convergence", L""), c.keyAutoConv);
    c.keyHunt = ParseKey(IniStr(L"Keys", L"hunt_toggle", L""), c.keyHunt);
    c.keyHuntPrev = ParseKey(IniStr(L"Keys", L"hunt_prev", L""), c.keyHuntPrev);
    c.keyHuntNext = ParseKey(IniStr(L"Keys", L"hunt_next", L""), c.keyHuntNext);
    c.keyHuntMode = ParseKey(IniStr(L"Keys", L"hunt_mode", L""), c.keyHuntMode);
    c.swallowKeys = IniInt(L"Keys", L"swallow", c.swallowKeys) != 0;
    c.showValues = IniInt(L"Overlay", L"show_values", c.showValues) != 0;
    c.dpiAware = IniInt(L"Display", L"dpi_aware", c.dpiAware) != 0;
    c.upscale = IniInt(L"Display", L"upscale", c.upscale) != 0;
    c.upscaleReportMode = IniInt(L"Display", L"upscale_report_mode", c.upscaleReportMode) != 0;
    c.swapInterval = IniInt(L"Display", L"swap_interval", c.swapInterval);
    c.virtualBackbuffer = IniInt(L"Display", L"virtual_backbuffer", c.virtualBackbuffer) != 0;
    c.overrides.clear();
    {
        std::vector<wchar_t> sec(32768);
        DWORD n = GetPrivateProfileSectionW(L"ShaderOverride", sec.data(), (DWORD)sec.size(), IniPath().c_str());
        for (const wchar_t* p = sec.data(); p < sec.data() + n && *p; p += wcslen(p) + 1) {
            std::wstring line(p);
            size_t eq = line.find(L'=');
            if (eq == std::wstring::npos || line[0] == L';') continue;
            uint32_t hash = (uint32_t)wcstoul(line.substr(0, eq).c_str(), nullptr, 16);
            std::wstring v = line.substr(eq + 1);
            while (!v.empty() && iswspace(v.front())) v.erase(v.begin());
            for (int m = 0; m < kModeCount; ++m)
                if (v.compare(0, wcslen(ModeName(m)), ModeName(m)) == 0) c.overrides[hash] = m;
        }
    }
    c.srLatencyFrames = IniInt(L"SR", L"latency_frames", c.srLatencyFrames);
    c.srWeaverHwnd = IniInt(L"SR", L"weaver_hwnd", c.srWeaverHwnd) != 0;
    c.logLevel = IniInt(L"Debug", L"log_level", c.logLevel);
    std::wstring df = IniStr(L"Debug", L"dump_frames", L"");
    c.dumpFrames.clear();
    for (wchar_t ch : df) c.dumpFrames += (char)(ch < 128 ? ch : '?');
    std::wstring tf = IniStr(L"Debug", L"trace_frames", L"");
    c.traceFrames.clear();
    for (wchar_t ch : tf) c.traceFrames += (char)(ch < 128 ? ch : '?');
    c.traceSteps = IniInt(L"Debug", L"trace_steps", c.traceSteps) != 0;
    c.statsEvery = IniInt(L"Debug", L"stats_every", c.statsEvery);
    c.logCrashes = IniInt(L"Debug", L"log_crashes", c.logCrashes) != 0;
    c.flickerProbe = IniInt(L"Debug", L"flicker_probe", c.flickerProbe) != 0;
    c.markBackbuffer = IniInt(L"Debug", L"mark_backbuffer", c.markBackbuffer) != 0;
    c.dumpDark = IniInt(L"Debug", L"dump_dark", c.dumpDark);
    Log("config: enable=%d separation=%.2f max_separation=%.3f convergence=%.3f%s output=%d swap=%d "
        "mono_w1=%d inject=%d block_binary=%d shader_overrides=%zu",
        c.enable, c.separation, c.maxSeparation, c.convergence, c.autoConvergence ? " (auto)" : "", c.output,
        c.swapEyes, c.monoW1, c.inject, c.blockProgramBinary, c.overrides.size());
    if (c.flatEnable)
        Log("flat depth: rule=%s top=%.2f screen=%.2f plane=%.2fx%.2f sprite_min=%.2f screen_batch=%d", c.flatRule ? "order" : "feet", c.flatTop, c.flatZero,
            c.flatPlane, c.flatPlaneHeight, c.flatSpriteMin, c.flatScreenBatch);
}

const wchar_t* ModeName(int mode)
{
    static const wchar_t* const kNames[kModeCount] = {L"stereo", L"mono", L"skip"};
    return mode >= 0 && mode < kModeCount ? kNames[mode] : L"stereo";
}

bool KoreanUi()
{
    return PRIMARYLANGID(GetUserDefaultUILanguage()) == LANG_KOREAN;
}

void SaveAutoStart(float conv)
{
    wchar_t buf[64];
    swprintf_s(buf, L"%.4g", conv);
    WritePrivateProfileStringW(L"Stereo", L"auto_start", buf, IniPath().c_str());
    g_cfg.autoStart = conv;
}

void SaveStereoValues()
{
    wchar_t buf[64];
    swprintf_s(buf, L"%.2f", g_cfg.separation);
    WritePrivateProfileStringW(L"Stereo", L"separation", buf, IniPath().c_str());
    if (g_cfg.autoConvergence) wcscpy_s(buf, L"auto");
    else swprintf_s(buf, L"%.4f", g_cfg.convergence);
    WritePrivateProfileStringW(L"Stereo", L"convergence", buf, IniPath().c_str());
    if (g_cfg.flatEnable) {
        swprintf_s(buf, L"%.3f", g_cfg.flatZero);
        WritePrivateProfileStringW(L"Flat", L"depth_screen", buf, IniPath().c_str());
    }
    // the whole [ShaderOverride] section is rewritten: "hash=mode\0...\0\0"
    std::wstring sec;
    for (const auto& kv : g_cfg.overrides) {
        if (kv.second == kModeStereo) continue;
        swprintf_s(buf, L"%08x=%ls", kv.first, ModeName(kv.second));
        sec += buf;
        sec += L'\0';
    }
    sec += L'\0';
    WritePrivateProfileSectionW(L"ShaderOverride", sec.c_str(), IniPath().c_str());
    Log("saved separation=%.2f convergence=%.4f, %zu shader overrides", g_cfg.separation, g_cfg.convergence,
        g_cfg.overrides.size());
}

// ---------------------------------------------------------------------------------------------
// real opengl32

HMODULE RealOpenGL()
{
    std::call_once(g_realOnce, [] {
        wchar_t dir[MAX_PATH];
        GetSystemDirectoryW(dir, MAX_PATH);
        g_real = LoadLibraryW((std::wstring(dir) + L"\\opengl32.dll").c_str());
        if (!g_real) {
            Log("FATAL: cannot load system opengl32.dll (%lu)", GetLastError());
            return;
        }
#define S3D_LOAD11(ret, name, args)                                                                 \
    R.name = (decltype(R.name))GetProcAddress(g_real, (#name)[0] == 'w' ? #name : "gl" #name);
        S3D_GL11_FUNCS(S3D_LOAD11)
#undef S3D_LOAD11
    });
    return g_real;
}

void LoadRealExtensions()
{
    if (R.BindFramebuffer) return;
#define S3D_LOADEXT(ret, name, args)                                                                \
    if (!R.name) R.name = (decltype(R.name))R.wglGetProcAddress((#name)[0] == 'w' ? #name : "gl" #name);
    S3D_EXT_FUNCS(S3D_LOADEXT)
#undef S3D_LOADEXT
    Log("real extensions: BindFramebuffer=%p BlitFramebuffer=%p ShaderSource=%p TexStorage2D=%p",
        R.BindFramebuffer, R.BlitFramebuffer, R.ShaderSource, R.TexStorage2D);
}

extern "C" void* __cdecl S3D_ResolveForward(unsigned idx)
{
    if (idx >= S3D_FORWARD_COUNT || !RealOpenGL()) return nullptr;
    void* p = (void*)GetProcAddress(g_real, kForwardNames[idx]);
    g_s3dForward[idx] = p;
    if (!p) Log("unresolved forward %s", kForwardNames[idx]);
    return p;
}

// ---------------------------------------------------------------------------------------------
// WGL exports

extern "C" {

// GL 1.1 functions we implement as exports (src/exports_impl.txt). Loaders such as glad behind
// SDL_GL_GetProcAddress (AGS 3.6) ask wglGetProcAddress for these as well, and NVIDIA answers with
// the driver's own entry point, which would bypass the wrapper: hand out our export instead.
static const char* const kOwnGl11[] = {
    "glDrawArrays", "glDrawElements", "glClear", "glBindTexture", "glDeleteTextures", "glTexImage2D",
    "glTexSubImage2D", "glCopyTexImage2D", "glCopyTexSubImage2D", "glTexParameteri", "glTexParameterf",
    "glTexParameteriv", "glTexParameterfv", "glDrawBuffer", "glReadBuffer", "glGetIntegerv", "glBegin",
    "glEnd", "glNewList", "glEndList", "glCallList", "glCallLists", "glFlush", "glFinish", "glScissor", "glViewport",
};

PROC WINAPI H_wglGetProcAddress(LPCSTR name)
{
    RealOpenGL();
    if ((uintptr_t)name > 0xFFFF && name[0] == 'g' && name[1] == 'l') {
        for (const char* own : kOwnGl11) {
            if (strcmp(own, name) != 0) continue;
            PROC p = GetProcAddress(g_self, name);
            LOGV("wglGetProcAddress(%s) = own export %p", name, p);
            if (p) return p;
        }
    }
    PROC p = R.wglGetProcAddress(name);
    PROC h = HookForName(name, p);
    LOGV("wglGetProcAddress(%s) = %p%s", name, p, h != p ? " [hooked]" : "");
    return h;
}

BOOL WINAPI H_wglMakeCurrent(HDC dc, HGLRC rc)
{
    RealOpenGL();
    BOOL ok = R.wglMakeCurrent(dc, rc);
    if (ok) OnMakeCurrent(dc, rc);
    return ok;
}

HGLRC WINAPI H_wglCreateContext(HDC dc)
{
    RealOpenGL();
    HGLRC rc = R.wglCreateContext(dc);
    PIXELFORMATDESCRIPTOR pfd{};
    int pf = GetPixelFormat(dc);
    DescribePixelFormat(dc, pf, sizeof(pfd), &pfd);
    Log("wglCreateContext(dc=%p) = %p  pixelformat=%d color=%d depth=%d stencil=%d flags=0x%lx", dc, rc,
        pf, pfd.cColorBits, pfd.cDepthBits, pfd.cStencilBits, pfd.dwFlags);
    return rc;
}

BOOL WINAPI H_wglDeleteContext(HGLRC rc)
{
    RealOpenGL();
    OnDeleteContext(rc);
    Log("wglDeleteContext(%p)", rc);
    return R.wglDeleteContext(rc);
}

BOOL WINAPI H_wglShareLists(HGLRC a, HGLRC b)
{
    RealOpenGL();
    Log("wglShareLists(%p, %p)", a, b);
    return R.wglShareLists(a, b);
}

BOOL WINAPI H_wglSwapBuffers(HDC dc)
{
    RealOpenGL();
    if (OnSwapBuffers(dc)) return TRUE;   // presented in the upscale window instead
    return R.wglSwapBuffers(dc);
}

BOOL WINAPI H_wglSwapLayerBuffers(HDC dc, UINT planes)
{
    RealOpenGL();
    if ((planes & WGL_SWAP_MAIN_PLANE) && OnSwapBuffers(dc) && planes == WGL_SWAP_MAIN_PLANE) return TRUE;
    return R.wglSwapLayerBuffers(dc, planes);
}

// Not composed (no game seen using it yet): logged, so a game presenting this way is noticed.
DWORD WINAPI H_wglSwapMultipleBuffers(UINT n, const WGLSWAP* swaps)
{
    RealOpenGL();
    static LONG calls;
    if (InterlockedIncrement(&calls) <= 5)
        Log("wglSwapMultipleBuffers(%u, dc %p, flags 0x%x) - not composed", n, n ? swaps[0].hdc : nullptr, n ? swaps[0].uiFlags : 0u);
    return R.wglSwapMultipleBuffers(n, swaps);
}

}   // extern "C"

// ---------------------------------------------------------------------------------------------
// crash logging: first-chance faults are logged (never handled) with the faulting module and the
// return addresses found on the stack, so a crash inside the driver still names its caller.

// Lock-free (no loader lock: callable while another thread is stuck inside the loader or is suspended):
// the module is the image mapping containing p, named by its mapped file.
void DescribeAddress(const void* p, char* out, size_t n)
{
    MEMORY_BASIC_INFORMATION mbi{};
    wchar_t path[MAX_PATH];
    if (VirtualQuery(p, &mbi, sizeof(mbi)) && mbi.Type == MEM_IMAGE && mbi.AllocationBase &&
        K32GetMappedFileNameW(GetCurrentProcess(), mbi.AllocationBase, path, MAX_PATH)) {
        const wchar_t* base = wcsrchr(path, L'\\');
        snprintf(out, n, "%ls+0x%llx", base ? base + 1 : path,
                 (unsigned long long)((const char*)p - (const char*)mbi.AllocationBase));
    } else {
        snprintf(out, n, "%p", p);
    }
}

static LONG CALLBACK CrashLogger(EXCEPTION_POINTERS* ep)
{
    DWORD code = ep->ExceptionRecord->ExceptionCode;
    if (code != EXCEPTION_ACCESS_VIOLATION && code != EXCEPTION_ILLEGAL_INSTRUCTION &&
        code != EXCEPTION_STACK_OVERFLOW && code != EXCEPTION_INT_DIVIDE_BY_ZERO && code != STATUS_HEAP_CORRUPTION &&
        code != EXCEPTION_PRIV_INSTRUCTION)
        return EXCEPTION_CONTINUE_SEARCH;
    static LONG count;
    if (InterlockedIncrement(&count) > 6) return EXCEPTION_CONTINUE_SEARCH;
    char where[300];
    DescribeAddress(ep->ExceptionRecord->ExceptionAddress, where, sizeof(where));
    ULONG_PTR op = ep->ExceptionRecord->NumberParameters >= 2 ? ep->ExceptionRecord->ExceptionInformation[0] : 0;
    ULONG_PTR addr = ep->ExceptionRecord->NumberParameters >= 2 ? ep->ExceptionRecord->ExceptionInformation[1] : 0;
    Log("EXCEPTION 0x%08lx at %s (op %llu, address %p)", code, where, (unsigned long long)op, (void*)addr);
#if defined(_M_X64)
    const ULONG_PTR* sp = (const ULONG_PTR*)ep->ContextRecord->Rsp;
#else
    const ULONG_PTR* sp = (const ULONG_PTR*)ep->ContextRecord->Esp;
#endif
    int shown = 0;
    for (int i = 0; i < 512 && shown < 16; ++i) {
        ULONG_PTR v = 0;
        if (IsBadReadPtr(sp + i, sizeof(v))) break;
        v = sp[i];
        HMODULE m = nullptr;
        if (v < 0x10000 || !GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                               (LPCWSTR)v, &m) || !m)
            continue;
        DescribeAddress((const void*)v, where, sizeof(where));
        Log("    stack[%d] %s", i, where);
        ++shown;
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

// A DPI-unaware game on a scaled display (200% on the 4K SR panel) renders at half size and gets
// stretched by Windows, which ruins SR weaving (it must be pixel exact). opengl32.dll is a static
// import, so this runs before the game creates its window.
static void MakeDpiAware()
{
    using SetCtxFn = BOOL(WINAPI*)(HANDLE);
    auto setCtx = (SetCtxFn)GetProcAddress(GetModuleHandleW(L"user32.dll"), "SetProcessDpiAwarenessContext");
    BOOL ok = setCtx ? setCtx((HANDLE)-4 /* DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2 */) : SetProcessDPIAware();
    DWORD err = ok ? 0 : GetLastError();
    Log("DPI awareness: %s", ok ? "per-monitor (game renders at native resolution)"
                            : err == ERROR_ACCESS_DENIED ? "already set by the game" : "could not be changed");
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH) {
        g_self = inst;
        DisableThreadLibraryCalls(inst);
        wchar_t exe[MAX_PATH];
        GetModuleFileNameW(nullptr, exe, MAX_PATH);
        Log("OGL3D " OGL3D_VERSION " (opengl32 proxy, %s) loaded into %ls", sizeof(void*) == 8 ? "x64" : "x86", exe);
        LoadConfig();
        if (g_cfg.logCrashes) AddVectoredExceptionHandler(1, CrashLogger);
        // upscale: the game stays DPI-unaware at its own resolution (readable UI) and must not
        // switch the panel's display mode; we present at native resolution ourselves
        if (g_cfg.upscale) InstallDisplayModeHooks();
        else if (g_cfg.dpiAware) MakeDpiAware();
    }
    return TRUE;
}

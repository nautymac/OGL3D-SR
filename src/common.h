#pragma once
#include "glapi.h"
#include <string>
#include <unordered_map>

// Log to OGL3D.log next to the DLL; every line is flushed (a crash must not eat the tail).
void Log(const char* fmt, ...);
void DescribeAddress(const void* p, char* out, size_t n);   // "module+0xoffset"
#define LOGV(...) do { if (g_cfg.logLevel >= 2) Log(__VA_ARGS__); } while (0)

struct Hotkey {
    int vk = 0;
    bool ctrl = false, shift = false, alt = false;
};

// Per-shader handling, chosen in hunting mode and saved under [ShaderOverride] (program hash = mode).
enum ShaderMode { kModeStereo = 0, kModeMono = 1, kModeSkip = 2, kModeCount };

struct Config {
    // [Stereo]
    bool enable = true;
    float separation = 50.0f;     // percent of max_separation
    float maxSeparation = 0.1f;   // clip-space x offset per eye at infinity, at 100%
    float convergence = 3.0f;     // view depth (clip w) that lands on the screen plane
    bool autoConvergence = false; // "convergence = auto": follow the scene (nearest content at the screen)
    float autoPopout = 0.0f;      // auto: how far the nearest content may come out, in % of separation
    float autoSpeed = 1.0f;       // auto: how quickly convergence follows the scene (1 = default)
    float autoStart = 0.0f;       // auto: last settled value, remembered so the next launch starts there
    bool swapEyes = false;
    int output = 4;               // 0 = half SBS, 1 = top-and-bottom, 2 = left only, 3 = right only, 4 = SR (falls back to SBS)
    bool monoW1 = true;           // keep geometry with clip w == 1 (2D, HUD, full-screen passes) at screen depth
    bool fixedFunction = true;    // per-eye projection matrix for draws without an injected shader
    float orthoDepth = 0.0f;      // != 0: flat (w == 1) geometry gets depth from its clip z, scaled by this
    float orthoConvergence = 0.0f;   // clip z that sits on the screen plane in ortho depth mode
    // [Inject]
    bool inject = true;
    bool blockProgramBinary = true;
    bool dumpShaders = false;
    // [Targets]
    int minTargetWidth = 64;
    float aspectTolerance = 0.03f;
    // [Overlay]
    bool showValues = false;      // keep separation/convergence on screen (Ctrl+F1)
    // [Display]
    bool dpiAware = true;         // make the game DPI aware so it renders at native resolution
    bool upscale = false;         // game stays DPI-unaware at its own resolution; we present natively (present.cpp)
    bool upscaleReportMode = true;   // upscale: report a blocked display mode change as the current mode
    bool virtualBackbuffer = true;   // the game draws into an off-screen copy of its back buffer; the
                                     // window only ever gets finished frames
    int swapInterval = -1;        // >= 0: replaces the game's wglSwapIntervalEXT value (0 = no vsync)
    // [Keys]
    Hotkey keySepDown{VK_F3, true}, keySepUp{VK_F4, true};
    Hotkey keyConvDown{VK_F5, true}, keyConvUp{VK_F6, true};
    Hotkey keySave{VK_F7, true}, keyToggle{'T', true}, keyDump{VK_F12, true};
    Hotkey keyWeave{'W', true, false, true};   // Ctrl+Alt+W: SR weaving on/off (off shows SBS)
    Hotkey keyShowValues{VK_F1, true};
    Hotkey keyAutoConv{VK_F2, true};
    Hotkey keyHunt{VK_F9, true}, keyHuntPrev{VK_F10, true}, keyHuntNext{VK_F11, true}, keyHuntMode{VK_F8, true};
    // [SR]
    int srLatencyFrames = 1;
    bool srWeaverHwnd = true;
    // [ShaderOverride]
    std::unordered_map<uint32_t, int> overrides;   // program hash -> ShaderMode
    // [Debug]
    int logLevel = 1;
    std::string dumpFrames;       // comma separated frame numbers to save as BMP
    int statsEvery = 600;         // log per-frame counters every N frames (0 = never)
    bool logCrashes = true;       // log first-chance faults with module offsets and stack
    bool flickerProbe = false;    // log frames whose eye images jump against both neighbours
    bool markBackbuffer = false;  // paint the back buffer magenta between the left-eye copy and composition
    int dumpDark = 0;             // save both eyes (up to N times) when the picture turns unmeasurably flat/dark
};
extern Config g_cfg;

std::wstring ModuleDir();         // folder of this DLL, with trailing backslash
std::wstring IniPath();
void LoadConfig();
void SaveStereoValues();          // [Stereo] separation/convergence and [ShaderOverride]
void SaveAutoStart(float conv);   // [Stereo] auto_start
const wchar_t* ModeName(int mode);
bool KoreanUi();                  // overlay language follows the Windows UI language

HMODULE RealOpenGL();             // System32 opengl32.dll
void LoadRealExtensions();        // fills the S3D_EXT_FUNCS part of R (needs a current context)

// stereo.cpp
PROC HookForName(const char* name, PROC real);   // our replacement for an extension, or real
void OnMakeCurrent(HDC dc, HGLRC rc);
void OnDeleteContext(HGLRC rc);
bool OnSwapBuffers(HDC dc);       // true: the frame was presented in our window, skip the game's swap

// present.cpp (upscale mode)
void InstallDisplayModeHooks();
bool PresentFrame(HWND game, GLuint sbsFbo, int eyeW, int eyeH, bool weave);
void PresentShutdown();

// srout.cpp
bool SrAvailable(HWND hwnd);
void SrSetLens(bool on);
bool SrWeave(GLuint sbsTex, int eyeW, int eyeH);
bool SrEyePositions(float left[3], float right[3]);

// osd.cpp
void OsdMessage(const std::wstring& text, double seconds);
void OsdStatus(const std::wstring& text);
bool OsdActive();
void OsdRender(GLuint fbo, int vx, int vy, int vw, int vh, int W, int H, bool compat);

// inject.cpp
bool InjectVertexShader(const std::string& src, std::string& out);

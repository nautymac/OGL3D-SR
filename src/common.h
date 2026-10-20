#pragma once
#include "glapi.h"
#include <string>
#include <unordered_map>
#include <vector>

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
    // [Flat] layered depth for flat 2D games (geometry shader behind the game's vertex shaders)
    bool flatEnable = false;
    int flatRule = 0;             // 0 = feet (adventure: floor slope, sprites at their bottom edge), 1 = order (layers by draw order)
    float flatTop = 1.0f;         // depth at the top of the screen (bottom = 0), in units of separation
    float flatZero = 0.0f;        // depth that sits on the screen plane
    float flatPlane = 1.2f;       // a quad at least this wide (NDC, 2 = screen) is a plane (background layer, overlay)
    float flatPlaneHeight = 0.0f; // ... and at least this high (AGS: 1.8, so wide walk-behinds stay sprites)
    float flatPlaneTall = 0.0f;   // a quad at least this high is a plane whatever its width (0 = off; Samorost 2: pictures in pieces)
    float flatSpriteMin = -0.25f; // sprites never come nearer than this depth
    int flatScreenBatch = 1;      // draws from this scissor/viewport batch on stay at screen depth (0 = off)
    float flatCacheScale = 0.0f;  // offset scale inside off-screen layer caches (0 = auto: scissored part of the viewport)
    bool flatGroupByDraw = false; // order rule: every draw call is a layer (Unity), not a run of draws with the same program and textures
    float flatOrderCurve = 1.0f;  // order rule: depth = 1 - place^curve; < 1 gives the first (far) layers more depth
    // silhouette depth: in these programs' pictures, dark pixels come nearer than the picture
    std::vector<uint32_t> silhouette;   // program hashes
    std::string silUv = "texcoordOut", silTex = "texture1";   // fragment shader varying and sampler of the picture
    float silDepth = 0.3f;        // how much nearer, as a part of depth_top
    float silThreshold = 0.15f;   // brightness (max of r, g, b) below which a pixel counts as dark
    // [Inject]
    bool inject = true;
    bool blockProgramBinary = true;
    bool dumpShaders = false;
    int cgGlsl = 0;               // NVIDIA Cg games: make cgGLGetLatestProfile answer GLSL (1); off by default (Rocketbirds turned white)
    bool arbPrograms = true;      // rewrite ARB/NV assembly vertex programs (Cg arbvp1) for the stereo offset
    std::wstring addArgs;         // appended to the game's command line (Machinarium: -ogl picks its OpenGL renderer)
    // [Targets]
    int minTargetWidth = 64;
    float aspectTolerance = 0.03f;
    // [Overlay]
    bool showValues = false;      // keep separation/convergence on screen (Ctrl+F1)
    bool cursor = true;           // SBS / TAB output: the mouse cursor is drawn into each eye (cursor.cpp)
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
    bool swallowKeys = true;      // the game does not receive the keyboard messages of our hotkeys
    // [SR]
    int srLatencyFrames = 1;
    bool srWeaverHwnd = true;
    // [ShaderOverride]
    std::unordered_map<uint32_t, int> overrides;   // program hash -> ShaderMode
    // [Debug]
    int logLevel = 1;
    std::string dumpFrames;       // comma separated frame numbers to save as BMP
    std::string traceFrames;      // comma separated frame numbers whose draw calls are logged
    bool traceSteps = false;      // traced frames also save each draw's target as an image
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
void IniWrittenByUs();            // after our own save: the ini watcher does not re-read it
const wchar_t* ModeName(int mode);
bool KoreanUi();                  // overlay language follows the Windows UI language

HMODULE RealOpenGL();             // System32 opengl32.dll
void LoadRealExtensions();        // fills the S3D_EXT_FUNCS part of R (needs a current context)

// stereo.cpp
PROC HookForName(const char* name, PROC real);   // our replacement for an extension, or real
void OnMakeCurrent(HDC dc, HGLRC rc);
void OnDeleteContext(HGLRC rc);
bool OnSwapBuffers(HDC dc);       // true: the frame was presented in our window, skip the game's swap

// cghook.cpp
void InstallCgGlslHooks();        // [Inject] cg_glsl: Cg programs compile to GLSL
bool PatchModuleImport(HMODULE m, const char* dll, const char* name, void* hook);
void InstallCommandLineArgs();    // [Inject] add_args: arguments appended to the game's command line

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
enum OverlayBlend { kBlendStraight, kBlendPremultiplied, kBlendInvert };
void OverlayUpload(GLuint& tex, int w, int h, const uint8_t* rgba);
bool OverlayDraw(GLuint fbo, int vx, int vy, int vw, int vh, bool compat, GLuint tex, float x0, float y0, float x1,
                 float y1, OverlayBlend blend);

// cursor.cpp
void CursorPrepare(HWND wnd, int W, int H);   // per composed frame, before drawing
bool CursorActive();                          // the cursor is drawn this frame
void CursorRender(GLuint fbo, int vx, int vy, int vw, int vh, int W, int H, bool compat);
void CursorHide(HWND wnd, bool hide);         // per composed frame: this output gets the cursor drawn in

// inject.cpp
bool InjectVertexShader(const std::string& src, std::string& out);
bool InjectArbVertexProgram(const std::string& src, int envIndex, std::string& out);
bool InjectSilhouette(const std::string& src, const std::string& uv, const std::string& tex, std::string& out,
                      std::string& why);
struct VsOutput {
    std::string quals, type, name, arr;   // arr: "[8]" for array outputs
};
struct VsBlock {
    std::string layout, name, inst, body;   // out <name> { <body> } <inst>;
    std::vector<std::string> members;
};
struct VsInfo {
    int version = 110;
    bool core = false;
    std::vector<VsOutput> outs;
    std::vector<VsBlock> blocks;
    unsigned builtins = 0;        // compatibility outputs written (gl_FrontColor ...)
    int texCoords = 0, clipDistances = 0;
    std::string renamed;          // injected source with the outputs renamed s3d_v_<name>
};
bool AnalyzeVertexShader(const std::string& injected, VsInfo& info, std::string& why);
std::string BuildFlatGs(const VsInfo& vs, bool compatContext);

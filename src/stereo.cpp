// Stereo engine: every draw into a "stereo" render target is issued twice, once per eye.
//
//  * Left eye  = the game's own render targets.
//  * Right eye = a shadow FBO per game FBO whose textures/renderbuffers are same-sized clones;
//                the default framebuffer's right eye is an offscreen FBO.
//  * While drawing the right eye, any bound texture that has a clone is swapped for the clone,
//    so post-processing chains stay per eye.
//  * The eye offset comes from the injected vertex-shader uniform (inject.cpp), or, for
//    fixed-function / uninjected programs in compatibility contexts, from a per-eye projection
//    matrix.
//  * A target is stereo when it has the window's aspect ratio (shadow maps, LUTs etc. stay mono).
//  * SwapBuffers composes both eyes into the back buffer (half SBS by default).
#include "common.h"
#include <algorithm>
#include <array>
#include <climits>
#include <cmath>
#include <initializer_list>
#include <iterator>
#include <map>
#include <cstdio>
#include <cstring>
#include <condition_variable>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace {

enum { kSlot2D, kSlotRect, kSlot2DMS, kSlotCount };
const GLenum kSlotTarget[kSlotCount] = {GL_TEXTURE_2D, GL_TEXTURE_RECTANGLE, GL_TEXTURE_2D_MULTISAMPLE};

int SlotOf(GLenum target)
{
    switch (target) {
    case GL_TEXTURE_2D: return kSlot2D;
    case GL_TEXTURE_RECTANGLE: return kSlotRect;
    case GL_TEXTURE_2D_MULTISAMPLE: return kSlot2DMS;
    }
    return -1;
}

bool IsDepthFormat(GLenum f)
{
    switch (f) {
    case GL_DEPTH_COMPONENT: case GL_DEPTH_COMPONENT16: case GL_DEPTH_COMPONENT24:
    case GL_DEPTH_COMPONENT32: case GL_DEPTH_COMPONENT32F:
        return true;
    }
    return false;
}

bool IsDepthStencilFormat(GLenum f)
{
    return f == GL_DEPTH_STENCIL || f == GL_DEPTH24_STENCIL8 || f == GL_DEPTH32F_STENCIL8;
}

struct TexInfo {
    GLenum target = 0;
    GLsizei w = 0, h = 0;
    GLenum ifmt = 0, fmt = GL_RGBA, type = GL_UNSIGNED_BYTE;
    GLsizei samples = 0;
    GLboolean fixedLoc = 1;
    int levels = 1;           // mip levels the game allocated
    bool immutable = false;   // glTexStorage2D
    GLuint right = 0;         // right-eye clone
    // glTextureView (WineD3D's shader resource views): a view of another texture's storage; its
    // right-eye twin is the same view of that texture's clone
    GLuint viewOf = 0;
    GLenum viewFmt = 0;
    GLuint minLevel = 0, numLevels = 1, minLayer = 0, numLayers = 1;
};

struct RbInfo {
    GLsizei w = 0, h = 0, samples = 0;
    GLenum ifmt = 0;
    GLuint right = 0;
};

struct ProgInfo {
    GLint loc = -1;           // s3d_P, -1 when the program was not injected
    GLint locQ = -1;          // s3d_Q (ortho depth)
    float eye = NAN;          // last value written, to skip redundant uniform calls
    unsigned ver = 0;
    uint32_t hash = 0;        // of the attached shaders' original sources ([ShaderOverride] key)
    int mode = kModeStereo;
    bool gs = false;          // flat depth geometry shader attached ([Flat])
    GLint locF = -1, locD = -1;
    unsigned fVer = 0;
    float lastD[4] = {NAN, NAN, NAN, NAN};   // s3d_D last written
    GLint locS = -1;          // s3d_S: silhouette depth in the fragment shader ([Flat] silhouette)
    float lastS = NAN;
    // texture units its samplers read (SampledUnits); read once at its first draw, after the engine
    // has set the sampler uniforms
    bool samplersKnown = false;
    std::vector<GLint> samplerLocs;
    bool unitsRead = false;
    uint32_t units = ~0u;
};

struct Att {
    GLenum kind = 0;          // 0, GL_TEXTURE, GL_RENDERBUFFER
    GLuint id = 0;
    GLenum textarget = 0;
    GLint level = 0;
    GLint layer = -1;
    bool operator==(const Att& o) const
    {
        return kind == o.kind && id == o.id && textarget == o.textarget && level == o.level && layer == o.layer;
    }
};

constexpr int kAttDepth = kMaxColorAttachments, kAttStencil = kMaxColorAttachments + 1, kAttCount = kMaxColorAttachments + 2;

struct FboInfo {
    Att att[kAttCount];
    GLenum drawBufs[kMaxColorAttachments] = {GL_COLOR_ATTACHMENT0};
    GLsizei nDrawBufs = 1;
    GLenum readBuf = GL_COLOR_ATTACHMENT0;
    GLuint right = 0;
    bool dirty = true;
    bool stereo = false;
    // flat depth: an off-screen layer cache (not the window's aspect) whose image is drawn into the
    // stereo scene; it is stereo so the layers drawn into it keep their own depths
    bool promoted = false;
    unsigned stamp = 0;       // Ctx::stamp the classification was made for
    int w = 0, h = 0;
};

// flat depth, rule = order: one draw of the frame, for the composition order of the next frame
struct FlatEv {
    GLuint fb = 0;
    uint64_t key = 0;         // layer group; 0 = a composite (draws a stereo render target)
    GLuint prog = 0, tex = 0;
    GLsizei count = -1;
    GLint sc[4] = {-1, -1, -1, -1};   // scissor box, -1 = off
    GLuint src[4] = {};       // render targets (game framebuffers) it samples
    int nsrc = 0;
    bool clear = false;       // a clear of fb (no draw)
};

struct EyeState {
    bool matrix = false;
    GLint mode = 0;
    GLfloat proj[16];
};

struct Ctx {
    HGLRC rc = nullptr;
    bool compat = true;
    bool isMain = false;
    std::unordered_map<GLuint, FboInfo> fbo;
    GLuint drawFb = 0, readFb = 0, rbBinding = 0, program = 0;
    GLuint activeUnit = 0;
    int maxUnit = 0;
    GLuint bound[kMaxUnits][kSlotCount] = {};
    GLenum listMode = 0;      // the game's glNewList mode while it compiles a list
    GLuint arbVp = 0;         // bound ARB vertex program
    GLuint arbFp = 0;         // bound ARB fragment program
    bool arbActive = false;   // the draw being issued runs one of our rewritten ARB vertex programs
    GLint arbEnv = -1;        // program.env index the rewrite reads
    // scissor/viewport batches of the current frame (flat depth: later batches are UI)
    int batch = 0;
    unsigned drawsInBatch = 0;
    GLuint batchFb = 0;       // framebuffer the current batch draws into
    bool batchPending = false;
    // flat depth, rule = order: groups of consecutive draws with one program, in drawing order
    uint64_t groupId = 0;             // (program, texture) of the current group
    std::vector<uint64_t> groupIds;
    size_t frameGroups = 0, lastGroupCount = 0;
    uint64_t groupKey = 0, groupKeyLast = 0;
    unsigned groupKeyFrame = 0;
    struct KeyDepth {
        float depth;          // smoothed 0 (last drawn, near) .. 1 (first, far)
        unsigned frame;       // last frame it was drawn in
    };
    std::unordered_map<uint64_t, KeyDepth> keyDepth;
    float orderDepth = 0.0f;  // of the draw being issued
    float orderSlot = 0.0f;   // depth between neighbouring groups
    float drawPrims = 0.0f;   // triangles in the draw being issued (0 = unknown)
    std::vector<FlatEv> flatEvents;                    // this frame's draws
    struct FfBox {                                     // a fixed-function draw's extent (NDC) and layer
        GLuint fb;
        float lo[2], hi[2];
        uint64_t key;
    };
    std::vector<FfBox> ffBoxes;                        // this frame's, in drawing order
    struct Picture {                                   // a textured fixed-function draw and its layer (FfContinues)
        GLuint fb, tex;
        float m[6];
        uint64_t key;
        size_t box;                                    // its entry in ffBoxes
        int kind;                                      // feet rule: its depth (FeetDepthForDraw)
        float depth;
    };
    std::vector<Picture> pictures;                     // this frame's
    // feet rule, fixed function (FeetDepthForDraw): the draw being issued is 0 at the screen, 1 at ffDepth,
    // 2 on the floor slope
    int ffKind = 0;
    float ffDepth = 0.0f;
    int ffSprites = 0;                                 // draws this frame that stand on the floor
    bool ffRunOn = false;                              // the object being drawn (parts stacked on each other)
    GLuint ffRunFb = 0;
    float ffRun[4] = {};                               // its box: lo x, lo y, hi x, hi y (NDC)
    float ffRunCap = 0.0f;                             // nearest depth of the objects it was drawn over
    struct FfObj {                                     // objects drawn this frame (their box and depth)
        GLuint fb;
        float box[4];
        float depth;
    };
    std::vector<FfObj> ffObjects;
    std::vector<std::array<float, 4>> ffOverlays;      // later screen-wide pictures (UI): what lies in them stays there
    std::unordered_map<GLuint, GLuint> texCopiedFrom;  // texture -> texture last glCopyImageSubData'd into it
    std::unordered_map<uint64_t, size_t> layoutPos;    // group key -> place in the last frame's composed picture
    size_t layoutCount = 0;                            // places in it (0 = far)
    GLuint layoutLastFb = 0;                           // target and place of the last known group drawn
    long long layoutLastG = -1;                        // in this frame (for groups new to the picture)
    // window / default framebuffer
    int winW = 0, winH = 0;
    unsigned stamp = 1;       // bumped on resize and texture/renderbuffer reallocation
    GLuint rightDefFbo = 0, rightDefColor = 0, rightDefDepth = 0;
    int rightDefW = 0, rightDefH = 0;
    GLuint leftCopyFbo = 0, leftCopyTex = 0;
    int leftCopyW = 0, leftCopyH = 0;
    // Virtual back buffer ([Display] virtual_backbuffer): the game's framebuffer 0 is this FBO, so the
    // window's real back buffer only ever holds finished frames. Some windows let the compositor show
    // the back buffer while it is being drawn (Broken Age flashed half-drawn and un-woven frames).
    GLuint vbbFbo = 0, vbbColor = 0, vbbDepth = 0;
    int vbbW = 0, vbbH = 0;
    bool vbbLive = false;     // the game's framebuffer 0 is redirected to vbbFbo
    bool vbbFailed = false;
    GLenum fb0DrawBuf = GL_BACK, fb0ReadBuf = GL_BACK;   // what the game selected for framebuffer 0
    GLuint leftSrc = 0;       // left eye of the last composed frame (vbbFbo or leftCopyFbo)
    GLuint sbsFbo = 0, sbsTex = 0;   // SR weaver input (both eyes side by side, full width each)
    int sbsW = 0, sbsH = 0;
    // Did the game render into the back buffer since the last SwapBuffers? Some games (Broken Age)
    // swap without drawing; the back buffer then holds an old composed frame, not the left eye.
    bool fb0Touched = false;
    bool leftCopyValid = false;
    bool lastWoven = false;   // the previous Compose went through the SR weaver
    // the eye images in leftCopy / rightDef: a new one each time the game draws a frame, and the
    // convergence / separation it was rendered with (auto convergence measures only new images)
    unsigned imageSerial = 0;
    float imageConv = NAN, imageSep = NAN;
    // immediate mode: recorded into a display list for the right eye
    GLuint immList = 0;
    GLuint immRight = 0;
    EyeState immEye;
    GLfloat immColor[4], immNormal[4], immTex[4];
};

// The real framebuffer behind a game framebuffer name (0 = its back buffer, possibly virtual).
GLuint RealFb(const Ctx* c, GLuint fb)
{
    return fb == 0 && c->vbbLive ? c->vbbFbo : fb;
}

// Default-framebuffer buffer names mean the virtual back buffer's colour attachment.
GLenum VbbBuffer(GLenum mode)
{
    switch (mode) {
    case GL_BACK:
    case GL_BACK_LEFT:
    case GL_FRONT:
    case GL_FRONT_LEFT:
    case GL_FRONT_AND_BACK:
    case GL_LEFT:
        return GL_COLOR_ATTACHMENT0;
    default:
        return GL_NONE;   // right / aux buffers do not exist off-screen
    }
}

GLuint LeftSrc(const Ctx* c)
{
    return c->leftSrc ? c->leftSrc : c->leftCopyFbo;
}

struct Stats {
    unsigned stereoDraws, monoDraws, clears, blits, copies, immediate, lists, uniformSets, matrixDraws, flushes, finishes;
};

std::recursive_mutex g_lock;
std::unordered_map<GLuint, TexInfo> g_tex;
std::unordered_map<GLuint, RbInfo> g_rb;
std::unordered_map<GLuint, ProgInfo> g_prog;
std::unordered_map<GLuint, std::string> g_injectedSrc;   // original source of rewritten shaders
std::unordered_map<GLuint, uint32_t> g_shaderHash;       // shader object -> hash of its original source
std::unordered_map<HGLRC, Ctx*> g_ctx;
thread_local Ctx* t_ctx;
thread_local GLenum t_drawMode = 0x0004;   // primitive mode of the draw being issued
thread_local GLsizei t_drawCount = -1;     // its vertex/index count (-1 = unknown)
thread_local bool t_drawArrays = false;    // it is glDrawArrays / glDrawElements (vertices the wrapper can read)
thread_local GLint t_drawFirst = 0;        // glDrawArrays' first vertex
thread_local GLenum t_drawIndexType = 0;   // glDrawElements' index type (0 = glDrawArrays) and indices
thread_local const void* t_drawIndices = nullptr;
thread_local bool t_ownShaders;            // the SR runtime is creating its shaders: leave them alone
extern bool g_traceNow;
extern unsigned g_traceDraw;

bool SrAvailableOwn(HWND hwnd)
{
    t_ownShaders = true;
    bool ok = SrAvailable(hwnd);
    t_ownShaders = false;
    return ok;
}
std::unordered_map<GLuint, VsInfo> g_vsInfo;       // vertex shaders whose outputs were renamed for the flat GS
std::unordered_map<GLuint, std::string> g_vsPlain; // ... and their injected source without renaming
// generated geometry shader (context, source) -> shader object. Per context: WineD3D deletes its first
// contexts and makes an unshared one, where the same names belong to other objects.
std::map<std::pair<HGLRC, std::string>, GLuint> g_flatGs;
std::unordered_set<GLuint> g_vsBad;                // vertex shaders flat depth could not analyse
std::unordered_set<GLuint> g_arbVp;                // ARB vertex programs holding our stereo tail
std::unordered_map<GLuint, uint32_t> g_arbFpUnits; // ARB fragment program -> texture units it reads
#ifndef GL_VERTEX_PROGRAM_ARB
#define GL_VERTEX_PROGRAM_ARB 0x8620
#endif
#ifndef GL_FRAGMENT_PROGRAM_ARB
#define GL_FRAGMENT_PROGRAM_ARB 0x8804
#endif
#ifndef GL_ACTIVE_TEXTURE
#define GL_ACTIVE_TEXTURE 0x84E0
#endif
#ifndef GL_PROGRAM_FORMAT_ASCII_ARB
#define GL_PROGRAM_FORMAT_ASCII_ARB 0x8875
#endif
#ifndef GL_PROGRAM_ERROR_POSITION_ARB
#define GL_PROGRAM_ERROR_POSITION_ARB 0x864B
#endif
#ifndef GL_PROGRAM_ERROR_STRING_ARB
#define GL_PROGRAM_ERROR_STRING_ARB 0x8874
#endif
#ifndef GL_MAX_PROGRAM_ENV_PARAMETERS_ARB
#define GL_MAX_PROGRAM_ENV_PARAMETERS_ARB 0x88B5
#endif

// Outputs of several vertex shader pieces of one program, each name once.
void MergeVsInfo(VsInfo& into, const VsInfo& add)
{
    into.version = std::max(into.version, add.version);
    into.core = into.core && add.core;
    for (const VsOutput& o : add.outs) {
        bool have = false;
        for (const VsOutput& e : into.outs) have |= e.name == o.name;
        if (!have) into.outs.push_back(o);
    }
    for (const VsBlock& b : add.blocks) {
        bool have = false;
        for (const VsBlock& e : into.blocks) have |= e.name == b.name;
        if (!have) into.blocks.push_back(b);
    }
    into.builtins |= add.builtins;
    into.texCoords = std::max(into.texCoords, add.texCoords);
    into.clipDistances = std::max(into.clipDistances, add.clipDistances);
}
HGLRC g_mainRc;
unsigned g_paramVer = 1;
unsigned g_frame;
Stats g_stats;
unsigned g_injectedCount, g_injectFailCount, g_blockedBinaries;

// Hunting: walk through the programs drawn in the last frame; the selected one is hidden so it
// can be recognised, and its mode (stereo / mono / skip) can be changed and saved.
bool g_hunting;
std::vector<GLuint> g_frameProgs;   // programs drawn this frame, in first-use order
std::vector<GLuint> g_huntList;     // ... in the previous frame
int g_huntIndex;
GLuint g_huntSel;

float SepValue() { return g_cfg.separation * 0.01f * g_cfg.maxSeparation; }

uint32_t Fnv1a(const void* data, size_t n, uint32_t h = 2166136261u)
{
    const uint8_t* p = (const uint8_t*)data;
    for (size_t i = 0; i < n; ++i) h = (h ^ p[i]) * 16777619u;
    return h;
}

bool Active(Ctx* c)
{
    return c && c->isMain && g_cfg.enable && c->winW > 0 && c->listMode == 0;
}

void BumpStamp()
{
    for (auto& kv : g_ctx) kv.second->stamp++;
}

// ---------------------------------------------------------------------------------------------
// texture / renderbuffer clones

// Fills ti from the driver for a texture we did not see being allocated.
bool QueryTex(Ctx* c, GLuint id, GLenum target, TexInfo& ti)
{
    int slot = SlotOf(target);
    if (slot < 0) return false;
    R.BindTexture(target, id);
    GLint w = 0, h = 0, f = 0, s = 0;
    R.GetTexLevelParameteriv(target, 0, GL_TEXTURE_WIDTH, &w);
    R.GetTexLevelParameteriv(target, 0, GL_TEXTURE_HEIGHT, &h);
    R.GetTexLevelParameteriv(target, 0, GL_TEXTURE_INTERNAL_FORMAT, &f);
    if (target == GL_TEXTURE_2D_MULTISAMPLE) R.GetTexLevelParameteriv(target, 0, GL_TEXTURE_SAMPLES, &s);
    GLint imm = 0, immLevels = 0;
    if (target != GL_TEXTURE_2D_MULTISAMPLE) {
        R.GetTexParameteriv(target, GL_TEXTURE_IMMUTABLE_FORMAT, &imm);
        if (imm) R.GetTexParameteriv(target, GL_TEXTURE_IMMUTABLE_LEVELS, &immLevels);
    }
    R.BindTexture(target, c->bound[c->activeUnit][slot]);
    if (w <= 0 || h <= 0) return false;
    ti.target = target;
    ti.w = w;
    ti.h = h;
    ti.ifmt = (GLenum)f;
    ti.samples = s;
    ti.immutable = imm != 0;
    if (imm && immLevels > 0) ti.levels = immLevels;
    if (IsDepthFormat(ti.ifmt)) ti.fmt = GL_DEPTH_COMPONENT, ti.type = GL_FLOAT;
    else if (IsDepthStencilFormat(ti.ifmt)) ti.fmt = GL_DEPTH_STENCIL, ti.type = GL_UNSIGNED_INT_24_8;
    LOGV("queried texture %u: %dx%d fmt 0x%x", id, w, h, f);
    return true;
}

TexInfo* FindTex(GLuint id)
{
    auto it = g_tex.find(id);
    return it == g_tex.end() ? nullptr : &it->second;
}

void AllocTexLevels(const TexInfo& ti, int firstLevel)
{
    if (ti.target == GL_TEXTURE_2D_MULTISAMPLE) {
        R.TexImage2DMultisample(ti.target, ti.samples, ti.ifmt, ti.w, ti.h, ti.fixedLoc);
        return;
    }
    for (int l = firstLevel; l < ti.levels; ++l)
        R.TexImage2D(ti.target, l, (GLint)ti.ifmt, std::max(1, ti.w >> l), std::max(1, ti.h >> l), 0, ti.fmt,
                     ti.type, nullptr);
}

// Creates the right-eye clone of a texture (same target, size, format, mips and sampling state).
GLuint TexClone(Ctx* c, GLuint id, GLenum targetHint)
{
    TexInfo* ti = FindTex(id);
    if (!ti) {
        TexInfo q;
        if (!QueryTex(c, id, targetHint, q)) return 0;
        ti = &(g_tex[id] = q);
    }
    if (ti->right) return ti->right;
    if (ti->viewOf) {
        // the same view of the original's clone
        TexInfo* oi = FindTex(ti->viewOf);
        GLuint origRight = oi ? TexClone(c, ti->viewOf, oi->target) : 0;
        if (!origRight || !R.TextureView) return 0;
        GLuint view = 0;
        R.GenTextures(1, &view);
        R.TextureView(view, ti->target, origRight, ti->viewFmt, ti->minLevel, ti->numLevels, ti->minLayer, ti->numLayers);
        ti = FindTex(id);   // TexClone above may have rehashed g_tex
        ti->right = view;
        LOGV("texture view %u (of %u) -> right view %u", id, ti->viewOf, view);
        return view;
    }
    int slot = SlotOf(ti->target);
    if (slot < 0 || ti->w <= 0) return 0;

    static const GLenum kParams[] = {GL_TEXTURE_MIN_FILTER, GL_TEXTURE_MAG_FILTER, GL_TEXTURE_WRAP_S, GL_TEXTURE_WRAP_T,
                                     GL_TEXTURE_BASE_LEVEL, GL_TEXTURE_MAX_LEVEL, GL_TEXTURE_COMPARE_MODE,
                                     GL_TEXTURE_COMPARE_FUNC};
    GLint values[8] = {};
    bool sampled = ti->target != GL_TEXTURE_2D_MULTISAMPLE;
    if (sampled) {
        R.BindTexture(ti->target, id);
        for (int i = 0; i < 8; ++i) R.GetTexParameteriv(ti->target, kParams[i], &values[i]);
    }
    GLuint clone = 0;
    R.GenTextures(1, &clone);
    R.BindTexture(ti->target, clone);
    if (ti->immutable && R.TexStorage2D) R.TexStorage2D(ti->target, ti->levels, ti->ifmt, ti->w, ti->h);
    else AllocTexLevels(*ti, 0);
    if (sampled)
        for (int i = 0; i < 8; ++i) R.TexParameteri(ti->target, kParams[i], values[i]);
    R.BindTexture(ti->target, c->bound[c->activeUnit][slot]);
    // Start from the current left content: the game may have drawn into or uploaded this texture
    // before it became stereo, and later passes would read garbage in the right eye.
    if (R.CopyImageSubData) {
        int levels = sampled ? ti->levels : 1;
        for (int l = 0; l < levels; ++l)
            R.CopyImageSubData(id, ti->target, l, 0, 0, 0, clone, ti->target, l, 0, 0, 0, std::max(1, ti->w >> l),
                               std::max(1, ti->h >> l), 1);
    }
    ti->right = clone;
    LOGV("texture %u -> right clone %u (%dx%d fmt 0x%x levels %d)", id, clone, ti->w, ti->h, ti->ifmt, ti->levels);
    return clone;
}

void DeleteTexClone(TexInfo& ti)
{
    if (ti.right) R.DeleteTextures(1, &ti.right);
    ti.right = 0;
}

GLuint RbClone(Ctx* c, GLuint id)
{
    auto it = g_rb.find(id);
    if (it == g_rb.end() || it->second.w <= 0) return 0;
    RbInfo& ri = it->second;
    if (ri.right) return ri.right;
    R.GenRenderbuffers(1, &ri.right);
    R.BindRenderbuffer(GL_RENDERBUFFER, ri.right);
    if (ri.samples > 0) R.RenderbufferStorageMultisample(GL_RENDERBUFFER, ri.samples, ri.ifmt, ri.w, ri.h);
    else R.RenderbufferStorage(GL_RENDERBUFFER, ri.ifmt, ri.w, ri.h);
    R.BindRenderbuffer(GL_RENDERBUFFER, c->rbBinding);
    if (R.CopyImageSubData)
        R.CopyImageSubData(id, GL_RENDERBUFFER, 0, 0, 0, 0, ri.right, GL_RENDERBUFFER, 0, 0, 0, 0, ri.w, ri.h, 1);
    LOGV("renderbuffer %u -> right clone %u (%dx%d fmt 0x%x)", id, ri.right, ri.w, ri.h, ri.ifmt);
    return ri.right;
}

// ---------------------------------------------------------------------------------------------
// framebuffers

GLenum AttPoint(int i)
{
    return i < kMaxColorAttachments ? GL_COLOR_ATTACHMENT0 + i : i == kAttDepth ? GL_DEPTH_ATTACHMENT : GL_STENCIL_ATTACHMENT;
}

bool AttSize(Ctx* c, const Att& a, int& w, int& h)
{
    if (a.kind == GL_TEXTURE) {
        if (a.layer >= 0) return false;   // array/3D layers are never cloned
        TexInfo* ti = FindTex(a.id);
        if (!ti) {
            TexInfo q;
            if (!QueryTex(c, a.id, a.textarget, q)) return false;
            ti = &(g_tex[a.id] = q);
        }
        if (SlotOf(ti->target) < 0) return false;
        w = std::max(1, ti->w >> a.level);
        h = std::max(1, ti->h >> a.level);
        return true;
    }
    if (a.kind == GL_RENDERBUFFER) {
        auto it = g_rb.find(a.id);
        if (it == g_rb.end()) return false;
        w = it->second.w;
        h = it->second.h;
        return w > 0;
    }
    return false;
}

bool IsStereoSize(Ctx* c, int w, int h)
{
    if (w < g_cfg.minTargetWidth || h <= 0 || c->winW <= 0 || c->winH <= 0) return false;
    float a = (float)w / (float)h, wa = (float)c->winW / (float)c->winH;
    return std::fabs(a - wa) <= g_cfg.aspectTolerance * wa;
}

GLuint EnsureRightDefault(Ctx* c)
{
    if (c->rightDefFbo && c->rightDefW == c->winW && c->rightDefH == c->winH) return c->rightDefFbo;
    if (c->rightDefFbo) {
        R.DeleteFramebuffers(1, &c->rightDefFbo);
        R.DeleteTextures(1, &c->rightDefColor);
        R.DeleteRenderbuffers(1, &c->rightDefDepth);
    }
    // Match the back buffer's encoding so GL_FRAMEBUFFER_SRGB behaves the same in both eyes.
    GLint enc = 0;
    R.BindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
    if (R.GetFramebufferAttachmentParameteriv)
        R.GetFramebufferAttachmentParameteriv(GL_DRAW_FRAMEBUFFER, GL_BACK_LEFT, GL_FRAMEBUFFER_ATTACHMENT_COLOR_ENCODING, &enc);
    GLenum colorFmt = enc == (GLint)GL_SRGB ? GL_SRGB8_ALPHA8 : GL_RGBA8;

    R.GenTextures(1, &c->rightDefColor);
    R.BindTexture(GL_TEXTURE_2D, c->rightDefColor);
    R.TexImage2D(GL_TEXTURE_2D, 0, (GLint)colorFmt, c->winW, c->winH, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    R.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    R.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    R.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
    R.BindTexture(GL_TEXTURE_2D, c->bound[c->activeUnit][kSlot2D]);
    R.GenRenderbuffers(1, &c->rightDefDepth);
    R.BindRenderbuffer(GL_RENDERBUFFER, c->rightDefDepth);
    R.RenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, c->winW, c->winH);
    R.BindRenderbuffer(GL_RENDERBUFFER, c->rbBinding);
    R.GenFramebuffers(1, &c->rightDefFbo);
    R.BindFramebuffer(GL_DRAW_FRAMEBUFFER, c->rightDefFbo);
    R.FramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, c->rightDefColor, 0);
    R.FramebufferRenderbuffer(GL_DRAW_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, c->rightDefDepth);
    GLenum st = R.CheckFramebufferStatus(GL_DRAW_FRAMEBUFFER);
    R.BindFramebuffer(GL_DRAW_FRAMEBUFFER, RealFb(c, c->drawFb));
    c->rightDefW = c->winW;
    c->rightDefH = c->winH;
    Log("right-eye back buffer %dx%d (%s) fbo %u status 0x%x", c->winW, c->winH, colorFmt == GL_SRGB8_ALPHA8 ? "sRGB" : "RGBA8",
        c->rightDefFbo, st);
    if (st != GL_FRAMEBUFFER_COMPLETE) {
        R.DeleteFramebuffers(1, &c->rightDefFbo);
        c->rightDefFbo = 0;
    }
    return c->rightDefFbo;
}

// The right-eye framebuffer for game framebuffer fb, or 0 when fb is mono.
GLuint RightFor(Ctx* c, GLuint fb)
{
    if (fb == 0) return EnsureRightDefault(c);
    FboInfo& f = c->fbo[fb];
    if (!f.dirty && f.stamp == c->stamp) return f.stereo ? f.right : 0;
    f.dirty = false;
    f.stamp = c->stamp;

    int w = 0, h = 0;
    bool sized = false;
    for (int i = 0; i < kAttCount && !sized; ++i)
        if (f.att[i].kind) sized = AttSize(c, f.att[i], w, h);
    bool stereo = sized && (IsStereoSize(c, w, h) || f.promoted);
    if (stereo != f.stereo || w != f.w || h != f.h) {
        // games that re-attach one framebuffer to other textures all the time (Rocketbirds) would fill the log
        static unsigned changes;
        if (++changes <= 300 || changes % 5000 == 0)
            Log("fbo %u: %dx%d -> %s%s", fb, w, h, stereo ? (f.promoted ? "stereo (a layer cache drawn into the stereo scene)" : "stereo") : "mono",
                changes == 300 ? " (further framebuffer changes are logged every 5000th)" : "");
    }
    f.stereo = stereo;
    f.w = w;
    f.h = h;
    if (!stereo) return 0;

    if (!f.right) R.GenFramebuffers(1, &f.right);
    R.BindFramebuffer(GL_DRAW_FRAMEBUFFER, f.right);
    bool ds = f.att[kAttDepth].kind && f.att[kAttDepth] == f.att[kAttStencil];
    for (int i = 0; i < kAttCount; ++i) {
        const Att& a = f.att[i];
        GLenum point = AttPoint(i);
        if (ds && i == kAttStencil) continue;
        if (ds && i == kAttDepth) point = GL_DEPTH_STENCIL_ATTACHMENT;
        GLuint clone = 0;
        if (a.kind == GL_TEXTURE && a.layer < 0) clone = TexClone(c, a.id, a.textarget);
        else if (a.kind == GL_RENDERBUFFER) clone = RbClone(c, a.id);
        if (a.kind == GL_TEXTURE && clone) {
            TexInfo* ti = FindTex(a.id);
            R.FramebufferTexture2D(GL_DRAW_FRAMEBUFFER, point, ti ? ti->target : a.textarget, clone, a.level);
        } else if (a.kind == GL_RENDERBUFFER && clone) {
            R.FramebufferRenderbuffer(GL_DRAW_FRAMEBUFFER, point, GL_RENDERBUFFER, clone);
        } else {
            R.FramebufferRenderbuffer(GL_DRAW_FRAMEBUFFER, point, GL_RENDERBUFFER, 0);
        }
    }
    if (R.DrawBuffers) R.DrawBuffers(f.nDrawBufs, f.drawBufs);
    else R.DrawBuffer(f.drawBufs[0]);
    GLenum st = R.CheckFramebufferStatus(GL_DRAW_FRAMEBUFFER);
    R.BindFramebuffer(GL_DRAW_FRAMEBUFFER, RealFb(c, c->drawFb));
    R.BindFramebuffer(GL_READ_FRAMEBUFFER, f.right);
    R.ReadBuffer(f.readBuf);
    R.BindFramebuffer(GL_READ_FRAMEBUFFER, RealFb(c, c->readFb));
    if (st != GL_FRAMEBUFFER_COMPLETE) {
        Log("fbo %u: right-eye copy incomplete (0x%x), drawing it mono", fb, st);
        f.stereo = false;
        return 0;
    }
    return f.right;
}

FboInfo* BoundFbo(Ctx* c, GLenum target)
{
    GLuint fb = target == GL_READ_FRAMEBUFFER ? c->readFb : c->drawFb;
    return fb ? &c->fbo[fb] : nullptr;
}

void SetAttachment(Ctx* c, GLenum target, GLenum point, const Att& a)
{
    FboInfo* f = BoundFbo(c, target);
    if (!f) return;
    if (point == GL_DEPTH_STENCIL_ATTACHMENT) {
        f->att[kAttDepth] = a;
        f->att[kAttStencil] = a;
    } else if (point == GL_DEPTH_ATTACHMENT) {
        f->att[kAttDepth] = a;
    } else if (point == GL_STENCIL_ATTACHMENT) {
        f->att[kAttStencil] = a;
    } else if (point >= GL_COLOR_ATTACHMENT0 && point < GL_COLOR_ATTACHMENT0 + kMaxColorAttachments) {
        f->att[point - GL_COLOR_ATTACHMENT0] = a;
    } else {
        return;
    }
    f->dirty = true;
}

// ---------------------------------------------------------------------------------------------
// per-eye state

ProgInfo* FindProg(GLuint p)
{
    auto it = g_prog.find(p);
    return it == g_prog.end() ? nullptr : &it->second;
}

// Flat depth, rule = order: 2D engines draw back to front. Consecutive draws with the same program
// and texture form a layer group (a background atlas, a tilemap batch, the sprites, the HUD);
// groups get depth by their position in the frame (first = far, last = screen). Each group is
// smoothed under the key (program, texture, how many such groups came before it), so depth does not
// jump when effects add or drop a group.
bool SamplesStereoTarget(Ctx* c);

// The game framebuffers that render into texture tex (a color attachment); WineD3D may have
// several with the same colour texture (other depth buffers), and attaches views of it (render
// target views), so attachments count by the texture they view.
int FbosOfTexture(Ctx* c, GLuint tex, GLuint* out, int max)
{
    int n = 0;
    for (auto& kv : c->fbo)
        for (int i = 0; i < kMaxColorAttachments && n < max; ++i) {
            const Att& a = kv.second.att[i];
            if (a.kind != GL_TEXTURE) continue;
            GLuint id = a.id;
            if (id != tex) {
                TexInfo* ti = FindTex(id);
                if (ti && ti->viewOf) id = ti->viewOf;
            }
            if (id == tex) {
                out[n++] = kv.first;
                break;
            }
        }
    return n;
}

// Texture units the current program samples, one bit each (all when unknown). WineD3D leaves the
// render targets of earlier passes bound on units the shader does not read: counted, they made
// ordinary sprites look like composite passes (no depth) in Huntdown.
uint32_t SampledUnits(Ctx* c)
{
    if (!c->program && c->arbFp && R.IsEnabled(GL_FRAGMENT_PROGRAM_ARB)) {   // ARB fragment program: its texture[n]
        auto it = g_arbFpUnits.find(c->arbFp);
        if (it != g_arbFpUnits.end()) return it->second;
    }
    if (!c->program && c->compat) {
        // fixed function: the units with 2D texturing enabled (render targets left on other units are not read)
        // (GL's active unit is read back: a game may switch it through a path we do not track)
        uint32_t m = 0;
        GLint active = GL_TEXTURE0;
        R.GetIntegerv(GL_ACTIVE_TEXTURE, &active);
        const int last = std::min(std::max(c->maxUnit, (int)(active - GL_TEXTURE0)), 7);
        for (int u = 0; u <= last; ++u) {
            if (GL_TEXTURE0 + u != active) R.ActiveTexture(GL_TEXTURE0 + u);
            if (R.IsEnabled(GL_TEXTURE_2D)) m |= 1u << u;
        }
        if (last > 0 || active != GL_TEXTURE0) R.ActiveTexture(active);
        return m;
    }
    ProgInfo* pi = c->program ? FindProg(c->program) : nullptr;
    if (!pi || !pi->samplersKnown || !R.GetUniformiv) return ~0u;
    if (!pi->unitsRead) {
        uint32_t m = 0;
        for (GLint loc : pi->samplerLocs) {
            GLint u = -1;
            R.GetUniformiv(c->program, loc, &u);
            if (u >= 0 && u < 32) m |= 1u << u;
        }
        pi->units = m;
        pi->unitsRead = true;
    }
    return pi->units;
}

static bool UnitIn(uint32_t units, int u) { return u >= 32 ? units == ~0u : ((units >> u) & 1) != 0; }

// Order rule: depth of the k-th of n layers, 1 (first drawn, far) .. 0 (last); order_curve < 1 gives the
// first layers (backgrounds, usually several parallax planes) more of the depth range
float OrderT(size_t k, size_t n)
{
    if (n < 2) return 0.0f;
    const float x = std::min(1.0f, (float)k / (float)(n - 1));
    return 1.0f - (g_cfg.flatOrderCurve == 1.0f ? x : std::pow(x, std::max(0.05f, g_cfg.flatOrderCurve)));
}

// A fixed-function draw from client-side arrays (Machinarium): its extent on the screen and, when it puts a
// bitmap onto a shape, the affine map from its texture coordinates to the screen. Adventure games draw
// pieces of the background again (walk-behinds: Machinarium's tower and ground edge); such a piece maps
// its texture exactly as the background did. m: x = m0*u + m1*v + m2, y = m3*u + m4*v + m5 (NDC).
struct FfGeom {
    float lo[2] = {1e30f, 1e30f}, hi[2] = {-1e30f, -1e30f};
    bool mapped = false;
    GLuint tex = 0;
    float m[6] = {};
    float u0 = 0, v0 = 0, u1 = 0, v1 = 0;   // texture rectangle the draw uses
    float tol = 0;                           // 3 pixels in NDC
};

static bool FfGeometry(Ctx* c, int n, FfGeom& g)
{
    if (!t_drawArrays || n < 3 || n > 4096 || !c->compat || !R.GetPointerv || !R.IsEnabled(GL_VERTEX_ARRAY)) return false;
    GLint vs = 0, vt = 0, vst = 0, vb = 1, eb = 0;
    R.GetIntegerv(GL_VERTEX_ARRAY_SIZE, &vs);
    R.GetIntegerv(GL_VERTEX_ARRAY_TYPE, &vt);
    R.GetIntegerv(GL_VERTEX_ARRAY_STRIDE, &vst);
    R.GetIntegerv(GL_VERTEX_ARRAY_BUFFER_BINDING, &vb);
    if (t_drawIndexType) R.GetIntegerv(GL_ELEMENT_ARRAY_BUFFER_BINDING, &eb);
    if (vs < 2 || vt != GL_FLOAT || vb || eb) return false;
    if (t_drawIndexType && (!t_drawIndices || (t_drawIndexType != GL_UNSIGNED_BYTE && t_drawIndexType != GL_UNSIGNED_SHORT &&
                                               t_drawIndexType != GL_UNSIGNED_INT)))
        return false;
    void* vp = nullptr;
    R.GetPointerv(GL_VERTEX_ARRAY_POINTER, &vp);
    if (!vp) return false;
    const size_t vstride = vst ? (size_t)vst : (size_t)vs * 4;
    GLfloat mv[16], pr[16];
    GLint view[4] = {};
    R.GetFloatv(GL_MODELVIEW_MATRIX, mv);
    R.GetFloatv(GL_PROJECTION_MATRIX, pr);
    R.GetIntegerv(GL_VIEWPORT, view);
    g.tol = view[2] > 0 ? 6.0f / (float)view[2] : 0.002f;
    auto vertex = [&](int i) -> size_t {
        if (!t_drawIndexType) return (size_t)(t_drawFirst + i);
        if (t_drawIndexType == GL_UNSIGNED_BYTE) return ((const uint8_t*)t_drawIndices)[i];
        if (t_drawIndexType == GL_UNSIGNED_SHORT) return ((const uint16_t*)t_drawIndices)[i];
        return ((const uint32_t*)t_drawIndices)[i];
    };
    std::vector<float> X(n), Y(n);
    for (int i = 0; i < n; ++i) {
        const float* p = (const float*)((const char*)vp + vertex(i) * vstride);
        const float in[4] = {p[0], p[1], vs > 2 ? p[2] : 0.0f, vs > 3 ? p[3] : 1.0f};
        float e[4], cl[4];
        for (int r = 0; r < 4; ++r) e[r] = mv[r] * in[0] + mv[4 + r] * in[1] + mv[8 + r] * in[2] + mv[12 + r] * in[3];
        for (int r = 0; r < 4; ++r) cl[r] = pr[r] * e[0] + pr[4 + r] * e[1] + pr[8 + r] * e[2] + pr[12 + r] * e[3];
        if (cl[3] == 0.0f) return false;
        X[i] = cl[0] / cl[3];
        Y[i] = cl[1] / cl[3];
        g.lo[0] = std::min(g.lo[0], X[i]);
        g.hi[0] = std::max(g.hi[0], X[i]);
        g.lo[1] = std::min(g.lo[1], Y[i]);
        g.hi[1] = std::max(g.hi[1], Y[i]);
    }
    // the texture map, from three vertices whose texture coordinates span a triangle
    GLint client = 0, ts = 0, tt = 0, tst = 0, tb = 1;
    if (!(SampledUnits(c) & 1u) || !(g.tex = c->bound[0][kSlot2D]) || !R.IsEnabled(GL_TEXTURE_COORD_ARRAY)) return true;
    R.GetIntegerv(GL_CLIENT_ACTIVE_TEXTURE, &client);
    R.GetIntegerv(GL_TEXTURE_COORD_ARRAY_SIZE, &ts);
    R.GetIntegerv(GL_TEXTURE_COORD_ARRAY_TYPE, &tt);
    R.GetIntegerv(GL_TEXTURE_COORD_ARRAY_STRIDE, &tst);
    R.GetIntegerv(GL_TEXTURE_COORD_ARRAY_BUFFER_BINDING, &tb);
    void* tp = nullptr;
    R.GetPointerv(GL_TEXTURE_COORD_ARRAY_POINTER, &tp);
    if (client != (GLint)GL_TEXTURE0 || ts < 2 || tt != GL_FLOAT || tb || !tp) return true;
    const size_t tstride = tst ? (size_t)tst : (size_t)ts * 4;
    auto tc = [&](int i) { return (const float*)((const char*)tp + vertex(i) * tstride); };
    int i1 = -1, i2 = -1;
    const float* a = tc(0);
    for (int i = 1; i < n && i2 < 0; ++i) {
        const float* b = tc(i);
        if (i1 < 0) {
            if (fabsf(b[0] - a[0]) + fabsf(b[1] - a[1]) > 1e-6f) i1 = i;
            continue;
        }
        const float* m1 = tc(i1);
        if (fabsf((m1[0] - a[0]) * (b[1] - a[1]) - (b[0] - a[0]) * (m1[1] - a[1])) > 1e-9f) i2 = i;
    }
    if (i2 < 0) return true;
    const int idx[3] = {0, i1, i2};
    float U[3], V[3], PX[3], PY[3];
    for (int j = 0; j < 3; ++j) {
        U[j] = tc(idx[j])[0];
        V[j] = tc(idx[j])[1];
        PX[j] = X[idx[j]];
        PY[j] = Y[idx[j]];
    }
    const float det = U[0] * (V[1] - V[2]) - V[0] * (U[1] - U[2]) + (U[1] * V[2] - U[2] * V[1]);
    if (fabsf(det) < 1e-12f) return true;
    auto solve = [&](const float* r, float* out) {   // Cramer's rule: [u v 1] . out = r
        out[0] = (r[0] * (V[1] - V[2]) - V[0] * (r[1] - r[2]) + (r[1] * V[2] - r[2] * V[1])) / det;
        out[1] = (U[0] * (r[1] - r[2]) - r[0] * (U[1] - U[2]) + (U[1] * r[2] - U[2] * r[1])) / det;
        out[2] = (U[0] * (V[1] * r[2] - V[2] * r[1]) - V[0] * (U[1] * r[2] - U[2] * r[1]) + r[0] * (U[1] * V[2] - U[2] * V[1])) / det;
    };
    solve(PX, g.m);
    solve(PY, g.m + 3);
    g.u0 = g.v0 = 1e30f;
    g.u1 = g.v1 = -1e30f;
    for (int i = 0; i < n; ++i) {
        const float* t = tc(i);
        if (fabsf(g.m[0] * t[0] + g.m[1] * t[1] + g.m[2] - X[i]) > g.tol || fabsf(g.m[3] * t[0] + g.m[4] * t[1] + g.m[5] - Y[i]) > g.tol)
            return true;   // not one bitmap placement
        g.u0 = std::min(g.u0, t[0]);
        g.u1 = std::max(g.u1, t[0]);
        g.v0 = std::min(g.v0, t[1]);
        g.v1 = std::max(g.v1, t[1]);
    }
    g.mapped = true;
    return true;
}

// The layer of a picture drawn earlier this frame into the same target that this draw repeats a piece of
// (same texture, both maps agree over this draw's texture rectangle), or 0. A piece that covers anything
// drawn over that picture since (a character behind a walk-behind) stays a layer of its own in front.
// matched: it repeats a piece of a picture at all (then it is no new picture itself).
static const Ctx::Picture* FfRepeatOf(Ctx* c, const FfGeom& g)
{
    for (const Ctx::Picture& p : c->pictures) {
        if (p.fb != c->drawFb || p.tex != g.tex) continue;
        bool same = true;
        const float cu[4] = {g.u0, g.u1, g.u0, g.u1}, cv[4] = {g.v0, g.v0, g.v1, g.v1};
        for (int k = 0; k < 4 && same; ++k)
            for (int r = 0; r < 2; ++r) {
                const float* m = g.m + 3 * r;
                const float* q = p.m + 3 * r;
                same &= fabsf((m[0] - q[0]) * cu[k] + (m[1] - q[1]) * cv[k] + (m[2] - q[2])) <= g.tol;
            }
        if (same) return &p;
    }
    return nullptr;
}

static uint64_t FfContinues(Ctx* c, const FfGeom& g, bool& matched)
{
    const Ctx::Picture* found = FfRepeatOf(c, g);
    matched = found != nullptr;
    if (found) {
        const Ctx::Picture& p = *found;
        for (size_t b = p.box + 1; b < c->ffBoxes.size(); ++b) {
            const Ctx::FfBox& o = c->ffBoxes[b];
            if (o.fb == p.fb && o.key != p.key && o.lo[0] < g.hi[0] - g.tol && g.lo[0] < o.hi[0] - g.tol &&
                o.lo[1] < g.hi[1] - g.tol && g.lo[1] < o.hi[1] - g.tol)
                return 0;
        }
        return p.key;
    }
    return 0;
}

// Feet rule for fixed-function draws (Machinarium; the geometry shader does this for GLSL). The first
// pictures as wide as the screen are the floor, sloping back with the screen height; a piece of a picture
// drawn again (walk-behind) keeps that picture's depth; other draws stand at the bottom edge of the object
// they belong to (parts stacked within each other's width: Machinarium's robot is legs, body and head);
// a screen-wide picture drawn after those (inventory bar, fade) and what lies in it stay at the screen.
void FeetDepthForDraw(Ctx* c)
{
    c->ffKind = 0;
    if ((g_cfg.flatScreenBatch > 0 && c->batch >= g_cfg.flatScreenBatch) || SamplesStereoTarget(c)) return;
    FfGeom g;
    if (!FfGeometry(c, t_drawCount, g)) return;
    auto t = [](float y) { return (std::max(-1.0f, std::min(1.0f, y)) + 1.0f) * 0.5f * g_cfg.flatTop - g_cfg.flatZero; };
    const Ctx::Picture* rep = g.mapped ? FfRepeatOf(c, g) : nullptr;
    const float w = g.hi[0] - g.lo[0], h = g.hi[1] - g.lo[1];
    const char* what = "object";
    if (rep) {
        c->ffKind = rep->kind;
        c->ffDepth = rep->depth;
        c->ffRunOn = false;
        what = "repeat";
    } else if (g_cfg.flatPlaneTall > 0.0f && h >= g_cfg.flatPlaneTall && !(w >= g_cfg.flatPlane && h >= g_cfg.flatPlaneHeight)) {
        c->ffKind = 2;   // a tall piece of a picture: on the floor slope like the rest of it
        c->ffRunOn = false;
        what = "tall";
    } else if (w >= g_cfg.flatPlane && h >= g_cfg.flatPlaneHeight) {
        if (c->ffSprites == 0) {
            c->ffKind = 2;
            what = "floor";
        } else {
            c->ffOverlays.push_back({g.lo[0], g.lo[1], g.hi[0], g.hi[1]});
            what = "overlay";
        }
        c->ffRunOn = false;
    } else {
        // mostly inside such a picture (an inventory icon; not a thought bubble reaching up into the bar)
        bool inOverlay = false;
        for (const auto& o : c->ffOverlays) {
            const float ix = std::min(g.hi[0], o[2]) - std::max(g.lo[0], o[0]), iy = std::min(g.hi[1], o[3]) - std::max(g.lo[1], o[1]);
            inOverlay |= ix > 0 && iy > 0 && ix * iy >= 0.7f * std::max(w * h, 1e-8f);
        }
        if (inOverlay) {
            what = "in overlay";
        } else {
            bool join = c->ffRunOn && c->ffRunFb == c->drawFb;
            if (join) {
                const float ox = std::min(g.hi[0], c->ffRun[2]) - std::max(g.lo[0], c->ffRun[0]);
                join = ox >= 0.6f * std::max(w, 1e-6f) && g.lo[1] <= c->ffRun[3] + 0.02f && g.hi[1] >= c->ffRun[1] - 0.02f;
            }
            if (join) {
                c->ffRun[0] = std::min(c->ffRun[0], g.lo[0]);
                c->ffRun[1] = std::min(c->ffRun[1], g.lo[1]);
                c->ffRun[2] = std::max(c->ffRun[2], g.hi[0]);
                c->ffRun[3] = std::max(c->ffRun[3], g.hi[1]);
                what = "part";
            } else {
                c->ffRun[0] = g.lo[0];
                c->ffRun[1] = g.lo[1];
                c->ffRun[2] = g.hi[0];
                c->ffRun[3] = g.hi[1];
                c->ffRunOn = true;
                c->ffRunFb = c->drawFb;
                // drawn over an object drawn before it, it is no farther than that object, or the eyes see
                // it behind what it covers (a thought bubble over the robot's head)
                c->ffRunCap = 1e30f;
                for (const Ctx::FfObj& o : c->ffObjects)
                    if (o.fb == c->drawFb && o.box[0] < g.hi[0] - g.tol && g.lo[0] < o.box[2] - g.tol && o.box[1] < g.hi[1] - g.tol &&
                        g.lo[1] < o.box[3] - g.tol)
                        c->ffRunCap = std::min(c->ffRunCap, o.depth);
                if (c->ffObjects.size() < 4096) c->ffObjects.push_back({c->drawFb, {g.lo[0], g.lo[1], g.hi[0], g.hi[1]}, 0.0f});
            }
            c->ffKind = 1;
            // sprite_min moves with the screen plane (depth_screen, Ctrl+F5/F6), or bottom sprites stay behind the floor
            c->ffDepth = std::min(std::max(t(c->ffRun[1]), g_cfg.flatSpriteMin - g_cfg.flatZero), c->ffRunCap);
            if (!c->ffObjects.empty()) {   // the object's entry: its box so far and its depth
                Ctx::FfObj& o = c->ffObjects.back();
                memcpy(o.box, c->ffRun, sizeof(o.box));
                o.depth = c->ffDepth;
            }
            c->ffSprites++;
        }
    }
    if (g.mapped && !rep && c->pictures.size() < 1024) {
        Ctx::Picture pic{c->drawFb, g.tex, {}, 0, 0, c->ffKind, c->ffDepth};
        memcpy(pic.m, g.m, sizeof(pic.m));
        c->pictures.push_back(pic);
    }
    if (g_traceNow)
        Log("T %u.%u feet %s: kind %d depth %.3f box %.3f,%.3f-%.3f,%.3f tex %u", g_frame, g_traceDraw, what, c->ffKind,
            c->ffKind == 1 ? c->ffDepth : 0.0f, g.lo[0], g.lo[1], g.hi[0], g.hi[1], g.mapped ? g.tex : 0);
}

void OrderDepthForDraw(Ctx* c)
{
    FlatEv ev;
    ev.fb = c->drawFb;
    ev.prog = c->program;
    // the texture it draws with: engines put it on any unit (Huntdown under WineD3D keeps a 0x0
    // placeholder on unit 0 and the sprite atlas on another one); the group key takes all of them
    uint64_t texKey = 0;
    const uint32_t units = SampledUnits(c);
    for (int u = 0; u <= c->maxUnit; ++u) {
        if (!UnitIn(units, u)) continue;
        const GLuint id = c->bound[u][kSlot2D];
        texKey = texKey * 0x100000001B3ull ^ id;
        if (id && !ev.tex) {
            TexInfo* ti = FindTex(id);
            if (ti && ti->w > 0) ev.tex = id;
        }
    }
    if (!ev.tex) ev.tex = c->bound[0][kSlot2D];
    ev.count = t_drawCount;
    t_drawCount = -1;
    c->drawPrims = ev.count <= 0 ? 0.0f : (float)(t_drawMode == GL_TRIANGLES ? ev.count / 3 : std::max(ev.count - 2, 0));
    if (R.IsEnabled(GL_SCISSOR_TEST)) R.GetIntegerv(GL_SCISSOR_BOX, ev.sc);
    // render targets this draw samples; a mono one drawn into the stereo scene is an off-screen layer
    // cache (Door Kickers: Action Squad renders the whole level into a low-resolution 1024x1024
    // target first): make it stereo, or every layer in it would end up at one depth
    const bool toStereo = Active(c) && RightFor(c, c->drawFb) != 0;
    for (int u = 0; u <= c->maxUnit; ++u) {
        GLuint id = c->bound[u][kSlot2D];
        if (!id || !UnitIn(units, u)) continue;
        TexInfo* ti = FindTex(id);
        GLuint base = ti && ti->viewOf ? ti->viewOf : id;
        GLuint fbs[4];
        int nfb = FbosOfTexture(c, base, fbs, 4);
        // a copy of a render target (WineD3D resolves the scene with glCopyImageSubData): its source
        for (GLuint t = base, hops = 0; !nfb && hops < 3; ++hops) {
            auto cf = c->texCopiedFrom.find(t);
            if (cf == c->texCopiedFrom.end()) break;
            t = cf->second;
            nfb = FbosOfTexture(c, t, fbs, 4);
        }
        for (int k = 0; k < nfb; ++k) {
            const GLuint fb = fbs[k];
            if (fb == c->drawFb) continue;
            if (ev.nsrc < 4 && std::find(ev.src, ev.src + ev.nsrc, fb) == ev.src + ev.nsrc) ev.src[ev.nsrc++] = fb;
            FboInfo& f = c->fbo[fb];
            // only a target drawn into in this frame is a layer cache: WineD3D also attaches ordinary
            // textures (sprite atlases) to its own framebuffers to upload or clear them
            if (toStereo && !f.promoted && !f.stereo &&
                std::any_of(c->flatEvents.begin(), c->flatEvents.end(), [fb](const FlatEv& e) { return e.fb == fb && e.key; })) {
                f.promoted = true;
                f.dirty = true;
                Log("flat depth: fbo %u (texture %u) is drawn into the stereo scene: layer cache", fb, base);
            }
        }
    }
    if (SamplesStereoTarget(c)) {   // composite pass: stays at the screen
        c->flatEvents.push_back(ev);
        return;
    }
    // fixed-function and ARB draws have no GLSL program; the salt keeps an untextured one's key from being 0 (no layer)
    const uint64_t prog = c->program ? c->program : c->arbActive ? 0x80000000u | c->arbVp : 0;
    const uint64_t id = ((prog << 32) ^ texKey) ^ 0x5BD1E995ull;
    // fixed function: a piece of a picture drawn before in this frame (a walk-behind) joins its layer, or
    // it stands out of the background as a rectangle
    FfGeom fg;
    const bool geom = !c->program && !c->arbActive && FfGeometry(c, ev.count, fg);
    bool repeat = false;
    const uint64_t cont = geom && fg.mapped ? FfContinues(c, fg, repeat) : 0;
    if (cont) {
        static int logged;
        if (logged++ < 3) Log("flat depth: a draw repeats part of an earlier picture (texture %u, same placement): same layer", fg.tex);
        c->groupKey = cont;
        c->groupId = 0;   // the draw after it starts a group of its own
    } else if (id != c->groupId || c->frameGroups == 0 || g_cfg.flatGroupByDraw) {
        c->groupId = id;
        int seen = 0;
        for (uint64_t g : c->groupIds) seen += g == id;
        c->groupIds.push_back(id);
        c->groupKey = (id * 0x9E3779B97F4A7C15ull) ^ (uint64_t)seen;
        c->frameGroups++;
    }
    ev.key = c->groupKey;
    c->flatEvents.push_back(ev);
    if (geom && c->ffBoxes.size() < 4096) {
        if (fg.mapped && !repeat) {
            Ctx::Picture pic{c->drawFb, fg.tex, {}, c->groupKey, c->ffBoxes.size(), 0, 0.0f};
            memcpy(pic.m, fg.m, sizeof(pic.m));
            c->pictures.push_back(pic);
        }
        c->ffBoxes.push_back({c->drawFb, {fg.lo[0], fg.lo[1]}, {fg.hi[0], fg.hi[1]}, c->groupKey});
    }
    size_t g = c->frameGroups - 1;
    size_t n = std::max(c->lastGroupCount, c->frameGroups);
    auto lt = c->layoutPos.find(c->groupKey);   // its place in the composed picture, when known
    if (lt != c->layoutPos.end()) {
        g = lt->second;
        n = c->layoutCount;
        if (c->drawFb == c->layoutLastFb) c->layoutLastG = (long long)g;
    } else if (c->layoutCount > 1 && c->drawFb == c->layoutLastFb && c->layoutLastG >= 0) {
        // not in the last frame's picture (an animated sprite shows another frame texture): right after
        // the group drawn before it into the same target, on the same scale as the known ones
        g = (size_t)std::min<long long>(c->layoutLastG + 1, (long long)c->layoutCount - 1);
        n = c->layoutCount;
    }
    if (lt != c->layoutPos.end() || c->drawFb != c->layoutLastFb) {
        c->layoutLastFb = c->drawFb;
        c->layoutLastG = lt != c->layoutPos.end() ? (long long)lt->second : -1;
    }
    const float t = OrderT(g, n);
    c->orderSlot = n > 1 ? (g > 0 ? OrderT(g - 1, n) - t : t - OrderT(1, n)) : 0.0f;
    // smoothed while the group keeps being drawn; a group back after a pause (an animated sprite's
    // frame texture, shown again later) takes its depth right away, or it lags behind from long ago
    auto it = c->keyDepth.find(c->groupKey);
    if (it == c->keyDepth.end()) it = c->keyDepth.emplace(c->groupKey, Ctx::KeyDepth{t, g_frame}).first;
    else if (g_frame - it->second.frame > 2) it->second.depth = t;
    else if (c->groupKeyFrame != g_frame || c->groupKeyLast != c->groupKey) it->second.depth += (t - it->second.depth) * 0.05f;
    it->second.frame = g_frame;
    c->groupKeyFrame = g_frame;
    c->groupKeyLast = c->groupKey;
    c->orderDepth = it->second.depth;
    if (g_traceNow) Log("T %u.%u order group %zu/%zu prog %u tex %u target %.3f depth %.3f slot %.3f prims %.0f%s", g_frame, g_traceDraw, g, n,
                        c->program, ev.tex, t, c->orderDepth, c->orderSlot, c->drawPrims, cont ? " (repeats an earlier picture)" : "");
}

// Painter's order of the composed picture: walks the framebuffer drawn last; a composite pass stands
// for the layers drawn into the targets it samples (before it), so a layer cache drawn first but
// composited over the background sits in front of that background.
static void WalkLayout(const std::vector<FlatEv>& ev, const std::unordered_map<uint64_t, uint64_t>& alias, GLuint fb,
                       size_t end, int depth, std::vector<uint64_t>& order)
{
    for (size_t i = 0; i < end; ++i) {
        const FlatEv& e = ev[i];
        if (e.fb != fb) continue;
        if (e.key) {
            auto a = alias.find(e.key);
            uint64_t k = a != alias.end() ? a->second : e.key;
            if (std::find(order.begin(), order.end(), k) == order.end()) order.push_back(k);
        } else if (depth < 6) {
            for (int s = 0; s < e.nsrc; ++s) WalkLayout(ev, alias, e.src[s], i, depth + 1, order);
        }
    }
}

// At the end of a frame: the depth of each layer group from where it lands in the composed picture.
void ComputeLayout(Ctx* c)
{
    std::vector<FlatEv>& ev = c->flatEvents;
    c->pictures.clear();
    c->ffBoxes.clear();
    c->ffSprites = 0;
    c->ffRunOn = false;
    c->ffOverlays.clear();
    c->ffObjects.clear();
    c->layoutPos.clear();
    c->layoutCount = 0;
    if (ev.empty()) return;
    // A run of groups drawn again into another region of the same target (deferred lighting draws the
    // tiles once for colour, once for normals) repeats an earlier run: same program and draw counts.
    // It takes that run's depths, or the lighting would not line up with the colour in one eye.
    std::unordered_map<uint64_t, uint64_t> alias;
    std::vector<size_t> starts;
    for (size_t i = 0; i < ev.size(); ++i)
        if (ev[i].key && (i == 0 || ev[i - 1].key != ev[i].key || ev[i - 1].fb != ev[i].fb)) starts.push_back(i);
    size_t cursor = SIZE_MAX;
    for (size_t s = 0; s < starts.size(); ++s) {
        const FlatEv& e = ev[starts[s]];
        if (cursor != SIZE_MAX && cursor < s) {
            const FlatEv& p = ev[starts[cursor]];
            if (p.fb == e.fb && p.prog == e.prog && p.count == e.count && p.key != e.key) {
                auto a = alias.find(p.key);
                alias[e.key] = a != alias.end() ? a->second : p.key;
                ++cursor;
                continue;
            }
            cursor = SIZE_MAX;
        }
        if (e.count <= 12) continue;
        auto same = [](const FlatEv& x, const FlatEv& y) { return x.fb == y.fb && x.prog == y.prog && x.count == y.count; };
        for (size_t q = 0; q < s; ++q) {
            const FlatEv& p = ev[starts[q]];
            if (!same(p, e) || p.key == e.key) continue;
            // a repeat: drawn into another scissor region, or the next group repeats the one after
            // p as well (FRAMED draws each comic panel as frame + picture: the panels are one layer)
            const bool otherRegion = e.sc[2] >= 0 && memcmp(p.sc, e.sc, sizeof(p.sc)) != 0;
            const bool runRepeats = q + 1 < s && s + 1 < starts.size() && same(ev[starts[q + 1]], ev[starts[s + 1]]);
            if (otherRegion || runRepeats) {
                auto pa = alias.find(p.key);
                alias[e.key] = pa != alias.end() ? pa->second : p.key;
                cursor = q + 1;
                break;
            }
        }
    }
    // the picture is the target drawn last (trailing copies to the window carry no layers; with no
    // program at all, fixed-function or ARB, the last event's target: Machinarium's copy to the window
    // leads back to the scene, Rocketbirds' composite into the window follows its bloom passes)
    GLuint root = ev.back().fb;
    for (size_t i = ev.size(); i-- > 0;)
        if (ev[i].prog) {
            root = ev[i].fb;
            break;
        }
    // A scratch target, cleared and drawn again more than once in the frame (Astral Ascent renders
    // every piece of text into one, glyph by glyph, and composites it): each use is one layer,
    // placed where it is composited, or the glyphs of a line step forward one after another.
    // Not the picture itself: Rocketbirds draws the scene into the window, copies it out, clears
    // the window and draws the copy back with the HUD (one use would flatten the whole scene).
    std::unordered_map<GLuint, int> uses;
    {
        std::unordered_map<GLuint, bool> drawn;   // drawn into since its last clear
        for (const FlatEv& e : ev) {
            if (e.clear) {
                drawn[e.fb] = false;
            } else if (e.key && !drawn[e.fb] && e.fb != root) {
                drawn[e.fb] = true;
                uses[e.fb]++;
            }
        }
    }
    std::string scratch;
    {
        std::unordered_map<GLuint, uint64_t> first;   // first layer of the current use
        for (const FlatEv& e : ev) {
            if (uses[e.fb] < 2) continue;
            if (e.clear) {
                first.erase(e.fb);
                continue;
            }
            if (!e.key) continue;
            auto f = first.find(e.fb);
            if (f == first.end()) {
                auto a = alias.find(e.key);
                first[e.fb] = a != alias.end() ? a->second : e.key;
            } else if (e.key != f->second && !alias.count(e.key)) {
                alias[e.key] = f->second;
            }
        }
        for (auto& u : uses)
            if (u.second >= 2) scratch += " fb" + std::to_string(u.first) + "x" + std::to_string(u.second);
    }
    std::vector<uint64_t> order;
    WalkLayout(ev, alias, root, ev.size(), 0, order);
    const size_t n = order.size();
    for (size_t k = 0; k < n; ++k) c->layoutPos[order[k]] = k;
    c->layoutCount = n;
    for (auto& a : alias) {
        auto it = c->layoutPos.find(a.second);
        if (it != c->layoutPos.end()) c->layoutPos.emplace(a.first, it->second);
    }
    if (g_traceNow) {
        std::string s, comp;
        for (const FlatEv& e : ev) {   // composites and copies, to see where the walk stops
            if (e.key || e.clear) continue;
            char buf[48];
            sprintf_s(buf, " fb%u<", e.fb);
            comp += buf;
            for (int k = 0; k < e.nsrc; ++k) {
                sprintf_s(buf, "%sfb%u", k ? "," : "", e.src[k]);
                comp += buf;
            }
        }
        Log("T %u layout events %zu, root fb%u, scratch targets:%s, composites/copies:%s", g_frame, ev.size(), root,
            scratch.empty() ? " -" : scratch.c_str(), comp.c_str());
        for (size_t k = 0; k < n; ++k) {
            for (const FlatEv& e : ev)
                if (e.key == order[k]) {
                    char buf[64];
                    sprintf_s(buf, " %zu:fb%u/p%u/t%u", k, e.fb, e.prog, e.tex);
                    s += buf;
                    break;
                }
        }
        Log("T %u layout (far to near, %zu groups, %zu repeats):%s", g_frame, n, alias.size(), s.c_str());
    }
    ev.clear();
}

// A bound texture has a right-eye clone: it is a stereo render target, so this draw composites or
// post-processes images that already have their per-eye offsets.
bool SamplesStereoTarget(Ctx* c)
{
    const uint32_t units = SampledUnits(c);
    for (int u = 0; u <= c->maxUnit; ++u)
        for (int sl = 0; sl < kSlotCount && UnitIn(units, u); ++sl) {
            GLuint id = c->bound[u][sl];
            if (!id) continue;
            TexInfo* ti = FindTex(id);
            if (ti && ti->right) return true;
            if (ti && ti->viewOf) {
                TexInfo* oi = FindTex(ti->viewOf);
                if (oi && oi->right) return true;
            }
        }
    return false;
}

void EyeBegin(Ctx* c, float eye, EyeState& st)
{
    st.matrix = false;
    ProgInfo* pi = c->program ? FindProg(c->program) : nullptr;
    if (pi && pi->loc >= 0) {
        if (pi->eye != eye || pi->ver != g_paramVer) {
            R.Uniform4f(pi->loc, eye, SepValue(), g_cfg.convergence, g_cfg.monoW1 ? 1.0f : 0.0f);
            if (pi->locQ >= 0) R.Uniform4f(pi->locQ, g_cfg.orthoDepth, g_cfg.orthoConvergence, 0.0f, 0.0f);
            pi->eye = eye;
            pi->ver = g_paramVer;
            g_stats.uniformSets++;
        }
        if (pi->gs) {
            if (pi->fVer != g_paramVer) {
                R.Uniform4f(pi->locF, g_cfg.flatEnable ? (g_cfg.flatRule == 1 ? 2.0f : 1.0f) : 0.0f, g_cfg.flatTop,
                            g_cfg.flatPlane, g_cfg.flatZero);
                pi->fVer = g_paramVer;
                pi->lastD[0] = NAN;
                pi->lastS = NAN;
            }
            // screen depth: later batches (UI), and passes that read a stereo render target (the
            // scene already drawn per eye being composited or post-processed)
            float d = (g_cfg.flatScreenBatch > 0 && c->batch >= g_cfg.flatScreenBatch) || SamplesStereoTarget(c) ? 1.0f : 0.0f;
            const bool order = g_cfg.flatRule == 1;
            float w = order ? c->orderDepth * g_cfg.flatTop - g_cfg.flatZero : 0.0f;
            // order rule: a draw spreads over 80% of its group's slot, so groups never overlap
            float slot = order ? c->orderSlot * g_cfg.flatTop * 0.8f : 0.0f;
            float scale = 1.0f;
            if (d == 0.0f && order && c->drawFb) {
                auto f = c->fbo.find(c->drawFb);
                if (f != c->fbo.end() && f->second.promoted) {
                    // layer cache: its offset is scaled to the part of it that fills the screen (the
                    // scissored region), so a layer moves as far on screen as one drawn there directly
                    scale = g_cfg.flatCacheScale;
                    if (scale <= 0.0f) {
                        scale = 1.0f;
                        GLint vp[4] = {}, sc[4] = {};
                        R.GetIntegerv(GL_VIEWPORT, vp);
                        if (R.IsEnabled(GL_SCISSOR_TEST)) {
                            R.GetIntegerv(GL_SCISSOR_BOX, sc);
                            if (sc[2] > 0 && sc[2] < vp[2]) scale = (float)sc[2] / (float)vp[2];
                        }
                    }
                    w *= scale;
                    slot *= scale;
                }
            }
            // feet: sprite_min moves with the screen plane (depth_screen), like the floor slope does
            const float v[4] = {d, order ? slot : g_cfg.flatSpriteMin - g_cfg.flatZero, order ? c->drawPrims : g_cfg.flatPlaneHeight,
                                order ? w : g_cfg.flatPlaneTall};
            if (memcmp(v, pi->lastD, sizeof(v)) != 0) {
                R.Uniform4f(pi->locD, v[0], v[1], v[2], v[3]);
                memcpy(pi->lastD, v, sizeof(v));
            }
            if (pi->locS >= 0) {
                // silhouette depth: dark pixels of the picture sit silhouette_depth nearer than the
                // picture, as a shift in pixels of the target (NDC offset * half the viewport width)
                float s = 0.0f;
                if (d == 0.0f && eye != 0.0f) {
                    GLint vp[4] = {};
                    R.GetIntegerv(GL_VIEWPORT, vp);
                    s = -eye * SepValue() * g_cfg.silDepth * g_cfg.flatTop * scale * (float)vp[2] * 0.5f;
                    static int logged;
                    if (logged++ < 4) Log("silhouette depth: program %u eye %+.0f shift %.1f px (viewport %d, group depth %.3f)", c->program, eye, s, vp[2], w);
                }
                if (s != pi->lastS) {
                    R.Uniform4f(pi->locS, s, g_cfg.silThreshold, 0.0f, 0.0f);
                    pi->lastS = s;
                }
            }
        }
        return;
    }
    if (c->arbActive) {
        // rewritten ARB vertex program: x = eye * separation, y = convergence, z = offset per w for flat
        // (w == 1) geometry - its order-rule depth (no per-sprite spread without a geometry shader)
        const float a = eye * SepValue();
        float flat = 0.0f;
        if (eye != 0.0f) {
            if (g_cfg.flatEnable && g_cfg.flatRule == 1) {
                const bool screen = (g_cfg.flatScreenBatch > 0 && c->batch >= g_cfg.flatScreenBatch) || SamplesStereoTarget(c);
                if (!screen) flat = a * ((c->orderDepth + c->orderSlot * 0.4f) * g_cfg.flatTop - g_cfg.flatZero);
            } else if (!g_cfg.monoW1) {
                flat = a * (1.0f - g_cfg.convergence);
            }
        }
        R.ProgramEnvParameter4fARB(GL_VERTEX_PROGRAM_ARB, (GLuint)c->arbEnv, a, g_cfg.convergence, flat, 0.0f);
        return;
    }
    // Fixed function, ARB programs and GLSL using the built-in matrices: offset the projection.
    if (eye == 0.0f || !c->compat || !g_cfg.fixedFunction) return;
    GLfloat p[16];
    R.GetFloatv(GL_PROJECTION_MATRIX, p);
    float s = eye * SepValue(), shift, slope = 0.0f;
    if (p[15] != 0.0f || p[11] == 0.0f) {
        // orthographic (flat 2D, Machinarium): with flat depth on, the draw's order-rule depth as a fixed
        // x shift per w (no per-sprite spread: there is no geometry shader on this path); feet rule: the
        // object's depth, or the floor's slope (shift grows with clip y)
        const bool flat = !c->program && g_cfg.flatEnable && c->isMain;
        if (flat && g_cfg.flatRule == 1 && !((g_cfg.flatScreenBatch > 0 && c->batch >= g_cfg.flatScreenBatch) || SamplesStereoTarget(c)))
            shift = s * ((c->orderDepth + c->orderSlot * 0.4f) * g_cfg.flatTop - g_cfg.flatZero);
        else if (flat && g_cfg.flatRule == 0 && c->ffKind == 1)
            shift = s * c->ffDepth;
        else if (flat && g_cfg.flatRule == 0 && c->ffKind == 2) {
            shift = s * (0.5f * g_cfg.flatTop - g_cfg.flatZero);
            slope = s * 0.5f * g_cfg.flatTop;
        } else
            return;   // 2D at the screen
    } else {
        shift = 0.0f;   // perspective: below
    }
    R.GetIntegerv(GL_MATRIX_MODE, &st.mode);
    memcpy(st.proj, p, sizeof(p));
    if (p[15] != 0.0f || p[11] == 0.0f) {
        // clip.x += shift * clip.w + slope * clip.y: row 0 += shift * row 3 + slope * row 1
        p[0] += shift * p[3] + slope * p[1];
        p[4] += shift * p[7] + slope * p[5];
        p[8] += shift * p[11] + slope * p[9];
        p[12] += shift * p[15] + slope * p[13];
        R.MatrixMode(GL_PROJECTION);
        R.LoadMatrixf(p);
        if ((GLenum)st.mode != GL_PROJECTION) R.MatrixMode(st.mode);
        st.matrix = true;
        g_stats.matrixDraws++;
        return;
    }
    // clip.x += eye * sep * (clip.w - convergence), with eye-space w == 1
    p[0] += s * p[3];
    p[4] += s * p[7];
    p[8] += s * p[11];
    p[12] += s * p[15] - s * g_cfg.convergence;
    R.MatrixMode(GL_PROJECTION);
    R.LoadMatrixf(p);
    if ((GLenum)st.mode != GL_PROJECTION) R.MatrixMode(st.mode);
    st.matrix = true;
    g_stats.matrixDraws++;
}

void EyeEnd(EyeState& st)
{
    if (!st.matrix) return;
    R.MatrixMode(GL_PROJECTION);
    R.LoadMatrixf(st.proj);
    if ((GLenum)st.mode != GL_PROJECTION) R.MatrixMode(st.mode);
    st.matrix = false;
}

// Rebinds every bound texture that has a right-eye clone (toRight) or back to the original.
void SwapTextures(Ctx* c, bool toRight)
{
    GLuint glUnit = c->activeUnit;   // the unit GL currently has active
    for (int u = 0; u <= c->maxUnit; ++u) {
        for (int s = 0; s < kSlotCount; ++s) {
            GLuint id = c->bound[u][s];
            if (!id) continue;
            TexInfo* ti = FindTex(id);
            if (ti && !ti->right && ti->viewOf && toRight) {
                TexInfo* oi = FindTex(ti->viewOf);
                if (oi && oi->right) {
                    TexClone(c, id, ti->target);
                    ti = FindTex(id);
                }
            }
            if (!ti || !ti->right) continue;
            if (glUnit != (GLuint)u) {
                R.ActiveTexture(GL_TEXTURE0 + u);
                glUnit = (GLuint)u;
            }
            R.BindTexture(kSlotTarget[s], toRight ? ti->right : id);
        }
    }
    if (glUnit != c->activeUnit) R.ActiveTexture(GL_TEXTURE0 + c->activeUnit);
}

void TraceStep(Ctx* c);

template <class F>
void StereoDraw(F&& draw)
{
    Ctx* c = t_ctx;
    if (!c || c->listMode) {
        draw();
        return;
    }
    std::lock_guard<std::recursive_mutex> lk(g_lock);
    if (c->drawFb == 0) c->fb0Touched = true;
    ProgInfo* pi = c->program ? FindProg(c->program) : nullptr;
    int mode = pi ? pi->mode : kModeStereo;
    if (g_hunting && c->isMain) {
        if (c->program && std::find(g_frameProgs.begin(), g_frameProgs.end(), c->program) == g_frameProgs.end())
            g_frameProgs.push_back(c->program);
        if (c->program && c->program == g_huntSel) return;   // hidden while selected
    }
    if (mode == kModeSkip) return;
    if (c->isMain) {
        if (c->batchPending) {
            if (c->drawFb == c->batchFb) c->batch++;
            c->batchPending = false;
        }
        c->batchFb = c->drawFb;
        c->drawsInBatch++;
    }
    // an ARB assembly vertex program we rewrote (no GLSL program bound)
    c->arbActive = !c->program && c->arbVp && g_arbVp.count(c->arbVp) && R.IsEnabled(GL_VERTEX_PROGRAM_ARB);
    // order depth for the draws that can take it: GLSL with our geometry shader, rewritten ARB programs, and
    // fixed-function draws (their orthographic projection gets the offset)
    const bool fixedFn = !c->program && !c->arbActive && c->compat && g_cfg.fixedFunction;
    if (((pi && pi->gs) || ((c->arbActive || fixedFn) && g_cfg.flatEnable)) && g_cfg.flatRule == 1 && c->isMain) OrderDepthForDraw(c);
    else if (fixedFn && g_cfg.flatEnable && g_cfg.flatRule == 0 && c->isMain) FeetDepthForDraw(c);
    if (pi && pi->gs && t_drawMode != GL_TRIANGLES && t_drawMode != GL_TRIANGLE_STRIP && t_drawMode != GL_TRIANGLE_FAN) {
        // a geometry shader only takes triangles; points/lines would fail (TODO: GS-less twin program)
        static int logged;
        if (logged++ < 5) Log("flat depth: program %u drew primitive mode 0x%x, skipped (geometry shader takes triangles)", c->program, t_drawMode);
        return;
    }
    GLuint right = Active(c) ? RightFor(c, c->drawFb) : 0;
    EyeState st;
    if (!right) {
        EyeBegin(c, 0.0f, st);
        draw();
        g_stats.monoDraws++;
        TraceStep(c);
        return;
    }
    const float eye = mode == kModeMono ? 0.0f : 1.0f;   // mono: both eyes, no offset (screen depth)
    EyeBegin(c, -eye, st);
    draw();
    EyeEnd(st);
    R.BindFramebuffer(GL_DRAW_FRAMEBUFFER, right);
    SwapTextures(c, true);
    EyeBegin(c, eye, st);
    draw();
    EyeEnd(st);
    SwapTextures(c, false);
    R.BindFramebuffer(GL_DRAW_FRAMEBUFFER, RealFb(c, c->drawFb));
    g_stats.stereoDraws++;
    TraceStep(c);
}

// Clears and other target-only operations: both eyes, no eye offset. color: the colour buffer is
// cleared (a stencil-only clear for a mask, Machinarium, does not start a new picture).
template <class F>
void StereoTargetOp(F&& op, bool color = true)
{
    Ctx* c = t_ctx;
    if (!c || c->listMode) {
        op();
        return;
    }
    std::lock_guard<std::recursive_mutex> lk(g_lock);
    if (c->drawFb == 0) c->fb0Touched = true;
    if (g_cfg.flatEnable && g_cfg.flatRule == 1 && c->isMain && color) {   // for scratch targets (ComputeLayout)
        FlatEv ev;
        ev.fb = c->drawFb;
        ev.clear = true;
        c->flatEvents.push_back(ev);
    }
    op();
    GLuint right = Active(c) ? RightFor(c, c->drawFb) : 0;
    if (!right) return;
    R.BindFramebuffer(GL_DRAW_FRAMEBUFFER, right);
    op();
    R.BindFramebuffer(GL_DRAW_FRAMEBUFFER, RealFb(c, c->drawFb));
    g_stats.clears++;
}

// glCopyTex(Sub)Image2D: copy the right eye's read framebuffer into the destination's clone.
template <class F>
void StereoCopy(GLenum target, F&& copy)
{
    Ctx* c = t_ctx;
    copy();
    int slot = SlotOf(target);
    if (!c || c->listMode || slot < 0 || !Active(c)) return;
    std::lock_guard<std::recursive_mutex> lk(g_lock);
    GLuint rightRead = RightFor(c, c->readFb);
    GLuint dst = c->bound[c->activeUnit][slot];
    if (!rightRead || !dst) return;
    GLuint clone = TexClone(c, dst, target);
    if (!clone) return;
    R.BindFramebuffer(GL_READ_FRAMEBUFFER, rightRead);
    R.BindTexture(target, clone);
    copy();
    R.BindTexture(target, dst);
    R.BindFramebuffer(GL_READ_FRAMEBUFFER, RealFb(c, c->readFb));
    g_stats.copies++;
}

// ---------------------------------------------------------------------------------------------
// draw trace ([Debug] trace_frames): one log line per draw call in the listed frames

bool g_traceNow;              // the current frame is listed in trace_frames
unsigned g_traceDraw;         // draw index within the traced frame

bool ListedIn(const std::string& s, unsigned frame)
{
    size_t i = 0;
    while (i < s.size()) {
        size_t j = s.find(',', i);
        if (j == std::string::npos) j = s.size();
        if ((unsigned)strtoul(s.c_str() + i, nullptr, 10) == frame && j > i) return true;
        i = j + 1;
    }
    return false;
}

// first mat4 uniform of a program (usually its transform), for the trace
std::unordered_map<GLuint, std::pair<GLint, std::string>> g_traceMat;

void TraceDraw(const char* fn, GLenum mode, GLsizei count)
{
    t_drawMode = mode;
    t_drawCount = count;
    Ctx* c = t_ctx;
    if (!g_traceNow || !c || !c->isMain || c->listMode) return;
    std::lock_guard<std::recursive_mutex> lk(g_lock);
    ProgInfo* pi = c->program ? FindProg(c->program) : nullptr;
    GLint vp[4] = {}, sc[4] = {};
    R.GetIntegerv(GL_VIEWPORT, vp);
    bool scissor = R.IsEnabled(GL_SCISSOR_TEST) != 0;
    if (scissor) R.GetIntegerv(GL_SCISSOR_BOX, sc);
    bool blend = R.IsEnabled(GL_BLEND) != 0;
    GLuint tex = c->bound[0][kSlot2D];
    TexInfo* ti = tex ? FindTex(tex) : nullptr;
    char mat[160] = "";
    if (c->program && R.GetActiveUniform && R.GetUniformfv) {
        auto it = g_traceMat.find(c->program);
        if (it == g_traceMat.end()) {
            std::pair<GLint, std::string> m{-1, ""};
            GLint n = 0;
            R.GetProgramiv(c->program, GL_ACTIVE_UNIFORMS, &n);
            for (GLint i = 0; i < n && m.first < 0; ++i) {
                char name[128];
                GLsizei len = 0;
                GLint size = 0;
                GLenum type = 0;
                R.GetActiveUniform(c->program, (GLuint)i, sizeof(name), &len, &size, &type, name);
                if (type == GL_FLOAT_MAT4 && strncmp(name, "gl_", 3) != 0) m = {R.GetUniformLocation(c->program, name), name};
            }
            it = g_traceMat.emplace(c->program, m).first;
        }
        if (it->second.first >= 0) {
            GLfloat m[16];
            R.GetUniformfv(c->program, it->second.first, m);
            snprintf(mat, sizeof(mat), " %s=[%.4f %.4f %.4f %.4f | %.4f %.4f %.4f]", it->second.second.c_str(), m[0], m[5],
                     m[1], m[4], m[12], m[13], m[14]);
        }
    }
    FboInfo* fi = nullptr;
    if (c->drawFb) {
        auto f = c->fbo.find(c->drawFb);
        if (f != c->fbo.end()) fi = &f->second;
    }
    std::string sampled;   // units the program reads: texture (size), v = view of, R = has a right-eye clone
    const uint32_t units = SampledUnits(c);
    for (int u = 0; u <= c->maxUnit && u < 32; ++u) {
        if (!((units >> u) & 1)) continue;
        for (int sl = 0; sl < kSlotCount; ++sl) {
            const GLuint id = c->bound[u][sl];
            if (!id) continue;
            TexInfo* t2 = FindTex(id);
            TexInfo* o2 = t2 && t2->viewOf ? FindTex(t2->viewOf) : nullptr;
            char buf[80];
            sprintf_s(buf, " u%d/s%d:%u(%dx%d)%s%s", u, sl, id, t2 ? t2->w : -1, t2 ? t2->h : -1,
                      t2 && t2->viewOf ? (std::string("v") + std::to_string(t2->viewOf)).c_str() : "",
                      (t2 && t2->right) || (o2 && o2->right) ? "R" : "");
            sampled += buf;
        }
    }
    Log("T %u.%u b%d %s mode=%x n=%d prog=%u/%08x%s%s fb=%u%s(t%u) vp=%d,%d,%d,%d%s sc=%d,%d,%d,%d blend=%d tex=%u(%dx%d)%s", g_frame,
        g_traceDraw++, c->batch, fn, mode, count, c->program, pi ? pi->hash : 0, pi && pi->loc < 0 ? "(noinj)" : "",
        pi && pi->gs ? "(gs)" : c->arbActive ? "(arbvp)" : "", c->drawFb,
        c->drawFb == 0 || (fi && fi->stereo) ? "S" : "", fi ? fi->att[0].id : 0, vp[0], vp[1], vp[2], vp[3], scissor ? "" : "(off)", sc[0], sc[1],
        sc[2], sc[3], blend, tex, ti ? ti->w : 0, ti ? ti->h : 0, mat);
    if (!sampled.empty() || units != ~0u) Log("T %u.%u   samples%s%s", g_frame, g_traceDraw - 1, sampled.c_str(), units == ~0u ? " (all units)" : "");
}

// ---------------------------------------------------------------------------------------------
// composition, hotkeys, frame dumps

void EnsureLeftCopy(Ctx* c)
{
    if (c->leftCopyFbo && c->leftCopyW == c->winW && c->leftCopyH == c->winH) return;
    if (c->leftCopyFbo) {
        R.DeleteFramebuffers(1, &c->leftCopyFbo);
        R.DeleteTextures(1, &c->leftCopyTex);
    }
    GLenum fmt = GL_RGBA8;
    if (c->rightDefColor) {
        GLint f = 0;
        R.BindTexture(GL_TEXTURE_2D, c->rightDefColor);
        R.GetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_INTERNAL_FORMAT, &f);
        if (f) fmt = (GLenum)f;
    }
    R.GenTextures(1, &c->leftCopyTex);
    R.BindTexture(GL_TEXTURE_2D, c->leftCopyTex);
    R.TexImage2D(GL_TEXTURE_2D, 0, (GLint)fmt, c->winW, c->winH, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    R.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    R.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    R.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
    R.BindTexture(GL_TEXTURE_2D, c->bound[c->activeUnit][kSlot2D]);
    R.GenFramebuffers(1, &c->leftCopyFbo);
    R.BindFramebuffer(GL_DRAW_FRAMEBUFFER, c->leftCopyFbo);
    R.FramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, c->leftCopyTex, 0);
    R.BindFramebuffer(GL_DRAW_FRAMEBUFFER, RealFb(c, c->drawFb));
    c->leftCopyW = c->winW;
    c->leftCopyH = c->winH;
    c->leftCopyValid = false;
}

// Creates (or resizes) the virtual back buffer and starts it with the current left eye; from the
// next frame on the game's framebuffer 0 is redirected to it (RealFb).
void EnsureVbb(Ctx* c)
{
    if (c->vbbFailed || c->winW <= 0 || c->winH <= 0) return;
    if (c->vbbFbo && c->vbbW == c->winW && c->vbbH == c->winH) {
        c->vbbLive = true;
        return;
    }
    R.BindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
    GLint samples = 0, enc = 0;
    R.GetIntegerv(GL_SAMPLES, &samples);
    if (samples > 0) {
        Log("virtual back buffer off: the default framebuffer is multisampled (%d samples)", samples);
        c->vbbFailed = true;
        c->vbbLive = false;
        return;
    }
    if (R.GetFramebufferAttachmentParameteriv)
        R.GetFramebufferAttachmentParameteriv(GL_DRAW_FRAMEBUFFER, GL_BACK_LEFT, GL_FRAMEBUFFER_ATTACHMENT_COLOR_ENCODING, &enc);
    const GLenum colorFmt = enc == (GLint)GL_SRGB ? GL_SRGB8_ALPHA8 : GL_RGBA8;
    if (c->vbbFbo) {
        R.DeleteFramebuffers(1, &c->vbbFbo);
        R.DeleteTextures(1, &c->vbbColor);
        R.DeleteRenderbuffers(1, &c->vbbDepth);
        c->vbbFbo = c->vbbColor = c->vbbDepth = 0;
    }
    GLint unpack = 0;
    R.GetIntegerv(GL_PIXEL_UNPACK_BUFFER_BINDING, &unpack);
    if (unpack) R.BindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
    R.GenTextures(1, &c->vbbColor);
    R.BindTexture(GL_TEXTURE_2D, c->vbbColor);
    R.TexImage2D(GL_TEXTURE_2D, 0, (GLint)colorFmt, c->winW, c->winH, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    R.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    R.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    R.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
    R.BindTexture(GL_TEXTURE_2D, c->bound[c->activeUnit][kSlot2D]);
    if (unpack) R.BindBuffer(GL_PIXEL_UNPACK_BUFFER, (GLuint)unpack);
    R.GenRenderbuffers(1, &c->vbbDepth);
    R.BindRenderbuffer(GL_RENDERBUFFER, c->vbbDepth);
    R.RenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, c->winW, c->winH);
    R.BindRenderbuffer(GL_RENDERBUFFER, c->rbBinding);
    R.GenFramebuffers(1, &c->vbbFbo);
    R.BindFramebuffer(GL_DRAW_FRAMEBUFFER, c->vbbFbo);
    R.FramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, c->vbbColor, 0);
    R.FramebufferRenderbuffer(GL_DRAW_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, c->vbbDepth);
    R.DrawBuffer(VbbBuffer(c->fb0DrawBuf));
    const GLenum st = R.CheckFramebufferStatus(GL_DRAW_FRAMEBUFFER);
    if (st != GL_FRAMEBUFFER_COMPLETE) {
        Log("virtual back buffer off: framebuffer incomplete (0x%x)", st);
        R.BindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
        R.DeleteFramebuffers(1, &c->vbbFbo);
        R.DeleteTextures(1, &c->vbbColor);
        R.DeleteRenderbuffers(1, &c->vbbDepth);
        c->vbbFbo = c->vbbColor = c->vbbDepth = 0;
        c->vbbFailed = true;
        c->vbbLive = false;
        return;
    }
    R.BindFramebuffer(GL_READ_FRAMEBUFFER, c->vbbFbo);
    R.ReadBuffer(VbbBuffer(c->fb0ReadBuf));
    // start with the frame just shown (a game may draw only part of the next one)
    R.BindFramebuffer(GL_READ_FRAMEBUFFER, c->leftCopyFbo);
    R.BlitFramebuffer(0, 0, c->winW, c->winH, 0, 0, c->winW, c->winH, GL_COLOR_BUFFER_BIT, GL_NEAREST);
    c->vbbW = c->winW;
    c->vbbH = c->winH;
    c->vbbLive = true;
    Log("virtual back buffer %dx%d (%s): the game draws off-screen, the window only gets finished frames", c->vbbW,
        c->vbbH, colorFmt == GL_SRGB8_ALPHA8 ? "sRGB" : "RGBA8");
}

// Side-by-side input for the SR weaver: left eye in [0, W), right eye in [W, 2W).
void EnsureSbs(Ctx* c)
{
    if (c->sbsFbo && c->sbsW == c->winW && c->sbsH == c->winH) return;
    if (c->sbsFbo) {
        R.DeleteFramebuffers(1, &c->sbsFbo);
        R.DeleteTextures(1, &c->sbsTex);
    }
    R.GenTextures(1, &c->sbsTex);
    R.BindTexture(GL_TEXTURE_2D, c->sbsTex);
    R.TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, c->winW * 2, c->winH, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    R.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    R.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    R.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
    R.BindTexture(GL_TEXTURE_2D, c->bound[c->activeUnit][kSlot2D]);
    R.GenFramebuffers(1, &c->sbsFbo);
    R.BindFramebuffer(GL_DRAW_FRAMEBUFFER, c->sbsFbo);
    R.FramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, c->sbsTex, 0);
    R.BindFramebuffer(GL_DRAW_FRAMEBUFFER, RealFb(c, c->drawFb));
    c->sbsW = c->winW;
    c->sbsH = c->winH;
}

// Saves and restores the GL state our own drawing (overlay) and the SR weaver change. The weaver's
// GL calls reach our hooks too (it imports opengl32.dll), so they run with t_ctx == nullptr (pure
// pass-through) and the game's tracked state is put back here. Create it with the default
// framebuffer bound for draw and read: PopAttrib restores draw/read buffers of the bound FB.
struct GlStateGuard {
    // Per-fragment state the game may leave on at the end of its frame. Our drawing (the SR weaver's
    // full-screen pass, the overlay) starts with all of it off: Broken Age ends some frames with a
    // multiply or additive blend still on, which blended the woven image into the previous one and
    // ran the screen to black (or white) until the game's last draw changed.
    static constexpr GLenum kEnables[12] = {GL_BLEND,          GL_DEPTH_TEST,     GL_CULL_FACE,
                                            GL_SCISSOR_TEST,   GL_STENCIL_TEST,   GL_FRAMEBUFFER_SRGB,
                                            GL_COLOR_LOGIC_OP, GL_SAMPLE_ALPHA_TO_COVERAGE,
                                            GL_RASTERIZER_DISCARD, GL_CLIP_DISTANCE0, GL_CLIP_DISTANCE0 + 1,
                                            GL_CLIP_DISTANCE0 + 2};
    Ctx* c;
    GLint viewport[4] = {}, vao = 0, arrayBuf = 0, elemBuf = 0, unpackBuf = 0, packBuf = 0, samplers[4] = {};
    GLint blend[4] = {}, blendEq[2] = {GL_FUNC_ADD, GL_FUNC_ADD}, polygonMode[2] = {GL_FILL, GL_FILL};
    GLboolean enables[12] = {}, colorMask[4] = {1, 1, 1, 1}, depthMask = 1;
    GLint clipOrigin = GL_LOWER_LEFT, clipDepth = GL_NEGATIVE_ONE_TO_ONE;

    explicit GlStateGuard(Ctx* ctx) : c(ctx)
    {
        R.BindFramebuffer(GL_FRAMEBUFFER, 0);
        if (R.ClipControl) {
            // WineD3D keeps the D3D convention (origin upper left): our full-screen passes (SR weaver,
            // overlay) are written for GL's and came out upside down
            R.GetIntegerv(GL_CLIP_ORIGIN, &clipOrigin);
            R.GetIntegerv(GL_CLIP_DEPTH_MODE, &clipDepth);
            if (clipOrigin != GL_LOWER_LEFT || clipDepth != GL_NEGATIVE_ONE_TO_ONE) R.ClipControl(GL_LOWER_LEFT, GL_NEGATIVE_ONE_TO_ONE);
        }
        for (int i = 0; i < 12; ++i) enables[i] = R.IsEnabled(kEnables[i]);   // (rasterizer discard is in no attrib group)
        if (c->compat) {
            R.PushAttrib(GL_ALL_ATTRIB_BITS);
            R.PushClientAttrib(GL_CLIENT_ALL_ATTRIB_BITS);
        } else {
            R.GetIntegerv(GL_POLYGON_MODE, polygonMode);
            R.GetBooleanv(GL_COLOR_WRITEMASK, colorMask);
            R.GetBooleanv(GL_DEPTH_WRITEMASK, &depthMask);
            R.GetIntegerv(GL_BLEND_SRC_RGB, &blend[0]);
            R.GetIntegerv(GL_BLEND_DST_RGB, &blend[1]);
            R.GetIntegerv(GL_BLEND_SRC_ALPHA, &blend[2]);
            R.GetIntegerv(GL_BLEND_DST_ALPHA, &blend[3]);
            R.GetIntegerv(GL_BLEND_EQUATION_RGB, &blendEq[0]);
            R.GetIntegerv(GL_BLEND_EQUATION_ALPHA, &blendEq[1]);
        }
        R.GetIntegerv(GL_VIEWPORT, viewport);
        R.GetIntegerv(GL_VERTEX_ARRAY_BINDING, &vao);
        R.GetIntegerv(GL_ARRAY_BUFFER_BINDING, &arrayBuf);
        R.GetIntegerv(GL_ELEMENT_ARRAY_BUFFER_BINDING, &elemBuf);
        R.GetIntegerv(GL_PIXEL_UNPACK_BUFFER_BINDING, &unpackBuf);
        R.GetIntegerv(GL_PIXEL_PACK_BUFFER_BINDING, &packBuf);
        if (R.BindSampler) {
            for (int u = 0; u < 4; ++u) {
                R.ActiveTexture(GL_TEXTURE0 + u);
                R.GetIntegerv(GL_SAMPLER_BINDING, &samplers[u]);
            }
            R.ActiveTexture(GL_TEXTURE0 + c->activeUnit);
        }
        for (GLenum e : kEnables) R.Disable(e);
        if (c->compat) R.Disable(GL_ALPHA_TEST);   // applies after the fragment shader in compatibility contexts
        R.PolygonMode(GL_FRONT_AND_BACK, GL_FILL);
        R.ColorMask(1, 1, 1, 1);
    }

    ~GlStateGuard()
    {
        R.BindFramebuffer(GL_FRAMEBUFFER, 0);   // PopAttrib restores the draw/read buffer of the bound FB
        if (c->compat) {
            R.PopClientAttrib();
            R.PopAttrib();
            if (enables[8]) R.Enable(GL_RASTERIZER_DISCARD);
        } else {
            for (int i = 0; i < 12; ++i) {
                if (enables[i]) R.Enable(kEnables[i]);
                else R.Disable(kEnables[i]);
            }
            R.PolygonMode(GL_FRONT_AND_BACK, polygonMode[0]);
            R.ColorMask(colorMask[0], colorMask[1], colorMask[2], colorMask[3]);
            R.DepthMask(depthMask);
            if (R.BlendFuncSeparate) R.BlendFuncSeparate(blend[0], blend[1], blend[2], blend[3]);
            if (R.BlendEquationSeparate) R.BlendEquationSeparate(blendEq[0], blendEq[1]);
        }
        R.UseProgram(c->program);
        if (R.BindVertexArray) R.BindVertexArray(vao);
        if (vao || c->compat) R.BindBuffer(GL_ELEMENT_ARRAY_BUFFER, elemBuf);
        R.BindBuffer(GL_ARRAY_BUFFER, arrayBuf);
        R.BindBuffer(GL_PIXEL_UNPACK_BUFFER, unpackBuf);
        R.BindBuffer(GL_PIXEL_PACK_BUFFER, packBuf);
        for (int u = 0; u <= std::max(c->maxUnit, 3); ++u) {
            R.ActiveTexture(GL_TEXTURE0 + u);
            for (int s = 0; s < kSlotCount; ++s) R.BindTexture(kSlotTarget[s], c->bound[u][s]);
            if (u < 4 && R.BindSampler) R.BindSampler(u, samplers[u]);
        }
        R.ActiveTexture(GL_TEXTURE0 + c->activeUnit);
        R.BindRenderbuffer(GL_RENDERBUFFER, c->rbBinding);
        R.Viewport(viewport[0], viewport[1], viewport[2], viewport[3]);
        if (R.ClipControl && (clipOrigin != GL_LOWER_LEFT || clipDepth != GL_NEGATIVE_ONE_TO_ONE))
            R.ClipControl(clipOrigin, clipDepth);
    }
};

// ---------------------------------------------------------------------------------------------
// Auto convergence ("convergence = auto"). Every few frames both eyes are shrunk on the GPU and
// read back through a PBO (two slots, so mapping never waits for the GPU). Block matching gives
// each block's disparity, which converts to its depth using the convergence/separation that frame
// was rendered with: d = sep * (1 - conv / depth). The nearest significant content (15th
// percentile) is then eased onto the screen plane, so it works without knowing the game's units.

// Convergence / separation the current frame is being rendered with: snapshot taken when the
// previous frame was presented, before hotkeys or auto convergence change them for the next one.
float g_frameConv = NAN, g_frameSep = NAN;

char g_frameKind = '?';        // what the game did this frame (set by OnSwapBuffers before Compose)
bool g_dumpAuto;               // [Debug] dump_dark: save both eyes at the next present
std::string g_framePattern;    // kinds of the last 120 frames
double g_frameMs = 0;          // time since the previous SwapBuffers

// True when the GPU has finished everything up to the fence (no fence: assume so). Never waits:
// mapping a read-back buffer the GPU has not filled yet would block the game (seen in upscale
// mode at start-up, when the GPU queue waits behind the D3D11 presentation).
bool FenceDone(GLsync& f)
{
    if (!f || !R.ClientWaitSync) return true;
    const GLenum r = R.ClientWaitSync(f, 0, 0);
    if (r == GL_TIMEOUT_EXPIRED) return false;
    R.DeleteSync(f);
    f = nullptr;
    return true;
}

GLsync NewFence()
{
    return R.FenceSync ? R.FenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0) : nullptr;
}

struct AutoConv {
    GLuint fbo[2] = {}, tex[2] = {};   // shrunk left / right eye
    int w = 0, h = 0;
    GLuint pbo[2] = {};
    GLsync fence[2] = {};
    bool pending[2] = {};
    unsigned issued[2] = {};           // frame the read back was issued in
    float conv[2] = {}, sep[2] = {};   // values the read-back image was rendered with
    unsigned lastSerial = 0, lastIssue = 0, logged = 0;
    float hist[5] = {};                // recent estimates (log depth): the median drives convergence
    int histN = 0, histPos = 0;
    int lostRun = 0;                   // consecutive measurements with the content out of reach
    int lostSteps = 0;                 // convergence steps taken since the last good measurement
    float lastGood = 0;                // convergence after the last well-matched measurement
    bool everGood = false;             // a well-matched measurement happened in this session
    float loggedConv = 0;
} g_ac;

// A block can only be matched horizontally if it varies horizontally: flat blocks and blocks
// with only horizontal edges match equally well at any horizontal offset (aperture problem).
bool HasHorizontalTexture(const int* lum, int w, int x0, int y0, int bs)
{
    long long grad = 0;
    for (int y = 0; y < bs; ++y) {
        const int* row = &lum[(size_t)(y0 + y) * w + x0];
        for (int x = 0; x + 1 < bs; ++x) grad += std::abs(row[x + 1] - row[x]);
    }
    return grad / (bs * (bs - 1)) >= 6;   // mean |horizontal gradient| on the 0..1020 luminance scale
}

// The best offset is not clearly better than another, non-adjacent one (relative or absolute
// margin; ties count, e.g. a perfect match at many offsets).
bool MatchAmbiguous(const std::vector<long long>& sad, int best, int bs)
{
    long long second = -1;
    for (int i = 0; i < (int)sad.size(); ++i)
        if (sad[i] >= 0 && std::abs(i - best) > 1 && (second < 0 || sad[i] < second)) second = sad[i];
    if (second < 0) return false;
    return second * 100 < sad[best] * 112 || second - sad[best] < 2LL * bs * bs;
}

// Depth of the nearest significant content, or NaN. sepSmall = disparity at infinity in pixels of
// the shrunk image.
float EstimateNearDepth(const uint8_t* L, const uint8_t* Rt, int w, int h, float conv, float sepSmall, int& reliable,
                        int& ambiguous, int& identical)
{
    reliable = ambiguous = identical = 0;
    if (sepSmall < 0.5f) return NAN;
    std::vector<int> yl((size_t)w * h), yr((size_t)w * h);
    for (size_t i = 0; i < (size_t)w * h; ++i) {
        yl[i] = L[i * 4] + 2 * L[i * 4 + 1] + L[i * 4 + 2];
        yr[i] = Rt[i * 4] + 2 * Rt[i * 4 + 1] + Rt[i * 4 + 2];
    }
    const int bs = 12;
    // Disparity (right - left) is at most sepSmall behind the screen (infinity) but has no limit in
    // front of it: search out to content seven times nearer than the convergence. Index = d + maxNeg.
    const int maxNeg = std::min(w / 2, (int)std::ceil(6.0f * sepSmall) + 3);
    const int maxPos = std::min(w / 3, (int)std::ceil(1.25f * sepSmall) + 3);
    std::vector<long long> sad(maxNeg + maxPos + 1);
    std::vector<float> depths;
    for (int y0 = 4; y0 + bs <= h - 4; y0 += bs) {
        for (int x0 = 4; x0 + bs <= w - 4; x0 += bs) {
            if (!HasHorizontalTexture(yl.data(), w, x0, y0, bs)) continue;   // flat, or horizontal edges only
            int best = -1;
            for (int d = -maxNeg; d <= maxPos; ++d) {
                long long& v = sad[d + maxNeg];
                v = -1;
                if (x0 + d < 0 || x0 + d + bs > w) continue;
                // past this a partial sum can neither win nor make the best match ambiguous
                const long long bound = best < 0 ? LLONG_MAX : sad[best] * 112 / 100 + 2LL * bs * bs;
                v = 0;
                for (int y = 0; y < bs && v <= bound; ++y) {
                    const int* a = &yl[(size_t)(y0 + y) * w + x0];
                    const int* b = &yr[(size_t)(y0 + y) * w + x0 + d];
                    for (int x = 0; x < bs; ++x) v += std::abs(a[x] - b[x]);
                }
                if (best < 0 || v < sad[best]) best = d + maxNeg;
            }
            if (best < 0) continue;
            // identical in both eyes (HUD / 2D): offset 0 matches as well as the best offset
            if (sad[maxNeg] >= 0 && sad[maxNeg] <= sad[best] + (long long)bs * bs) {
                identical++;
                continue;
            }
            if (MatchAmbiguous(sad, best, bs)) {
                ambiguous++;
                continue;
            }
            float D = (float)(best - maxNeg);
            if (best > 0 && best + 1 < (int)sad.size() && sad[best - 1] >= 0 && sad[best + 1] >= 0) {
                double a = (double)sad[best - 1], b = (double)sad[best], c2 = (double)sad[best + 1];
                double den = a - 2 * b + c2;
                if (den > 0) D += (float)(0.5 * (a - c2) / den);   // sub-pixel
            }
            reliable++;
            depths.push_back(D >= 0.97f * sepSmall ? conv * 40.0f : conv / (1.0f - D / sepSmall));
        }
    }
    if (depths.size() < 6) return NAN;
    std::sort(depths.begin(), depths.end());
    return depths[depths.size() * 15 / 100];
}

// Block matching takes 1-10 ms per measurement, so it runs on a worker thread: the render thread only
// maps the read-back buffer, copies it (~1 MB) and later applies the result.
struct AcWorker {
    std::mutex m;
    std::condition_variable cv;
    bool started = false, job = false, done = false;
    std::vector<uint8_t> px;           // left eye, then right eye (w * h * 4 each)
    int w = 0, h = 0;
    float conv = 0, sepSmall = 0;
    float nearDepth = NAN;
    int reliable = 0, ambiguous = 0, identical = 0;
    double ms = 0;
    float meanL = 0;                   // mean left-eye luminance (0..255), for diagnosis
} g_acw;

DWORD WINAPI AcWorkerMain(void*)
{
    std::vector<uint8_t> px;
    for (;;) {
        int w, h;
        float conv, sepSmall;
        {
            std::unique_lock<std::mutex> lk(g_acw.m);
            g_acw.cv.wait(lk, [] { return g_acw.job; });
            px.swap(g_acw.px);
            w = g_acw.w;
            h = g_acw.h;
            conv = g_acw.conv;
            sepSmall = g_acw.sepSmall;
        }
        LARGE_INTEGER t0, t1, qf;
        QueryPerformanceCounter(&t0);
        int reliable = 0, ambiguous = 0, identical = 0;
        const float nearDepth = EstimateNearDepth(px.data(), px.data() + (size_t)w * h * 4, w, h, conv, sepSmall, reliable,
                                                  ambiguous, identical);
        QueryPerformanceCounter(&t1);
        QueryPerformanceFrequency(&qf);
        double lum = 0;
        for (size_t i = 0; i < (size_t)w * h; ++i) lum += px[i * 4] + 2 * px[i * 4 + 1] + px[i * 4 + 2];
        std::lock_guard<std::mutex> lk(g_acw.m);
        g_acw.meanL = (float)(lum / (4.0 * w * h));
        g_acw.px.swap(px);   // the buffer goes back for the next copy
        g_acw.nearDepth = nearDepth;
        g_acw.reliable = reliable;
        g_acw.ambiguous = ambiguous;
        g_acw.identical = identical;
        g_acw.ms = (t1.QuadPart - t0.QuadPart) * 1000.0 / qf.QuadPart;
        g_acw.job = false;
        g_acw.done = true;
    }
}

// Calm in steady state, quick on real scene changes: the median of the last five estimates is the
// target (single noisy readings do nothing), changes under ~4% are ignored (no constant small
// shifts that read as flicker), small corrections are gentle and only large, consistent ones
// (camera cuts) take big steps. auto_speed scales the steps.
void ApplyAutoConvergence(float nearDepth, int reliable, int ambiguous, int identical)
{
    if (g_ac.lastGood <= 0) g_ac.lastGood = g_cfg.convergence;   // the start value bounds the first drops
    const bool measured = !std::isnan(nearDepth) && nearDepth > 0;
    // Lost: almost nothing measurable that is not 2D (HUD, video, logos), or mostly unmatched blocks.
    // A convergence far too large for the shot (a cut from a wide shot to a close-up) pushes the
    // content of each eye out to the sides by up to a third of the screen: what remains is the
    // backdrop, often plain black or very bright, and nothing within matching range. Only a smaller
    // convergence brings the content back, so step down hard, past the median. On black or white
    // fades (the other way to get here) the step is invisible.
    // (not before the first good measurement: black loading screens at start-up would throw away the
    // remembered start value)
    const bool flat = g_ac.everGood && reliable + ambiguous < 6 && identical < 20;
    const bool unmatched = ambiguous >= 6 && ambiguous > 3 * reliable;
    if (flat || unmatched) {
        if (++g_ac.lostRun < 2) return;   // one odd frame is not enough
        g_ac.lostRun = 0;
        // Plain black / white (a fade, or only backdrop left): first down to a quarter of the last good
        // value, which brings content up to 28x nearer into matching range and keeps the step small
        // after ordinary fades; further only if it stays unmeasurable. Visible but unmatched content is
        // proof of a convergence far too large: no such stop.
        const float floor = g_ac.lastGood / (unmatched || ++g_ac.lostSteps > 6 ? 32.0f : 4.0f);
        const float conv = std::max(floor, g_cfg.convergence * 0.4f);
        if (conv >= g_cfg.convergence * 0.98f) return;
        g_cfg.convergence = conv;
        g_paramVer++;
        g_ac.histN = g_ac.histPos = 0;   // the next real measurement takes over at once
        Log("auto convergence: content out of reach (%d blocks, %d ambiguous, %d 2D) -> convergence %.4g", reliable,
            ambiguous, identical, conv);
        g_ac.loggedConv = conv;
        return;
    }
    g_ac.lostRun = 0;
    if (!measured) return;
    g_ac.lostSteps = 0;
    const float estimate = nearDepth * (1.0f + g_cfg.autoPopout * 0.01f);
    g_ac.hist[g_ac.histPos] = std::log(estimate);
    g_ac.histPos = (g_ac.histPos + 1) % 5;
    g_ac.histN = std::min(g_ac.histN + 1, 5);
    float sorted[5];
    std::copy(g_ac.hist, g_ac.hist + g_ac.histN, sorted);
    std::sort(sorted, sorted + g_ac.histN);
    const float target = std::exp(sorted[g_ac.histN / 2]);

    if (reliable >= 20 && reliable > ambiguous) {
        g_ac.lastGood = target;
        g_ac.everGood = true;
    }
    const float lr = std::log(target / g_cfg.convergence);
    if (std::fabs(lr) < 0.04f) return;   // deadband
    const float gain = std::clamp((std::fabs(lr) < 0.3f ? 0.2f : 0.45f) * g_cfg.autoSpeed, 0.05f, 0.9f);
    const float conv = std::clamp(g_cfg.convergence * std::exp(gain * std::clamp(lr, -1.2f, 1.2f)), 1e-4f, 1e7f);
    g_cfg.convergence = conv;
    g_paramVer++;
    const bool bigChange = g_ac.loggedConv <= 0 || std::fabs(std::log(conv / g_ac.loggedConv)) > 0.15f;
    if (++g_ac.logged <= 8 || g_cfg.logLevel >= 2 || bigChange) {
        Log("auto convergence: near depth %.4g (median of %d: %.4g; %d blocks, %d ambiguous) -> convergence %.4g",
            nearDepth, g_ac.histN, target, reliable, ambiguous, conv);
        g_ac.loggedConv = conv;
    }
}

// Called from Compose (scissor / sRGB / color mask already neutral), after both eyes are final.
void AutoConvergenceStep(Ctx* c)
{
    if (!g_cfg.autoConvergence || !g_cfg.enable || !c->rightDefFbo || !c->leftCopyFbo || !R.MapBuffer ||
        !R.GenBuffers || !R.BufferData)
        return;
    // a measurement the worker finished
    bool workerBusy;
    {
        std::unique_lock<std::mutex> lk(g_acw.m);
        if (g_acw.done) {
            g_acw.done = false;
            const float nearDepth = g_acw.nearDepth, conv = g_acw.conv;
            const int reliable = g_acw.reliable, ambiguous = g_acw.ambiguous, identical = g_acw.identical;
            const double ms = g_acw.ms;
            const float meanL = g_acw.meanL;
            lk.unlock();
            LOGV("auto convergence measure: near %.4g, %d blocks, %d ambiguous, %d 2D, luma %.1f (conv %.4g, %.2f ms)",
                 nearDepth, reliable, ambiguous, identical, meanL, conv, ms);
            ApplyAutoConvergence(nearDepth, reliable, ambiguous, identical);
            static int darkDumps;
            static unsigned lastDarkDump;
            if (g_cfg.dumpDark > darkDumps && g_ac.everGood && reliable + ambiguous < 6 && identical < 20 &&
                (darkDumps == 0 || g_frame - lastDarkDump > 300)) {
                darkDumps++;
                lastDarkDump = g_frame;
                g_dumpAuto = true;
                Log("flat / dark picture (frame %u, last frames %s): saving both eyes", g_frame, g_framePattern.c_str());
            }
            lk.lock();
        }
        workerBusy = g_acw.job || g_acw.done;
    }
    const bool due = std::any_of(g_ac.pending, g_ac.pending + 2, [](bool p) { return p; }) ||
                     (c->imageSerial != g_ac.lastSerial && g_frame - g_ac.lastIssue >= 2);
    if (!due) return;
    const int W = c->winW, H = c->winH;
    const int w = std::max(64, W / 8), h = std::max(36, H / 8);
    GLint packBuf = 0, align = 4, rowLen = 0, skipRows = 0, skipPix = 0;
    R.GetIntegerv(GL_PIXEL_PACK_BUFFER_BINDING, &packBuf);
    if (w != g_ac.w || h != g_ac.h) {
        if (!g_ac.tex[0]) {
            R.GenTextures(2, g_ac.tex);
            R.GenFramebuffers(2, g_ac.fbo);
            R.GenBuffers(2, g_ac.pbo);
        }
        for (int i = 0; i < 2; ++i) {
            R.BindTexture(GL_TEXTURE_2D, g_ac.tex[i]);
            R.TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
            R.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
            R.BindFramebuffer(GL_DRAW_FRAMEBUFFER, g_ac.fbo[i]);
            R.FramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, g_ac.tex[i], 0);
            R.BindBuffer(GL_PIXEL_PACK_BUFFER, g_ac.pbo[i]);
            R.BufferData(GL_PIXEL_PACK_BUFFER, (GLsizeiptr)w * h * 8, nullptr, GL_STREAM_READ);
            if (g_ac.fence[i]) R.DeleteSync(g_ac.fence[i]);
            g_ac.fence[i] = nullptr;
            g_ac.pending[i] = false;
        }
        R.BindTexture(GL_TEXTURE_2D, c->bound[c->activeUnit][kSlot2D]);
        g_ac.w = w;
        g_ac.h = h;
    }
    R.GetIntegerv(GL_PACK_ALIGNMENT, &align);
    R.GetIntegerv(GL_PACK_ROW_LENGTH, &rowLen);
    R.GetIntegerv(GL_PACK_SKIP_ROWS, &skipRows);
    R.GetIntegerv(GL_PACK_SKIP_PIXELS, &skipPix);
    R.PixelStorei(GL_PACK_ALIGNMENT, 4);
    R.PixelStorei(GL_PACK_ROW_LENGTH, 0);
    R.PixelStorei(GL_PACK_SKIP_ROWS, 0);
    R.PixelStorei(GL_PACK_SKIP_PIXELS, 0);

    // 1. read backs issued at least two frames ago are finished on the GPU: map without waiting
    for (int s = 0; s < 2; ++s) {
        if (!g_ac.pending[s] || g_frame - g_ac.issued[s] < 2 || workerBusy) continue;
        if (!FenceDone(g_ac.fence[s])) {   // not on the GPU yet: try again next frame, give up after a while
            if (g_frame - g_ac.issued[s] > 120) {
                R.DeleteSync(g_ac.fence[s]);
                g_ac.fence[s] = nullptr;
                g_ac.pending[s] = false;
            }
            continue;
        }
        R.BindBuffer(GL_PIXEL_PACK_BUFFER, g_ac.pbo[s]);
        if (const uint8_t* p = (const uint8_t*)R.MapBuffer(GL_PIXEL_PACK_BUFFER, GL_READ_ONLY)) {
            {
                std::lock_guard<std::mutex> lk(g_acw.m);
                g_acw.px.assign(p, p + (size_t)w * h * 8);
                g_acw.w = w;
                g_acw.h = h;
                g_acw.conv = g_ac.conv[s];
                g_acw.sepSmall = g_ac.sep[s] * w;
                g_acw.job = true;
            }
            g_acw.cv.notify_one();
            workerBusy = true;
            if (!g_acw.started) {
                g_acw.started = true;
                if (HANDLE t = CreateThread(nullptr, 0, AcWorkerMain, nullptr, 0, nullptr)) CloseHandle(t);
            }
            R.UnmapBuffer(GL_PIXEL_PACK_BUFFER);
        }
        g_ac.pending[s] = false;
    }
    // 2. a new eye image (the game drew a frame) goes into a free slot. It is tagged with the values
    // it was rendered with: on frames the game does not draw, the images are older than the current
    // convergence, and a mismatched tag biases the estimate in the direction of the last change.
    const int s = !g_ac.pending[0] ? 0 : !g_ac.pending[1] ? 1 : -1;
    if (s >= 0 && c->imageSerial != g_ac.lastSerial && g_frame - g_ac.lastIssue >= 2) {
        const GLuint src[2] = {LeftSrc(c), c->rightDefFbo};
        for (int e = 0; e < 2; ++e) {
            R.BindFramebuffer(GL_READ_FRAMEBUFFER, src[e]);
            R.BindFramebuffer(GL_DRAW_FRAMEBUFFER, g_ac.fbo[e]);
            R.BlitFramebuffer(0, 0, W, H, 0, 0, w, h, GL_COLOR_BUFFER_BIT, GL_LINEAR);
        }
        R.BindBuffer(GL_PIXEL_PACK_BUFFER, g_ac.pbo[s]);
        for (int e = 0; e < 2; ++e) {
            R.BindFramebuffer(GL_READ_FRAMEBUFFER, g_ac.fbo[e]);
            R.ReadPixels(0, 0, w, h, GL_BGRA, GL_UNSIGNED_BYTE, (void*)((size_t)e * w * h * 4));
        }
        g_ac.fence[s] = NewFence();
        g_ac.pending[s] = true;
        g_ac.issued[s] = g_frame;
        g_ac.conv[s] = c->imageConv;
        g_ac.sep[s] = c->imageSep;
        g_ac.lastSerial = c->imageSerial;
        g_ac.lastIssue = g_frame;
    }

    R.BindBuffer(GL_PIXEL_PACK_BUFFER, (GLuint)packBuf);
    R.PixelStorei(GL_PACK_ALIGNMENT, align);
    R.PixelStorei(GL_PACK_ROW_LENGTH, rowLen);
    R.PixelStorei(GL_PACK_SKIP_ROWS, skipRows);
    R.PixelStorei(GL_PACK_SKIP_PIXELS, skipPix);
}

// ---------------------------------------------------------------------------------------------
// Flicker probe ([Debug] flicker_probe = 1). Every frame the left eye, the right eye and the
// composed back buffer are halved (kept in a GPU ring of recent frames) and box-filtered further
// down to ~120 px wide, then read back through a PBO ring and analysed a few frames later. A frame
// whose image differs from both neighbours while the neighbours agree with each other (A-B-A) is
// a flicker: it is logged with what the game did in those frames, and a sheet of the three frames
// (rows) x left / right / composed (columns) is saved as OGL3D_flicker_NNNNN.bmp.

void WriteBmp(const std::wstring& path, int w, int h, const std::vector<uint8_t>& bgra);


struct Probe {
    static constexpr int kImages = 3, kSlots = 4, kRing = 8, kMaxSheets = 12;
    int W = 0, H = 0, hw = 0, hh = 0, sw = 0, sh = 0;
    std::vector<GLuint> chainFbo, chainTex;   // halving steps below the ring level
    std::vector<std::array<int, 2>> chainSize;
    GLuint ringFbo[kRing][kImages] = {}, ringTex[kRing][kImages] = {};
    unsigned ringFrame[kRing] = {};
    GLuint pbo[kSlots] = {};
    GLsync fence[kSlots] = {};
    struct Info {
        bool used = false;
        unsigned frame = 0;
        char kind = '?';
        float conv = 0;
        double ms = 0;
    };
    Info slot[kSlots];
    struct Frame {
        Info info;
        std::vector<uint8_t> lum[kImages];
    };
    std::vector<Frame> hist;   // last three analysed frames, oldest first
    unsigned events[kImages] = {}, frames = 0, sheets = 0;
    GLuint sheetFbo = 0, sheetTex = 0;
    int sheetW = 0, sheetH = 0;
} g_probe;

const char* const kProbeName[Probe::kImages] = {"left", "right", "output"};

void ProbeTarget(GLuint& fbo, GLuint& tex, int w, int h)
{
    R.GenTextures(1, &tex);
    R.BindTexture(GL_TEXTURE_2D, tex);
    R.TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    R.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    R.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    R.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
    R.GenFramebuffers(1, &fbo);
    R.BindFramebuffer(GL_DRAW_FRAMEBUFFER, fbo);
    R.FramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
}

void ProbeFree(GLuint& fbo, GLuint& tex)
{
    if (fbo) R.DeleteFramebuffers(1, &fbo);
    if (tex) R.DeleteTextures(1, &tex);
    fbo = tex = 0;
}

// (Re)creates the ring, the halving chain and the read-back buffers for the window size.
void ProbeSetup(Ctx* c)
{
    Probe& p = g_probe;
    if (p.W == c->winW && p.H == c->winH && p.pbo[0]) return;
    for (size_t i = 0; i < p.chainFbo.size(); ++i) ProbeFree(p.chainFbo[i], p.chainTex[i]);
    p.chainFbo.clear();
    p.chainTex.clear();
    p.chainSize.clear();
    for (int r = 0; r < Probe::kRing; ++r)
        for (int k = 0; k < Probe::kImages; ++k) ProbeFree(p.ringFbo[r][k], p.ringTex[r][k]);
    ProbeFree(p.sheetFbo, p.sheetTex);
    p.W = c->winW;
    p.H = c->winH;
    p.hw = std::max(1, p.W / 2);
    p.hh = std::max(1, p.H / 2);
    for (int r = 0; r < Probe::kRing; ++r) {
        for (int k = 0; k < Probe::kImages; ++k) ProbeTarget(p.ringFbo[r][k], p.ringTex[r][k], p.hw, p.hh);
        p.ringFrame[r] = ~0u;
    }
    int w = p.hw, h = p.hh;
    while (w > 160) {
        w = std::max(1, w / 2);
        h = std::max(1, h / 2);
        GLuint fbo = 0, tex = 0;
        ProbeTarget(fbo, tex, w, h);
        p.chainFbo.push_back(fbo);
        p.chainTex.push_back(tex);
        p.chainSize.push_back({w, h});
    }
    p.sw = w;
    p.sh = h;
    p.sheetW = 3 * (p.hw / 3);
    p.sheetH = 3 * (p.hh / 3);
    ProbeTarget(p.sheetFbo, p.sheetTex, p.sheetW, p.sheetH);
    if (!p.pbo[0]) R.GenBuffers(Probe::kSlots, p.pbo);
    for (int s = 0; s < Probe::kSlots; ++s) {
        R.BindBuffer(GL_PIXEL_PACK_BUFFER, p.pbo[s]);
        R.BufferData(GL_PIXEL_PACK_BUFFER, (GLsizeiptr)p.sw * p.sh * 4 * Probe::kImages, nullptr, GL_STREAM_READ);
        if (p.fence[s]) R.DeleteSync(p.fence[s]);
        p.fence[s] = nullptr;
        p.slot[s].used = false;
    }
    p.hist.clear();
    R.BindTexture(GL_TEXTURE_2D, c->bound[c->activeUnit][kSlot2D]);
    Log("flicker probe: %dx%d -> ring %dx%d x%d frames, analysed at %dx%d", p.W, p.H, p.hw, p.hh, Probe::kRing, p.sw, p.sh);
}

// Saves frames a, b, c (rows) x left / right / output (columns) from the ring, if still there.
void ProbeSheet(const unsigned (&fr)[3])
{
    Probe& p = g_probe;
    if (p.sheets >= Probe::kMaxSheets) return;
    int ring[3];
    for (int i = 0; i < 3; ++i) {
        ring[i] = (int)(fr[i] % Probe::kRing);
        if (p.ringFrame[ring[i]] != fr[i]) return;
    }
    const int cw = p.sheetW / 3, ch = p.sheetH / 3;
    R.BindFramebuffer(GL_DRAW_FRAMEBUFFER, p.sheetFbo);
    for (int row = 0; row < 3; ++row)
        for (int k = 0; k < Probe::kImages; ++k) {
            R.BindFramebuffer(GL_READ_FRAMEBUFFER, p.ringFbo[ring[row]][k]);
            const int y0 = (2 - row) * ch;   // first frame on top (GL rows go upwards)
            R.BlitFramebuffer(0, 0, p.hw, p.hh, k * cw, y0, (k + 1) * cw, y0 + ch, GL_COLOR_BUFFER_BIT, GL_LINEAR);
        }
    std::vector<uint8_t> px((size_t)p.sheetW * p.sheetH * 4);
    R.BindBuffer(GL_PIXEL_PACK_BUFFER, 0);
    R.BindFramebuffer(GL_READ_FRAMEBUFFER, p.sheetFbo);
    R.ReadPixels(0, 0, p.sheetW, p.sheetH, GL_BGRA, GL_UNSIGNED_BYTE, px.data());
    wchar_t name[64];
    swprintf_s(name, L"OGL3D_flicker_%05u.bmp", fr[1]);
    WriteBmp(ModuleDir() + name, p.sheetW, p.sheetH, px);
    p.sheets++;
    Log("  saved %ls (rows: frames %u, %u, %u; columns: left, right, output)", name, fr[0], fr[1], fr[2]);
}

void ProbeAnalyse(const uint8_t* px, const Probe::Info& info)
{
    Probe& p = g_probe;
    const int n = p.sw * p.sh;
    Probe::Frame f;
    f.info = info;
    for (int k = 0; k < Probe::kImages; ++k) {
        f.lum[k].resize(n);
        const uint8_t* q = px + (size_t)k * n * 4;
        for (int i = 0; i < n; ++i) f.lum[k][i] = (uint8_t)((q[i * 4] + 2 * q[i * 4 + 1] + q[i * 4 + 2]) / 4);
    }
    p.hist.push_back(std::move(f));
    if (p.hist.size() > 3) p.hist.erase(p.hist.begin());
    p.frames++;
    if (p.hist.size() == 3 && p.hist[1].info.frame == p.hist[0].info.frame + 1 &&
        p.hist[2].info.frame == p.hist[1].info.frame + 1) {
        const Probe::Frame &A = p.hist[0], &B = p.hist[1], &C = p.hist[2];
        const int gx = 8, gy = 6;
        std::string what;
        for (int k = 0; k < Probe::kImages; ++k) {
            int count = 0;
            float wab = 0, wbc = 0, wac = 0;
            for (int ry = 0; ry < gy; ++ry)
                for (int rx = 0; rx < gx; ++rx) {
                    const int x0 = rx * p.sw / gx, x1 = (rx + 1) * p.sw / gx, y0 = ry * p.sh / gy, y1 = (ry + 1) * p.sh / gy;
                    long ab = 0, bc = 0, ac = 0, m = 0;
                    for (int y = y0; y < y1; ++y)
                        for (int x = x0; x < x1; ++x, ++m) {
                            const int i = y * p.sw + x, a = A.lum[k][i], b = B.lum[k][i], cc = C.lum[k][i];
                            ab += std::abs(a - b);
                            bc += std::abs(b - cc);
                            ac += std::abs(a - cc);
                        }
                    if (!m) continue;
                    const float dab = (float)ab / m, dbc = (float)bc / m, dac = (float)ac / m;
                    if (dab > 10 && dbc > 10 && dac < 0.4f * std::min(dab, dbc)) {
                        count++;
                        if (dab + dbc > wab + wbc) {
                            wab = dab;
                            wbc = dbc;
                            wac = dac;
                        }
                    }
                }
            if (!count) continue;
            p.events[k]++;
            char buf[160];
            sprintf_s(buf, " %s %d/%d regions (A>B %.0f, B>C %.0f, A>C %.0f)", kProbeName[k], count, gx * gy, wab, wbc, wac);
            what += buf;
        }
        if (!what.empty()) {
            Log("FLICKER frame %u [%c%c%c] %.1f/%.1f/%.1f ms conv %.4g/%.4g/%.4g:%s", B.info.frame, A.info.kind,
                B.info.kind, C.info.kind, A.info.ms, B.info.ms, C.info.ms, A.info.conv, B.info.conv, C.info.conv,
                what.c_str());
            const unsigned fr[3] = {A.info.frame, B.info.frame, C.info.frame};
            ProbeSheet(fr);
        }
    }
    if (p.frames % 1800 == 0)
        Log("flicker probe: %u frames analysed; A-B-A flickers: left %u, right %u, output %u; last frames %s", p.frames,
            p.events[0], p.events[1], p.events[2], g_framePattern.c_str());
}

// Called from Compose with neutral scissor/sRGB/mask state, after the composed image is final.
void ProbeStep(Ctx* c)
{
    if (!g_cfg.flickerProbe || !c->leftCopyFbo || !R.MapBuffer || !R.GenBuffers || c->winW < 16 || c->winH < 16) return;
    Probe& p = g_probe;
    GLint packBuf = 0, unpackBuf = 0, align = 4, rowLen = 0, skipRows = 0, skipPix = 0;
    R.GetIntegerv(GL_PIXEL_PACK_BUFFER_BINDING, &packBuf);
    R.GetIntegerv(GL_PIXEL_UNPACK_BUFFER_BINDING, &unpackBuf);
    if (unpackBuf) R.BindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
    R.GetIntegerv(GL_PACK_ALIGNMENT, &align);
    R.GetIntegerv(GL_PACK_ROW_LENGTH, &rowLen);
    R.GetIntegerv(GL_PACK_SKIP_ROWS, &skipRows);
    R.GetIntegerv(GL_PACK_SKIP_PIXELS, &skipPix);
    R.PixelStorei(GL_PACK_ALIGNMENT, 4);
    R.PixelStorei(GL_PACK_ROW_LENGTH, 0);
    R.PixelStorei(GL_PACK_SKIP_ROWS, 0);
    R.PixelStorei(GL_PACK_SKIP_PIXELS, 0);
    ProbeSetup(c);
    // this slot was filled kSlots frames ago: analyse it before reusing it
    const int s = (int)(g_frame % Probe::kSlots);
    if (p.slot[s].used && !FenceDone(p.fence[s])) {   // not filled yet: skip it (the analysis sees a gap)
        R.DeleteSync(p.fence[s]);
        p.fence[s] = nullptr;
        p.slot[s].used = false;
    }
    if (p.slot[s].used) {
        std::vector<uint8_t> px((size_t)p.sw * p.sh * 4 * Probe::kImages);
        R.BindBuffer(GL_PIXEL_PACK_BUFFER, p.pbo[s]);
        bool ok = false;
        if (const void* m = R.MapBuffer(GL_PIXEL_PACK_BUFFER, GL_READ_ONLY)) {
            memcpy(px.data(), m, px.size());
            ok = true;
        }
        R.UnmapBuffer(GL_PIXEL_PACK_BUFFER);
        p.slot[s].used = false;
        if (ok) ProbeAnalyse(px.data(), p.slot[s]);
    }
    const int r = (int)(g_frame % Probe::kRing);
    const GLuint src[Probe::kImages] = {LeftSrc(c), c->rightDefFbo ? c->rightDefFbo : LeftSrc(c), 0};
    for (int k = 0; k < Probe::kImages; ++k) {
        R.BindFramebuffer(GL_READ_FRAMEBUFFER, src[k]);
        R.BindFramebuffer(GL_DRAW_FRAMEBUFFER, p.ringFbo[r][k]);
        R.BlitFramebuffer(0, 0, p.W, p.H, 0, 0, p.hw, p.hh, GL_COLOR_BUFFER_BIT, GL_LINEAR);
        GLuint from = p.ringFbo[r][k];
        int fw = p.hw, fh = p.hh;
        for (size_t i = 0; i < p.chainFbo.size(); ++i) {
            R.BindFramebuffer(GL_READ_FRAMEBUFFER, from);
            R.BindFramebuffer(GL_DRAW_FRAMEBUFFER, p.chainFbo[i]);
            R.BlitFramebuffer(0, 0, fw, fh, 0, 0, p.chainSize[i][0], p.chainSize[i][1], GL_COLOR_BUFFER_BIT, GL_LINEAR);
            from = p.chainFbo[i];
            fw = p.chainSize[i][0];
            fh = p.chainSize[i][1];
        }
        R.BindBuffer(GL_PIXEL_PACK_BUFFER, p.pbo[s]);
        R.BindFramebuffer(GL_READ_FRAMEBUFFER, from);
        R.ReadPixels(0, 0, p.sw, p.sh, GL_BGRA, GL_UNSIGNED_BYTE, (void*)((size_t)k * p.sw * p.sh * 4));
    }
    p.ringFrame[r] = g_frame;
    p.fence[s] = NewFence();
    p.slot[s].used = true;
    p.slot[s].frame = g_frame;
    p.slot[s].kind = g_frameKind;
    p.slot[s].conv = std::isnan(g_frameConv) ? g_cfg.convergence : g_frameConv;
    p.slot[s].ms = g_frameMs;
    R.BindBuffer(GL_PIXEL_PACK_BUFFER, (GLuint)packBuf);
    if (unpackBuf) R.BindBuffer(GL_PIXEL_UNPACK_BUFFER, (GLuint)unpackBuf);
    R.PixelStorei(GL_PACK_ALIGNMENT, align);
    R.PixelStorei(GL_PACK_ROW_LENGTH, rowLen);
    R.PixelStorei(GL_PACK_SKIP_ROWS, skipRows);
    R.PixelStorei(GL_PACK_SKIP_PIXELS, skipPix);
}

bool g_weaveOff;   // Ctrl+Alt+W: show SBS instead of weaving

// Returns true when the frame was presented in the upscale window (skip the game's own swap).
bool Compose(Ctx* c, HDC dc)
{
    const int W = c->winW, H = c->winH;
    bool stereo = g_cfg.enable && EnsureRightDefault(c);
    EnsureLeftCopy(c);
    GLboolean scissor = R.IsEnabled(GL_SCISSOR_TEST);
    GLboolean srgb = R.IsEnabled(GL_FRAMEBUFFER_SRGB);
    GLboolean mask[4];
    R.GetBooleanv(GL_COLOR_WRITEMASK, mask);
    if (scissor) R.Disable(GL_SCISSOR_TEST);
    if (srgb) R.Disable(GL_FRAMEBUFFER_SRGB);
    R.ColorMask(1, 1, 1, 1);

    GLint readBuf = GL_BACK, drawBuf = GL_BACK;
    R.BindFramebuffer(GL_READ_FRAMEBUFFER, 0);
    R.GetIntegerv(GL_READ_BUFFER, &readBuf);
    R.ReadBuffer(GL_BACK);
    // Left eye: with the virtual back buffer it is that buffer (the game drew there; on frames it
    // does not draw it still holds the previous image). Otherwise it is copied from the back buffer,
    // but only if the game drew into it this frame: else that holds one of our composed frames and
    // the previous left eye is reused.
    const bool vbbFrame = c->vbbLive && c->vbbFbo && c->vbbW == W && c->vbbH == H;
    const bool newImage = c->fb0Touched || !c->leftCopyValid;
    if (vbbFrame) {
        c->leftSrc = c->vbbFbo;
        R.BindFramebuffer(GL_READ_FRAMEBUFFER, c->vbbFbo);   // read it whatever the game selected
        R.ReadBuffer(GL_COLOR_ATTACHMENT0);
        R.BindFramebuffer(GL_READ_FRAMEBUFFER, 0);
    } else {
        if (newImage || c->vbbLive) {   // (after a resize the game drew into the old-size buffer)
            const bool fromVbb = c->vbbLive && c->vbbFbo;
            if (fromVbb) R.BindFramebuffer(GL_READ_FRAMEBUFFER, c->vbbFbo);
            R.BindFramebuffer(GL_DRAW_FRAMEBUFFER, c->leftCopyFbo);
            R.BlitFramebuffer(0, 0, fromVbb ? c->vbbW : W, fromVbb ? c->vbbH : H, 0, 0, W, H, GL_COLOR_BUFFER_BIT,
                              fromVbb ? GL_LINEAR : GL_NEAREST);
            if (fromVbb) R.BindFramebuffer(GL_READ_FRAMEBUFFER, 0);
        }
        c->leftSrc = c->leftCopyFbo;
    }
    if (newImage) {
        c->leftCopyValid = true;
        c->imageSerial++;
        c->imageConv = std::isnan(g_frameConv) ? g_cfg.convergence : g_frameConv;
        c->imageSep = std::isnan(g_frameSep) ? SepValue() : g_frameSep;
    }
    c->fb0Touched = false;

    GLuint left = c->leftSrc, right = stereo ? c->rightDefFbo : c->leftSrc;
    if (g_cfg.swapEyes) std::swap(left, right);
    R.BindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
    R.GetIntegerv(GL_DRAW_BUFFER, &drawBuf);
    R.DrawBuffer(GL_BACK);
    if (g_cfg.markBackbuffer && R.ClearBufferfv) {   // debug: whatever shows magenta was never composed
        const GLfloat magenta[4] = {1.0f, 0.0f, 1.0f, 1.0f};
        R.ClearBufferfv(GL_COLOR, 0, magenta);
    }
    auto blit = [&](GLuint src, int x0, int y0, int x1, int y1) {
        R.BindFramebuffer(GL_READ_FRAMEBUFFER, src);
        R.BlitFramebuffer(0, 0, W, H, x0, y0, x1, y1, GL_COLOR_BUFFER_BIT, GL_LINEAR);
    };
    // The overlay goes into the composed output (never into the eye copies, which may be reused
    // on frames the game does not draw), at the same spot in each eye's area: screen depth.
    auto overlay = [&](GLuint fbo, std::initializer_list<std::array<int, 4>> areas) {
        if (!OsdActive()) return;
        GlStateGuard guard(c);
        for (const auto& a : areas) OsdRender(fbo, a[0], a[1], a[2], a[3], W, H, c->compat);
    };
    if (g_cfg.logLevel >= 2) {   // what the game left on at the end of the frame (changes only, first 40)
        static std::string lastState;
        static int logged;
        GLint bs = 0, bd = 0, be = 0;
        R.GetIntegerv(GL_BLEND_SRC_RGB, &bs);
        R.GetIntegerv(GL_BLEND_DST_RGB, &bd);
        R.GetIntegerv(GL_BLEND_EQUATION_RGB, &be);
        char st[160];
        sprintf_s(st, "blend %d (0x%x, 0x%x, eq 0x%x) depth %d stencil %d cull %d alpha test %d logic op %d",
                  R.IsEnabled(GL_BLEND), bs, bd, be, R.IsEnabled(GL_DEPTH_TEST), R.IsEnabled(GL_STENCIL_TEST),
                  R.IsEnabled(GL_CULL_FACE), c->compat ? R.IsEnabled(GL_ALPHA_TEST) : 0, R.IsEnabled(GL_COLOR_LOGIC_OP));
        if (st != lastState && logged < 40) {
            logged++;
            lastState = st;
            Log("frame %u ends with %s", g_frame, st);
        }
    }
    bool woven = false, presented = false;
    if (g_cfg.upscale) {
        // Both eyes at the game's resolution go to the native-resolution D3D11 window (present.cpp);
        // when that is not possible (game in the background) the game's own window gets half SBS.
        EnsureSbs(c);
        R.BindFramebuffer(GL_DRAW_FRAMEBUFFER, c->sbsFbo);
        blit(left, 0, 0, W, H);
        blit(right, W, 0, 2 * W, H);
        overlay(c->sbsFbo, {{0, 0, W, H}, {W, 0, W, H}});
        presented = PresentFrame(WindowFromDC(dc), c->sbsFbo, W, H, g_cfg.output == 4 && !g_weaveOff);
        if (!presented && g_cfg.output == 4) SrSetLens(false);
    } else if (g_cfg.output == 4 && !g_weaveOff && SrAvailableOwn(WindowFromDC(dc))) {
        EnsureSbs(c);
        R.BindFramebuffer(GL_DRAW_FRAMEBUFFER, c->sbsFbo);
        blit(left, 0, 0, W, H);
        blit(right, W, 0, 2 * W, H);
        overlay(c->sbsFbo, {{0, 0, W, H}, {W, 0, W, H}});
        {
            GlStateGuard guard(c);
            R.Viewport(0, 0, W, H);
            Ctx* self = t_ctx;
            t_ctx = nullptr;
            woven = SrWeave(c->sbsTex, W, H);
            t_ctx = self;
        }
        R.BindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
        SrSetLens(woven);
    } else if (g_cfg.output == 4 && !g_cfg.upscale) {
        SrSetLens(false);
    }
    c->lastWoven = woven;
    if (!woven && !presented) {
        R.BindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
        switch (g_cfg.output) {
        case 1:   // top-and-bottom, left eye on top (GL origin is bottom-left)
            blit(left, 0, H / 2, W, H);
            blit(right, 0, 0, W, H / 2);
            overlay(0, {{0, H / 2, W, H - H / 2}, {0, 0, W, H / 2}});
            break;
        case 2:
            blit(left, 0, 0, W, H);
            overlay(0, {{0, 0, W, H}});
            break;
        case 3:
            blit(right, 0, 0, W, H);
            overlay(0, {{0, 0, W, H}});
            break;
        default:         // half SBS (also the fallback when SR is unavailable)
            blit(left, 0, 0, W / 2, H);
            blit(right, W / 2, 0, W, H);
            overlay(0, {{0, 0, W / 2, H}, {W / 2, 0, W - W / 2, H}});
            break;
        }
    }
    if (stereo && !g_cfg.flatEnable) AutoConvergenceStep(c);   // flat depth has no view depth to converge on
    ProbeStep(c);
    if (g_cfg.virtualBackbuffer) EnsureVbb(c);   // the game's next frame goes there
    if (c->vbbLive) {
        R.BindFramebuffer(GL_READ_FRAMEBUFFER, c->vbbFbo);
        R.ReadBuffer(VbbBuffer(c->fb0ReadBuf));
    }
    R.BindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
    R.DrawBuffer((GLenum)drawBuf);
    R.BindFramebuffer(GL_READ_FRAMEBUFFER, 0);
    R.ReadBuffer((GLenum)readBuf);
    R.BindFramebuffer(GL_DRAW_FRAMEBUFFER, RealFb(c, c->drawFb));
    R.BindFramebuffer(GL_READ_FRAMEBUFFER, RealFb(c, c->readFb));
    if (scissor) R.Enable(GL_SCISSOR_TEST);
    if (srgb) R.Enable(GL_FRAMEBUFFER_SRGB);
    R.ColorMask(mask[0], mask[1], mask[2], mask[3]);
    return presented;
}

void WriteBmp(const std::wstring& path, int w, int h, const std::vector<uint8_t>& bgra)
{
    FILE* f = _wfopen(path.c_str(), L"wb");
    if (!f) return;
    BITMAPFILEHEADER fh{};
    BITMAPINFOHEADER ih{};
    fh.bfType = 0x4D42;
    fh.bfOffBits = sizeof(fh) + sizeof(ih);
    fh.bfSize = fh.bfOffBits + (DWORD)bgra.size();
    ih.biSize = sizeof(ih);
    ih.biWidth = w;
    ih.biHeight = h;   // bottom-up, like glReadPixels
    ih.biPlanes = 1;
    ih.biBitCount = 32;
    fwrite(&fh, sizeof(fh), 1, f);
    fwrite(&ih, sizeof(ih), 1, f);
    fwrite(bgra.data(), 1, bgra.size(), f);
    fclose(f);
}

// Block matching between the eyes on a 12x6 grid: logs the horizontal disparity (right minus
// left, full-resolution pixels; positive = behind the screen) so depth can be checked by numbers.
void MeasureDisparity(const std::vector<uint8_t>& L, const std::vector<uint8_t>& Rt, int W, int H)
{
    const int s = 4;   // work at quarter resolution
    const int w = W / s, h = H / s;
    if (w < 64 || h < 32) return;
    std::vector<int> yl((size_t)w * h), yr((size_t)w * h);
    auto luminance = [&](const std::vector<uint8_t>& px, std::vector<int>& out) {
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) {
                int sum = 0;
                for (int dy = 0; dy < s; ++dy) {
                    const uint8_t* p = &px[((size_t)(y * s + dy) * W + (size_t)x * s) * 4];
                    for (int dx = 0; dx < s; ++dx, p += 4) sum += p[0] + 2 * p[1] + p[2];   // BGRA
                }
                out[(size_t)y * w + x] = sum / (s * s);
            }
    };
    luminance(L, yl);
    luminance(Rt, yr);
    const int cols = 12, rows = 6, bw = 16, bh = 16, maxD = std::max(4, w * 6 / 100);
    std::vector<int> found;
    int mismatches = 0;
    std::vector<long long> sad(2 * maxD + 1);
    for (int r = 0; r < rows; ++r) {   // r = 0 is the top row of the screen
        std::string line;
        int y0 = h - (2 * r + 1) * h / (2 * rows) - bh / 2;
        for (int col = 0; col < cols; ++col) {
            int x0 = (2 * col + 1) * w / (2 * cols) - bw / 2;
            if (!HasHorizontalTexture(yl.data(), w, x0, y0, bw)) {   // flat or horizontal edges only
                line += "     .";
                continue;
            }
            int best = -1;
            for (int d = -maxD; d <= maxD; ++d) {
                long long& v = sad[d + maxD];
                v = -1;
                if (x0 + d < 0 || x0 + d + bw > w) continue;
                v = 0;
                for (int y = 0; y < bh; ++y) {
                    const int* a = &yl[(size_t)(y0 + y) * w + x0];
                    const int* b = &yr[(size_t)(y0 + y) * w + x0 + d];
                    for (int x = 0; x < bw; ++x) v += std::abs(a[x] - b[x]);
                }
                if (best < 0 || v < sad[best]) best = d + maxD;
            }
            if (best >= 0 && sad[maxD] >= 0 && sad[maxD] <= sad[best] + (long long)bw * bh) best = maxD;   // ties: 0
            if (best < 0 || (best != maxD && MatchAmbiguous(sad, best, bw))) {   // ambiguous
                line += "     ?";
                continue;
            }
            int px = (best - maxD) * s;
            found.push_back(px);
            // even the best match differs a lot: the eyes show different content here (an effect
            // that is wrong in one eye, or an occlusion edge)
            bool mismatch = sad[best] > 40LL * bw * bh;
            mismatches += mismatch;
            char buf[16];
            sprintf_s(buf, "%5d%c", px, mismatch ? '!' : ' ');
            line += buf;
        }
        Log("  disparity row %d:%s", r, line.c_str());
    }
    if (found.empty()) {
        Log("disparity: no textured blocks");
        return;
    }
    std::sort(found.begin(), found.end());
    Log("disparity (right - left, px at %dx%d per eye; + = behind screen): min %d, median %d, max %d over %zu blocks, "
        "%d blocks differ between the eyes (!)",
        W, H, found.front(), found[found.size() / 2], found.back(), found.size(), mismatches);
}

// Saves both eyes (from our own copies, so it works with any output mode) as a half-resolution
// side-by-side BMP and logs the measured disparity.
void DumpFrame(Ctx* c)
{
    const int W = c->winW, H = c->winH;
    bool stereo = g_cfg.enable && c->rightDefFbo;
    std::vector<uint8_t> L((size_t)W * H * 4), Rt((size_t)W * H * 4);
    GLint pack = 0, align = 4, rowLen = 0, skipRows = 0, skipPix = 0;
    R.GetIntegerv(GL_PIXEL_PACK_BUFFER_BINDING, &pack);
    if (pack) R.BindBuffer(GL_PIXEL_PACK_BUFFER, 0);
    R.GetIntegerv(GL_PACK_ALIGNMENT, &align);
    R.GetIntegerv(GL_PACK_ROW_LENGTH, &rowLen);
    R.GetIntegerv(GL_PACK_SKIP_ROWS, &skipRows);
    R.GetIntegerv(GL_PACK_SKIP_PIXELS, &skipPix);
    R.PixelStorei(GL_PACK_ALIGNMENT, 4);
    R.PixelStorei(GL_PACK_ROW_LENGTH, 0);
    R.PixelStorei(GL_PACK_SKIP_ROWS, 0);
    R.PixelStorei(GL_PACK_SKIP_PIXELS, 0);
    R.BindFramebuffer(GL_READ_FRAMEBUFFER, LeftSrc(c));
    R.ReadPixels(0, 0, W, H, GL_BGRA, GL_UNSIGNED_BYTE, L.data());
    R.BindFramebuffer(GL_READ_FRAMEBUFFER, stereo ? c->rightDefFbo : LeftSrc(c));
    R.ReadPixels(0, 0, W, H, GL_BGRA, GL_UNSIGNED_BYTE, Rt.data());
    std::vector<uint8_t> srIn;   // what the SR weaver received (both eyes + overlay), when weaving
    if (c->lastWoven && c->sbsFbo) {
        srIn.resize((size_t)W * 2 * H * 4);
        R.BindFramebuffer(GL_READ_FRAMEBUFFER, c->sbsFbo);
        R.ReadPixels(0, 0, W * 2, H, GL_BGRA, GL_UNSIGNED_BYTE, srIn.data());
    }
    R.BindFramebuffer(GL_READ_FRAMEBUFFER, RealFb(c, c->readFb));
    R.PixelStorei(GL_PACK_ALIGNMENT, align);
    R.PixelStorei(GL_PACK_ROW_LENGTH, rowLen);
    R.PixelStorei(GL_PACK_SKIP_ROWS, skipRows);
    R.PixelStorei(GL_PACK_SKIP_PIXELS, skipPix);
    if (pack) R.BindBuffer(GL_PIXEL_PACK_BUFFER, (GLuint)pack);

    // half resolution, left eye | right eye
    const int hw = W / 2, hh = H / 2;
    std::vector<uint8_t> out((size_t)W * hh * 4);
    for (int y = 0; y < hh; ++y)
        for (int x = 0; x < hw; ++x)
            for (int eye = 0; eye < 2; ++eye) {
                const std::vector<uint8_t>& src = eye ? Rt : L;
                uint8_t* o = &out[((size_t)y * W + (size_t)eye * hw + x) * 4];
                for (int ch = 0; ch < 4; ++ch) {
                    int sum = src[((size_t)(2 * y) * W + 2 * x) * 4 + ch] + src[((size_t)(2 * y) * W + 2 * x + 1) * 4 + ch] +
                              src[((size_t)(2 * y + 1) * W + 2 * x) * 4 + ch] +
                              src[((size_t)(2 * y + 1) * W + 2 * x + 1) * 4 + ch];
                    o[ch] = (uint8_t)(sum / 4);
                }
            }
    wchar_t name[64];
    swprintf_s(name, L"OGL3D_frame%05u.bmp", g_frame);
    WriteBmp(ModuleDir() + name, W, hh, out);
    if (!srIn.empty()) {   // quarter resolution of the 2W x H weaver input
        const int ow = W / 2, oh = H / 4;
        std::vector<uint8_t> small((size_t)ow * oh * 4);
        for (int y = 0; y < oh; ++y)
            for (int x = 0; x < ow; ++x)
                memcpy(&small[((size_t)y * ow + x) * 4], &srIn[((size_t)(y * 4) * (W * 2) + (size_t)x * 4) * 4], 4);
        swprintf_s(name, L"OGL3D_frame%05u_srinput.bmp", g_frame);
        WriteBmp(ModuleDir() + name, ow, oh, small);
    }
    Log("frame %u dumped (both eyes, %dx%d each; separation %.1f%% convergence %.3f)", g_frame, hw, hh,
        g_cfg.separation, g_cfg.convergence);
    MeasureDisparity(L, Rt, W, H);
}

// [Debug] trace_steps: in a traced frame, saves the left-eye target of every draw after it ran
// (OGL3D_step<frame>_<draw>.bmp, at most 640 wide), so off-screen caches and layers can be seen.
void TraceStep(Ctx* c)
{
    if (!g_traceNow || !g_cfg.traceSteps || !c->isMain || g_traceDraw == 0) return;
    int W = c->winW, H = c->winH;
    if (c->drawFb) {
        auto f = c->fbo.find(c->drawFb);
        if (f == c->fbo.end() || f->second.w <= 0 || f->second.h <= 0) return;
        W = f->second.w;
        H = f->second.h;
    }
    if (W <= 0 || H <= 0) return;
    std::vector<uint8_t> px((size_t)W * H * 4);
    GLint pack = 0, align = 4, rowLen = 0;
    R.GetIntegerv(GL_PIXEL_PACK_BUFFER_BINDING, &pack);
    if (pack) R.BindBuffer(GL_PIXEL_PACK_BUFFER, 0);
    R.GetIntegerv(GL_PACK_ALIGNMENT, &align);
    R.GetIntegerv(GL_PACK_ROW_LENGTH, &rowLen);
    R.PixelStorei(GL_PACK_ALIGNMENT, 4);
    R.PixelStorei(GL_PACK_ROW_LENGTH, 0);
    R.BindFramebuffer(GL_READ_FRAMEBUFFER, RealFb(c, c->drawFb));
    GLint readBuf = 0;   // WineD3D leaves GL_NONE on its draw framebuffers
    R.GetIntegerv(GL_READ_BUFFER, &readBuf);
    if (RealFb(c, c->drawFb)) R.ReadBuffer(GL_COLOR_ATTACHMENT0);
    R.ReadPixels(0, 0, W, H, GL_BGRA, GL_UNSIGNED_BYTE, px.data());
    R.ReadBuffer((GLenum)readBuf);
    R.BindFramebuffer(GL_READ_FRAMEBUFFER, RealFb(c, c->readFb));
    R.PixelStorei(GL_PACK_ALIGNMENT, align);
    R.PixelStorei(GL_PACK_ROW_LENGTH, rowLen);
    if (pack) R.BindBuffer(GL_PIXEL_PACK_BUFFER, (GLuint)pack);
    const int s = (W + 639) / 640, ow = W / s, oh = H / s;
    std::vector<uint8_t> small((size_t)ow * oh * 4);
    for (int y = 0; y < oh; ++y)
        for (int x = 0; x < ow; ++x) {
            uint8_t* o = &small[((size_t)y * ow + x) * 4];
            memcpy(o, &px[((size_t)(y * s) * W + (size_t)x * s) * 4], 4);
            o[3] = 255;
        }
    wchar_t name[64];
    swprintf_s(name, L"OGL3D_step%05u_%03u.bmp", g_frame, g_traceDraw - 1);
    WriteBmp(ModuleDir() + name, ow, oh, small);
}

bool KeyDown(const Hotkey& k)
{
    if (!k.vk || !(GetAsyncKeyState(k.vk) & 0x8000)) return false;
    bool ctrl = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
    bool shift = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
    bool alt = (GetAsyncKeyState(VK_MENU) & 0x8000) != 0;
    return ctrl == k.ctrl && shift == k.shift && alt == k.alt;
}

bool g_dumpRequested;

// Rising edge of a hotkey; prev is that action's own state.
bool Pressed(const Hotkey& k, bool& prev)
{
    bool down = KeyDown(k);
    bool edge = down && !prev;
    prev = down;
    return edge;
}

std::wstring KeyName(const Hotkey& k)
{
    std::wstring s;
    if (k.ctrl) s += L"Ctrl+";
    if (k.alt) s += L"Alt+";
    if (k.shift) s += L"Shift+";
    wchar_t buf[16];
    if (k.vk >= VK_F1 && k.vk <= VK_F24) swprintf_s(buf, L"F%d", k.vk - VK_F1 + 1);
    else if (k.vk >= VK_NUMPAD0 && k.vk <= VK_NUMPAD9) swprintf_s(buf, L"Num%d", k.vk - VK_NUMPAD0);
    else if ((k.vk >= '0' && k.vk <= '9') || (k.vk >= 'A' && k.vk <= 'Z')) swprintf_s(buf, L"%c", (wchar_t)k.vk);
    else swprintf_s(buf, L"0x%02X", k.vk);
    return s + buf;
}

std::wstring ValuesText()
{
    wchar_t buf[128];
    if (g_cfg.flatEnable) swprintf_s(buf, L"Separation %.1f%%    Screen depth %.2f", g_cfg.separation, g_cfg.flatZero);
    else swprintf_s(buf, L"Separation %.1f%%    Convergence %.2f%ls", g_cfg.separation, g_cfg.convergence,
                    g_cfg.autoConvergence ? L" (auto)" : L"");
    return buf;
}

const wchar_t* ModeLabel(int mode)
{
    static const wchar_t* const ko[kModeCount] = {L"입체", L"평면 (화면 깊이)", L"숨김"};
    static const wchar_t* const en[kModeCount] = {L"stereo", L"mono (screen depth)", L"hidden"};
    mode = std::clamp(mode, 0, kModeCount - 1);
    return KoreanUi() ? ko[mode] : en[mode];
}

// Persistent overlay lines: values (Ctrl+F1) and the hunting status.
void UpdateStatus()
{
    const bool ko = KoreanUi();
    std::wstring s;
    if (g_cfg.showValues) s = ValuesText();
    if (g_hunting) {
        wchar_t buf[256];
        ProgInfo* pi = g_huntSel ? FindProg(g_huntSel) : nullptr;
        if (g_huntList.empty())
            swprintf_s(buf, ko ? L"셰이더 찾기: 그려진 셰이더 없음" : L"Hunting: no shaders drawn");
        else
            swprintf_s(buf, ko ? L"셰이더 찾기 %d/%zu    #%08x    %ls (선택된 것은 가려짐)" : L"Hunting %d/%zu    #%08x    %ls (selected one is hidden)",
                       g_huntIndex + 1, g_huntList.size(), pi ? pi->hash : 0u, ModeLabel(pi ? pi->mode : 0));
        if (!s.empty()) s += L"\n";
        s += buf;
        s += L"\n" + KeyName(g_cfg.keyHuntPrev) + L" / " + KeyName(g_cfg.keyHuntNext) + (ko ? L" 이전/다음    " : L" prev/next    ") +
             KeyName(g_cfg.keyHuntMode) + (ko ? L" 입체/평면/숨김    " : L" stereo/mono/hide    ") + KeyName(g_cfg.keySave) +
             (ko ? L" 저장" : L" save");
    }
    OsdStatus(s);
}

// Called once per frame: the programs drawn this frame become the list to walk through.
void HuntFrameEnd()
{
    if (!g_hunting) return;
    g_huntList.swap(g_frameProgs);
    g_frameProgs.clear();
    if (g_huntList.empty()) {
        g_huntIndex = 0;
        g_huntSel = 0;
    } else {
        auto it = std::find(g_huntList.begin(), g_huntList.end(), g_huntSel);
        if (it != g_huntList.end()) {
            g_huntIndex = (int)(it - g_huntList.begin());
        } else {
            g_huntIndex = std::clamp(g_huntIndex, 0, (int)g_huntList.size() - 1);
            g_huntSel = g_huntList[g_huntIndex];
        }
    }
    UpdateStatus();
}

void HuntStep(int dir)
{
    if (g_huntList.empty()) return;
    const int n = (int)g_huntList.size();
    g_huntIndex = ((g_huntIndex + dir) % n + n) % n;
    g_huntSel = g_huntList[g_huntIndex];
    ProgInfo* pi = FindProg(g_huntSel);
    Log("hunting: %d/%d program %u hash %08x (%ls)", g_huntIndex + 1, n, g_huntSel, pi ? pi->hash : 0u,
        ModeName(pi ? pi->mode : 0));
    UpdateStatus();
}

// stereo -> mono -> hidden -> stereo, for every program built from the same shaders
void HuntCycleMode()
{
    ProgInfo* pi = g_huntSel ? FindProg(g_huntSel) : nullptr;
    if (!pi) return;
    const int mode = (pi->mode + 1) % kModeCount;
    if (pi->hash) {
        for (auto& kv : g_prog)
            if (kv.second.hash == pi->hash) kv.second.mode = mode;
        if (mode == kModeStereo) g_cfg.overrides.erase(pi->hash);
        else g_cfg.overrides[pi->hash] = mode;
    } else {
        pi->mode = mode;   // unknown sources: works now, cannot be saved
    }
    Log("hunting: program %u hash %08x -> %ls", g_huntSel, pi->hash, ModeName(mode));
    wchar_t buf[160];
    swprintf_s(buf, KoreanUi() ? L"#%08x → %ls  (%ls 로 저장)" : L"#%08x → %ls  (%ls saves)", pi->hash, ModeLabel(mode),
               KeyName(g_cfg.keySave).c_str());
    OsdMessage(buf, 3.0);
    UpdateStatus();
}

// [Keys] swallow: the game does not see our hotkeys. Games often react to a key whatever the
// modifiers (Door Kickers: Action Squad toggles its frame statistics on F5, so Ctrl+F5 flipped them),
// so the keyboard messages of a hotkey become WM_NULL on the window's thread before the game gets
// them. PollHotkeys reads GetAsyncKeyState and still sees the keys.
HHOOK g_keyHook;
HWND g_keyHookWnd;
bool g_keySwallowed[256];   // key-down eaten: eat its key-up too

bool IsOurHotkey(int vk, bool ctrl, bool shift, bool alt)
{
    const Hotkey* keys[] = {&g_cfg.keySepDown, &g_cfg.keySepUp, &g_cfg.keyConvDown, &g_cfg.keyConvUp, &g_cfg.keySave,
                            &g_cfg.keyToggle, &g_cfg.keyDump, &g_cfg.keyWeave, &g_cfg.keyShowValues, &g_cfg.keyAutoConv,
                            &g_cfg.keyHunt, &g_cfg.keyHuntPrev, &g_cfg.keyHuntNext, &g_cfg.keyHuntMode};
    for (const Hotkey* k : keys) {
        const bool huntNav = k == &g_cfg.keyHuntPrev || k == &g_cfg.keyHuntNext || k == &g_cfg.keyHuntMode;
        if (k->vk == vk && k->ctrl == ctrl && k->shift == shift && k->alt == alt && (!huntNav || g_hunting)) return true;
    }
    return false;
}

LRESULT CALLBACK KeyMsgHook(int code, WPARAM remove, LPARAM lp)
{
    MSG* m = (MSG*)lp;
    if (code == HC_ACTION && g_cfg.swallowKeys && m) {
        int vk = 0;
        bool down = false;
        if (m->message == WM_KEYDOWN || m->message == WM_SYSKEYDOWN) {
            vk = (int)m->wParam;
            down = true;
        } else if (m->message == WM_KEYUP || m->message == WM_SYSKEYUP) {
            vk = (int)m->wParam;
        } else if (m->message == WM_INPUT) {
            RAWINPUT ri;
            UINT size = sizeof(ri);
            if (GetRawInputData((HRAWINPUT)m->lParam, RID_INPUT, &ri, &size, sizeof(RAWINPUTHEADER)) != (UINT)-1 &&
                ri.header.dwType == RIM_TYPEKEYBOARD) {
                vk = ri.data.keyboard.VKey;
                down = !(ri.data.keyboard.Flags & RI_KEY_BREAK);
            }
        }
        if (vk > 0 && vk < 256) {
            const bool eat = down ? IsOurHotkey(vk, (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0,
                                                (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0,
                                                (GetAsyncKeyState(VK_MENU) & 0x8000) != 0)
                                  : g_keySwallowed[vk];
            if (eat) {
                if (remove == PM_REMOVE) g_keySwallowed[vk] = down;
                m->message = WM_NULL;
            }
        }
    }
    return CallNextHookEx(nullptr, code, remove, lp);
}

// Hooks the message retrieval of the thread that owns the game window (once per window).
void KeyHookFor(HWND wnd)
{
    if (!g_cfg.swallowKeys || !wnd || wnd == g_keyHookWnd) return;
    const DWORD tid = GetWindowThreadProcessId(wnd, nullptr);
    if (g_keyHook) UnhookWindowsHookEx(g_keyHook);
    g_keyHook = SetWindowsHookExW(WH_GETMESSAGE, KeyMsgHook, nullptr, tid);
    g_keyHookWnd = wnd;
    Log("hotkeys: kept from the game (message hook on window thread %lu)%s", tid, g_keyHook ? "" : " - hook failed");
}

void PollHotkeys(float dt)
{
    DWORD pid = 0;
    GetWindowThreadProcessId(GetForegroundWindow(), &pid);
    if (pid != GetCurrentProcessId()) return;
    static bool pSave, pToggle, pDump, pWeave, pShow, pHunt, pPrev, pNext, pMode, pAuto, adjusting;
    const bool ko = KoreanUi();
    bool changed = false;
    if (KeyDown(g_cfg.keySepUp)) g_cfg.separation = std::min(200.0f, g_cfg.separation + 25.0f * dt), changed = true;
    if (KeyDown(g_cfg.keySepDown)) g_cfg.separation = std::max(0.0f, g_cfg.separation - 25.0f * dt), changed = true;
    const bool convUp = KeyDown(g_cfg.keyConvUp), convDown = KeyDown(g_cfg.keyConvDown);
    if (g_cfg.flatEnable && (convUp || convDown)) {
        // flat depth: the convergence keys move the depth that sits on the screen plane
        g_cfg.flatZero += (convUp ? 0.3f : -0.3f) * dt;
        g_cfg.flatZero = std::max(-1.0f, std::min(2.0f, g_cfg.flatZero));
        changed = true;
    } else if ((convUp || convDown) && g_cfg.autoConvergence) {
        g_cfg.autoConvergence = false;   // adjusting by hand takes over from auto
        Log("auto convergence off (manual adjustment)");
    }
    if (convUp && !g_cfg.flatEnable) g_cfg.convergence *= std::exp(0.7f * dt), changed = true;
    if (convDown && !g_cfg.flatEnable) g_cfg.convergence *= std::exp(-0.7f * dt), changed = true;
    if (Pressed(g_cfg.keyAutoConv, pAuto)) {
        g_cfg.autoConvergence = !g_cfg.autoConvergence;
        Log("auto convergence %s", g_cfg.autoConvergence ? "on" : "off");
        OsdMessage(g_cfg.autoConvergence ? (ko ? L"자동 Convergence 켜짐" : L"Auto convergence on")
                                         : (ko ? L"자동 Convergence 꺼짐" : L"Auto convergence off"),
                   2.0);
        if (g_cfg.showValues) UpdateStatus();
    }
    if (changed) {
        g_paramVer++;
        adjusting = true;
        OsdMessage(ValuesText(), 2.5);
        if (g_cfg.showValues) UpdateStatus();
    } else if (adjusting) {
        adjusting = false;
        Log("separation %.2f%% convergence %.4f flat screen depth %.3f", g_cfg.separation, g_cfg.convergence, g_cfg.flatZero);
    }
    if (Pressed(g_cfg.keySave, pSave)) {
        SaveStereoValues();
        // always English (user preference), whatever the Windows UI language
        wchar_t buf[200];
        if (g_cfg.flatEnable) swprintf_s(buf, L"Saved    Separation %.1f%%    Screen depth %.2f", g_cfg.separation, g_cfg.flatZero);
        else if (g_cfg.autoConvergence) swprintf_s(buf, L"Saved    Separation %.1f%%    Convergence auto", g_cfg.separation);
        else swprintf_s(buf, L"Saved    Separation %.1f%%    Convergence %.2f", g_cfg.separation, g_cfg.convergence);
        std::wstring msg = buf;
        if (!g_cfg.overrides.empty()) {
            swprintf_s(buf, L"    %zu shader overrides", g_cfg.overrides.size());
            msg += buf;
        }
        OsdMessage(msg, 3.0);
    }
    if (Pressed(g_cfg.keyToggle, pToggle)) {
        g_cfg.enable = !g_cfg.enable;
        g_paramVer++;
        Log("stereo %s", g_cfg.enable ? "on" : "off");
        OsdMessage(g_cfg.enable ? (ko ? L"3D 켜짐" : L"3D on") : (ko ? L"3D 꺼짐" : L"3D off"), 2.0);
    }
    if (Pressed(g_cfg.keyDump, pDump)) g_dumpRequested = true;
    if (Pressed(g_cfg.keyWeave, pWeave)) {
        g_weaveOff = !g_weaveOff;
        Log("SR weaving %s", g_weaveOff ? "off (SBS)" : "on");
        OsdMessage(g_weaveOff ? (ko ? L"SR 위빙 꺼짐 (SBS 출력)" : L"SR weaving off (side-by-side)")
                              : (ko ? L"SR 위빙 켜짐" : L"SR weaving on"),
                   2.0);
    }
    if (Pressed(g_cfg.keyShowValues, pShow)) {
        g_cfg.showValues = !g_cfg.showValues;
        UpdateStatus();
    }
    if (Pressed(g_cfg.keyHunt, pHunt)) {
        g_hunting = !g_hunting;
        g_frameProgs.clear();
        g_huntList.clear();
        g_huntSel = 0;
        g_huntIndex = 0;
        Log("hunting %s", g_hunting ? "on" : "off");
        if (!g_hunting) OsdMessage(ko ? L"셰이더 찾기 끝" : L"Hunting off", 2.0);
        UpdateStatus();
    }
    bool next = Pressed(g_cfg.keyHuntNext, pNext), prev = Pressed(g_cfg.keyHuntPrev, pPrev);
    bool mode = Pressed(g_cfg.keyHuntMode, pMode);
    if (g_hunting) {
        if (next) HuntStep(1);
        if (prev) HuntStep(-1);
        if (mode) HuntCycleMode();
    }
}

bool FrameListed(unsigned frame)
{
    return ListedIn(g_cfg.dumpFrames, frame);
}

// The client size in the window's own DPI awareness = the size of its GL default framebuffer
// (in upscale mode the process default becomes DPI aware while the game window stays unaware).
void UpdateWindowSize(Ctx* c, HDC dc)
{
    using GetWndCtxFn = HANDLE(WINAPI*)(HWND);
    using SetThrCtxFn = HANDLE(WINAPI*)(HANDLE);
    static auto getWnd = (GetWndCtxFn)GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetWindowDpiAwarenessContext");
    static auto setThr = (SetThrCtxFn)GetProcAddress(GetModuleHandleW(L"user32.dll"), "SetThreadDpiAwarenessContext");
    HWND wnd = WindowFromDC(dc);
    if (!wnd) return;
    HANDLE prev = getWnd && setThr ? setThr(getWnd(wnd)) : nullptr;
    RECT rc{};
    BOOL ok = GetClientRect(wnd, &rc);
    if (prev) setThr(prev);
    if (!ok) return;
    int w = rc.right - rc.left, h = rc.bottom - rc.top;
    if (w <= 0 || h <= 0 || (w == c->winW && h == c->winH)) return;
    Log("window %dx%d", w, h);
    c->winW = w;
    c->winH = h;
    c->stamp++;
}

// ---------------------------------------------------------------------------------------------
// hooks reached through wglGetProcAddress

#define CTX_OR_RETURN(call) \
    Ctx* c = t_ctx;         \
    if (!c) {               \
        call;               \
        return;             \
    }

void GLAPIENTRY H_BindFramebuffer(GLenum target, GLuint fb)
{
    Ctx* c = t_ctx;
    R.BindFramebuffer(target, c ? RealFb(c, fb) : fb);
    if (!c) return;
    if (target == GL_FRAMEBUFFER || target == GL_DRAW_FRAMEBUFFER) c->drawFb = fb;
    if (target == GL_FRAMEBUFFER || target == GL_READ_FRAMEBUFFER) c->readFb = fb;
}

void GLAPIENTRY H_DeleteFramebuffers(GLsizei n, const GLuint* ids)
{
    Ctx* c = t_ctx;
    if (c) {
        for (GLsizei i = 0; i < n; ++i) {
            auto it = c->fbo.find(ids[i]);
            if (it == c->fbo.end()) continue;
            if (it->second.right) R.DeleteFramebuffers(1, &it->second.right);
            c->fbo.erase(it);
            if (c->drawFb == ids[i]) c->drawFb = 0;
            if (c->readFb == ids[i]) c->readFb = 0;
        }
    }
    R.DeleteFramebuffers(n, ids);
    if (c && c->vbbLive) {   // a deleted bound FBO leaves the real framebuffer 0 bound
        if (c->drawFb == 0) R.BindFramebuffer(GL_DRAW_FRAMEBUFFER, c->vbbFbo);
        if (c->readFb == 0) R.BindFramebuffer(GL_READ_FRAMEBUFFER, c->vbbFbo);
    }
}

void GLAPIENTRY H_FramebufferTexture2D(GLenum target, GLenum att, GLenum textarget, GLuint tex, GLint level)
{
    if (Ctx* c0 = t_ctx; c0 && c0->vbbLive && (target == GL_READ_FRAMEBUFFER ? c0->readFb : c0->drawFb) == 0)
        return;   // attaching to framebuffer 0 is a GL error: keep it away from the virtual back buffer
    R.FramebufferTexture2D(target, att, textarget, tex, level);
    CTX_OR_RETURN((void)0);
    std::lock_guard<std::recursive_mutex> lk(g_lock);
    Att a;
    if (tex) a = Att{GL_TEXTURE, tex, textarget, level, -1};
    SetAttachment(c, target, att, a);
}

void GLAPIENTRY H_FramebufferTexture(GLenum target, GLenum att, GLuint tex, GLint level)
{
    if (Ctx* c0 = t_ctx; c0 && c0->vbbLive && (target == GL_READ_FRAMEBUFFER ? c0->readFb : c0->drawFb) == 0)
        return;   // attaching to framebuffer 0 is a GL error: keep it away from the virtual back buffer
    R.FramebufferTexture(target, att, tex, level);
    CTX_OR_RETURN((void)0);
    std::lock_guard<std::recursive_mutex> lk(g_lock);
    Att a;
    if (tex) {
        TexInfo* ti = FindTex(tex);
        a = Att{GL_TEXTURE, tex, ti ? ti->target : (GLenum)GL_TEXTURE_2D, level, ti && SlotOf(ti->target) >= 0 ? -1 : 0};
    }
    SetAttachment(c, target, att, a);
}

void GLAPIENTRY H_FramebufferTextureLayer(GLenum target, GLenum att, GLuint tex, GLint level, GLint layer)
{
    if (Ctx* c0 = t_ctx; c0 && c0->vbbLive && (target == GL_READ_FRAMEBUFFER ? c0->readFb : c0->drawFb) == 0)
        return;   // attaching to framebuffer 0 is a GL error: keep it away from the virtual back buffer
    R.FramebufferTextureLayer(target, att, tex, level, layer);
    CTX_OR_RETURN((void)0);
    std::lock_guard<std::recursive_mutex> lk(g_lock);
    Att a;
    if (tex) a = Att{GL_TEXTURE, tex, 0, level, layer};
    SetAttachment(c, target, att, a);
}

void GLAPIENTRY H_FramebufferRenderbuffer(GLenum target, GLenum att, GLenum rbtarget, GLuint rb)
{
    if (Ctx* c0 = t_ctx; c0 && c0->vbbLive && (target == GL_READ_FRAMEBUFFER ? c0->readFb : c0->drawFb) == 0)
        return;   // attaching to framebuffer 0 is a GL error: keep it away from the virtual back buffer
    R.FramebufferRenderbuffer(target, att, rbtarget, rb);
    CTX_OR_RETURN((void)0);
    std::lock_guard<std::recursive_mutex> lk(g_lock);
    Att a;
    if (rb) a = Att{GL_RENDERBUFFER, rb, 0, 0, -1};
    SetAttachment(c, target, att, a);
}

void GLAPIENTRY H_DrawBuffers(GLsizei n, const GLenum* bufs)
{
    Ctx* c = t_ctx;
    if (c && c->vbbLive && c->drawFb == 0 && n > 0 && n <= kMaxColorAttachments) {
        GLenum mapped[kMaxColorAttachments];
        for (GLsizei i = 0; i < n; ++i) mapped[i] = i == 0 ? VbbBuffer(bufs[i]) : GL_NONE;
        c->fb0DrawBuf = bufs[0];
        R.DrawBuffers(n, mapped);
        return;
    }
    R.DrawBuffers(n, bufs);
    if (!c) return;
    FboInfo* f = BoundFbo(c, GL_DRAW_FRAMEBUFFER);
    if (!f) return;
    f->nDrawBufs = std::min<GLsizei>(n, kMaxColorAttachments);
    for (GLsizei i = 0; i < f->nDrawBufs; ++i) f->drawBufs[i] = bufs[i];
    f->dirty = true;
}

void GLAPIENTRY H_BindRenderbuffer(GLenum target, GLuint rb)
{
    R.BindRenderbuffer(target, rb);
    CTX_OR_RETURN((void)0);
    c->rbBinding = rb;
}

void RenderbufferAlloc(Ctx* c, GLsizei samples, GLenum ifmt, GLsizei w, GLsizei h)
{
    std::lock_guard<std::recursive_mutex> lk(g_lock);
    RbInfo& ri = g_rb[c->rbBinding];
    bool resized = ri.w != w || ri.h != h;
    ri.w = w;
    ri.h = h;
    ri.samples = samples;
    ri.ifmt = ifmt;
    if (ri.right) {
        R.BindRenderbuffer(GL_RENDERBUFFER, ri.right);
        if (samples > 0) R.RenderbufferStorageMultisample(GL_RENDERBUFFER, samples, ifmt, w, h);
        else R.RenderbufferStorage(GL_RENDERBUFFER, ifmt, w, h);
        R.BindRenderbuffer(GL_RENDERBUFFER, c->rbBinding);
    }
    if (resized) BumpStamp();
}

void GLAPIENTRY H_RenderbufferStorage(GLenum target, GLenum ifmt, GLsizei w, GLsizei h)
{
    R.RenderbufferStorage(target, ifmt, w, h);
    CTX_OR_RETURN((void)0);
    if (c->rbBinding) RenderbufferAlloc(c, 0, ifmt, w, h);
}

void GLAPIENTRY H_RenderbufferStorageMultisample(GLenum target, GLsizei samples, GLenum ifmt, GLsizei w, GLsizei h)
{
    R.RenderbufferStorageMultisample(target, samples, ifmt, w, h);
    CTX_OR_RETURN((void)0);
    if (c->rbBinding) RenderbufferAlloc(c, samples, ifmt, w, h);
}

void GLAPIENTRY H_DeleteRenderbuffers(GLsizei n, const GLuint* ids)
{
    {
        std::lock_guard<std::recursive_mutex> lk(g_lock);
        for (GLsizei i = 0; i < n; ++i) {
            auto it = g_rb.find(ids[i]);
            if (it == g_rb.end()) continue;
            if (it->second.right) R.DeleteRenderbuffers(1, &it->second.right);
            g_rb.erase(it);
        }
    }
    if (Ctx* c = t_ctx)
        for (GLsizei i = 0; i < n; ++i)
            if (c->rbBinding == ids[i]) c->rbBinding = 0;
    R.DeleteRenderbuffers(n, ids);
}

void GLAPIENTRY H_BlitFramebuffer(GLint sx0, GLint sy0, GLint sx1, GLint sy1, GLint dx0, GLint dy0, GLint dx1,
                                  GLint dy1, GLbitfield mask, GLenum filter)
{
    R.BlitFramebuffer(sx0, sy0, sx1, sy1, dx0, dy0, dx1, dy1, mask, filter);
    Ctx* c = t_ctx;
    if (c && c->drawFb == 0) c->fb0Touched = true;
    if (!Active(c)) return;
    std::lock_guard<std::recursive_mutex> lk(g_lock);
    if (g_cfg.flatEnable && g_cfg.flatRule == 1 && c->isMain && (mask & GL_COLOR_BUFFER_BIT) && c->readFb != c->drawFb) {
        FlatEv ev;   // a copy of another target's picture, for the composition order (ComputeLayout)
        ev.fb = c->drawFb;
        ev.src[0] = c->readFb;
        ev.nsrc = 1;
        c->flatEvents.push_back(ev);
    }
    GLuint rightDraw = RightFor(c, c->drawFb);
    if (!rightDraw) return;
    GLuint rightRead = RightFor(c, c->readFb);
    R.BindFramebuffer(GL_DRAW_FRAMEBUFFER, rightDraw);
    if (rightRead) R.BindFramebuffer(GL_READ_FRAMEBUFFER, rightRead);
    R.BlitFramebuffer(sx0, sy0, sx1, sy1, dx0, dy0, dx1, dy1, mask, filter);
    R.BindFramebuffer(GL_DRAW_FRAMEBUFFER, RealFb(c, c->drawFb));
    if (rightRead) R.BindFramebuffer(GL_READ_FRAMEBUFFER, RealFb(c, c->readFb));
    g_stats.blits++;
}

void GLAPIENTRY H_ActiveTexture(GLenum unit)
{
    R.ActiveTexture(unit);
    CTX_OR_RETURN((void)0);
    GLuint u = unit - GL_TEXTURE0;
    if (u < (GLuint)kMaxUnits) {
        c->activeUnit = u;
        c->maxUnit = std::max(c->maxUnit, (int)u);
    }
}

// Applies the same call to the clone of the texture bound to target on the active unit.
template <class F>
void MirrorToClone(GLenum target, F&& f)
{
    Ctx* c = t_ctx;
    int slot = SlotOf(target);
    if (!c || slot < 0) return;
    GLuint id = c->bound[c->activeUnit][slot];
    if (!id) return;
    std::lock_guard<std::recursive_mutex> lk(g_lock);
    TexInfo* ti = FindTex(id);
    if (!ti || !ti->right) return;
    R.BindTexture(target, ti->right);
    f();
    R.BindTexture(target, id);
}

void GLAPIENTRY H_GenerateMipmap(GLenum target)
{
    R.GenerateMipmap(target);
    MirrorToClone(target, [&] { R.GenerateMipmap(target); });
    Ctx* c = t_ctx;
    int slot = SlotOf(target);
    if (!c || slot < 0) return;
    std::lock_guard<std::recursive_mutex> lk(g_lock);
    if (TexInfo* ti = FindTex(c->bound[c->activeUnit][slot])) {
        int levels = 1;
        while ((std::max(ti->w, ti->h) >> levels) > 0) ++levels;
        ti->levels = std::max(ti->levels, levels);
    }
}

void GLAPIENTRY H_TextureView(GLuint tex, GLenum target, GLuint orig, GLenum ifmt, GLuint minLevel, GLuint numLevels,
                              GLuint minLayer, GLuint numLayers)
{
    R.TextureView(tex, target, orig, ifmt, minLevel, numLevels, minLayer, numLayers);
    CTX_OR_RETURN((void)0);
    std::lock_guard<std::recursive_mutex> lk(g_lock);
    TexInfo* oi = FindTex(orig);
    if (!oi) {
        TexInfo q;
        if (!QueryTex(c, orig, target, q)) return;
        oi = &(g_tex[orig] = q);
    }
    TexInfo v;
    v.target = target;
    v.w = std::max(1, oi->w >> minLevel);
    v.h = std::max(1, oi->h >> minLevel);
    v.ifmt = ifmt;
    v.samples = oi->samples;
    v.levels = (int)numLevels;
    v.immutable = true;
    v.viewOf = orig;
    v.viewFmt = ifmt;
    v.minLevel = minLevel;
    v.numLevels = numLevels;
    v.minLayer = minLayer;
    v.numLayers = numLayers;
    TexInfo& slot = g_tex[tex];
    DeleteTexClone(slot);
    slot = v;
}

// glCopyImageSubData (D3D11 CopyResource/CopySubresourceRegion in WineD3D): the right-eye clones
// get the same copy, from the source's clone when it has one.
GLuint RightName(Ctx* c, GLuint name, GLenum target)
{
    if (target == GL_RENDERBUFFER) {
        auto it = g_rb.find(name);
        return it != g_rb.end() ? it->second.right : 0;
    }
    TexInfo* ti = FindTex(name);
    if (!ti) return 0;
    if (!ti->right && ti->viewOf) {
        TexInfo* oi = FindTex(ti->viewOf);
        if (oi && oi->right) return TexClone(c, name, ti->target);
    }
    return ti->right;
}

void GLAPIENTRY H_CopyImageSubData(GLuint src, GLenum srcTarget, GLint srcLevel, GLint srcX, GLint srcY, GLint srcZ,
                                   GLuint dst, GLenum dstTarget, GLint dstLevel, GLint dstX, GLint dstY, GLint dstZ,
                                   GLsizei w, GLsizei h, GLsizei d)
{
    R.CopyImageSubData(src, srcTarget, srcLevel, srcX, srcY, srcZ, dst, dstTarget, dstLevel, dstX, dstY, dstZ, w, h, d);
    Ctx* c = t_ctx;
    if (!c || !Active(c)) return;
    std::lock_guard<std::recursive_mutex> lk(g_lock);
    if (srcTarget != GL_RENDERBUFFER && dstTarget != GL_RENDERBUFFER && src != dst) c->texCopiedFrom[dst] = src;
    GLuint srcR = RightName(c, src, srcTarget);
    GLuint dstR = RightName(c, dst, dstTarget);
    if (!dstR && srcR) {
        // a stereo image copied into a plain texture: that texture becomes stereo too
        TexInfo* di = dstTarget == GL_RENDERBUFFER ? nullptr : FindTex(dst);
        if (di || (dstTarget != GL_RENDERBUFFER && SlotOf(dstTarget) >= 0)) dstR = TexClone(c, dst, dstTarget);
    }
    if (!dstR) return;
    R.CopyImageSubData(srcR ? srcR : src, srcTarget, srcLevel, srcX, srcY, srcZ, dstR, dstTarget, dstLevel, dstX, dstY,
                       dstZ, w, h, d);
    g_stats.copies++;
}

void GLAPIENTRY H_TexStorage2D(GLenum target, GLsizei levels, GLenum ifmt, GLsizei w, GLsizei h)
{
    R.TexStorage2D(target, levels, ifmt, w, h);
    CTX_OR_RETURN((void)0);
    int slot = SlotOf(target);
    if (slot < 0 || !c->bound[c->activeUnit][slot]) return;
    std::lock_guard<std::recursive_mutex> lk(g_lock);
    TexInfo& ti = g_tex[c->bound[c->activeUnit][slot]];
    DeleteTexClone(ti);
    ti = TexInfo{};
    ti.target = target;
    ti.w = w;
    ti.h = h;
    ti.ifmt = ifmt;
    ti.levels = levels;
    ti.immutable = true;
    BumpStamp();
}

void GLAPIENTRY H_TexImage2DMultisample(GLenum target, GLsizei samples, GLenum ifmt, GLsizei w, GLsizei h,
                                        GLboolean fixed)
{
    R.TexImage2DMultisample(target, samples, ifmt, w, h, fixed);
    CTX_OR_RETURN((void)0);
    int slot = SlotOf(target);
    if (slot < 0 || !c->bound[c->activeUnit][slot]) return;
    std::lock_guard<std::recursive_mutex> lk(g_lock);
    TexInfo& ti = g_tex[c->bound[c->activeUnit][slot]];
    ti.target = target;
    ti.w = w;
    ti.h = h;
    ti.ifmt = ifmt;
    ti.samples = samples;
    ti.fixedLoc = fixed;
    if (ti.right) {
        R.BindTexture(target, ti.right);
        R.TexImage2DMultisample(target, samples, ifmt, w, h, fixed);
        R.BindTexture(target, c->bound[c->activeUnit][slot]);
    }
    BumpStamp();
}

std::unordered_map<GLuint, std::string> g_shaderSrc;   // original sources, kept only for dump_shaders and silhouette
std::unordered_set<GLuint> g_silFs;                    // fragment shaders holding the silhouette rewrite

// dump_shaders: OGL3D_shaders\prog_<hash>.glsl holds every shader of a program (the hash the
// hunting overlay shows), so an effect found while hunting can be identified.
void DumpProgram(GLuint prog, uint32_t hash, const GLuint* shaders, GLsizei n)
{
    std::wstring dir = ModuleDir() + L"OGL3D_shaders\\";
    CreateDirectoryW(dir.c_str(), nullptr);
    wchar_t name[64];
    swprintf_s(name, L"prog_%08x.glsl", hash);
    FILE* f = _wfopen((dir + name).c_str(), L"wb");
    if (!f) return;
    fprintf(f, "// program %u, hash %08x\n", prog, hash);
    for (GLsizei i = 0; i < n; ++i) {
        GLint type = 0;
        R.GetShaderiv(shaders[i], GL_SHADER_TYPE, &type);
        const char* kind = (GLenum)type == GL_VERTEX_SHADER ? "vertex" : (GLenum)type == GL_FRAGMENT_SHADER ? "fragment"
                         : (GLenum)type == GL_GEOMETRY_SHADER ? "geometry" : "other";
        auto it = g_shaderSrc.find(shaders[i]);
        fprintf(f, "\n// ======== %s shader %u ========\n", kind, shaders[i]);
        const std::string* src = it != g_shaderSrc.end() ? &it->second : nullptr;
        for (auto& kv : g_flatGs)
            if (!src && kv.second == shaders[i] && t_ctx && kv.first.first == t_ctx->rc) src = &kv.first.second;   // our flat depth geometry shader
        if (src) fwrite(src->data(), 1, src->size(), f);
        else fprintf(f, "// (source not seen)\n");
    }
    fclose(f);
}

void GLAPIENTRY H_ShaderSource(GLuint sh, GLsizei count, const GLchar* const* str, const GLint* len)
{
    if (!t_ctx || t_ownShaders || count <= 0 || !str) {
        R.ShaderSource(sh, count, str, len);
        return;
    }
    std::string src;
    for (GLsizei i = 0; i < count; ++i) {
        if (!str[i]) continue;
        if (len && len[i] >= 0) src.append(str[i], (size_t)len[i]);
        else src.append(str[i]);
    }
    {
        std::lock_guard<std::recursive_mutex> lk(g_lock);
        g_shaderHash[sh] = Fnv1a(src.data(), src.size());   // identifies programs for [ShaderOverride]
        if (g_cfg.dumpShaders || !g_cfg.silhouette.empty()) g_shaderSrc[sh] = src;
        g_silFs.erase(sh);
    }
    GLint type = 0;
    if (g_cfg.inject && R.GetShaderiv) R.GetShaderiv(sh, GL_SHADER_TYPE, &type);
    if ((GLenum)type != GL_VERTEX_SHADER) {
        R.ShaderSource(sh, count, str, len);
        return;
    }
    std::string out;
    const bool hasMain = InjectVertexShader(src, out);
    if (!hasMain) {
        // A vertex shader piece without main(), linked together with the one that has it (WineD3D
        // writes its VS -> PS linkage in such a piece). Flat depth must see its outputs as well.
        out = src;
        if (src.find("s3d_main0") == std::string::npos && !g_cfg.flatEnable) {
            g_injectFailCount++;
            Log("vertex shader %u: no main() found, left as is", sh);
        }
    }
    VsInfo vi;
    std::string why;
    DWORD t0 = GetTickCount();
    bool renamed = g_cfg.flatEnable && AnalyzeVertexShader(out, vi, why);
    if (GetTickCount() - t0 > 50) Log("flat depth: analysing vertex shader %u (%zu bytes) took %lu ms", sh, out.size(), GetTickCount() - t0);
    if (g_cfg.flatEnable && !renamed) Log("flat depth: vertex shader %u left without geometry shader (%s)", sh, why.c_str());
    if (!hasMain && !renamed) {
        R.ShaderSource(sh, count, str, len);
        std::lock_guard<std::recursive_mutex> lk(g_lock);
        g_vsInfo.erase(sh);
        g_vsPlain.erase(sh);
        if (g_cfg.flatEnable) g_vsBad.insert(sh);
        return;
    }
    const GLchar* p = renamed ? vi.renamed.c_str() : out.c_str();
    R.ShaderSource(sh, 1, &p, nullptr);
    {
        std::lock_guard<std::recursive_mutex> lk(g_lock);
        g_injectedSrc[sh] = src;
        g_vsInfo.erase(sh);
        g_vsPlain.erase(sh);
        g_vsBad.erase(sh);
        if (renamed) {
            g_vsInfo[sh] = std::move(vi);
            g_vsPlain[sh] = out;
        } else if (g_cfg.flatEnable) {
            g_vsBad.insert(sh);
        }
    }
    if (!hasMain) {
        static int logged;
        if (logged++ < 3) Log("vertex shader %u has no main() (a piece of a larger one): outputs renamed for flat depth", sh);
        return;
    }
    if (++g_injectedCount <= 3) Log("vertex shader %u rewritten for stereo (%zu -> %zu bytes)", sh, src.size(), out.size());
}

// Puts the stereo rewrite without renamed outputs back (no flat depth geometry shader for it).
bool UnrenameVertexShader(GLuint sh)
{
    std::string plain;
    {
        std::lock_guard<std::recursive_mutex> lk(g_lock);
        auto it = g_vsPlain.find(sh);
        if (it == g_vsPlain.end()) return false;
        plain = std::move(it->second);
        g_vsPlain.erase(it);
        g_vsInfo.erase(sh);
    }
    const GLchar* p = plain.c_str();
    R.ShaderSource(sh, 1, &p, nullptr);
    R.CompileShader(sh);
    GLint ok = 0;
    R.GetShaderiv(sh, GL_COMPILE_STATUS, &ok);
    return ok != 0;
}

void GLAPIENTRY H_CompileShader(GLuint sh)
{
    R.CompileShader(sh);
    std::string original;
    {
        std::lock_guard<std::recursive_mutex> lk(g_lock);
        auto it = g_injectedSrc.find(sh);
        if (it == g_injectedSrc.end()) return;
        original = std::move(it->second);
        g_injectedSrc.erase(it);
    }
    GLint ok = 0;
    R.GetShaderiv(sh, GL_COMPILE_STATUS, &ok);
    if (ok) return;
    if (g_vsPlain.count(sh)) {
        char log[2048] = {};
        R.GetShaderInfoLog(sh, sizeof(log) - 1, nullptr, log);
        Log("flat depth: renamed vertex shader %u failed to compile, using the plain stereo rewrite:\n%s", sh, log);
        if (UnrenameVertexShader(sh)) return;
    }
    // The rewrite broke this shader: log why and fall back to the original source.
    char log[2048] = {};
    R.GetShaderInfoLog(sh, sizeof(log) - 1, nullptr, log);
    Log("stereo rewrite of vertex shader %u failed to compile, using the original:\n%s", sh, log);
    g_injectFailCount++;
    const GLchar* p = original.c_str();
    R.ShaderSource(sh, 1, &p, nullptr);
    R.CompileShader(sh);
}

// Flat depth: compiles (once per distinct source) and attaches the geometry shader for the program's
// renamed vertex shader. Returns the vertex shader it was made for, or 0.
GLuint AttachFlatGs(GLuint prog, GLuint& gsOut)
{
    // GL calls stay outside g_lock: WineD3D drives GL from several threads, and a driver call made
    // while holding the lock can wait on a thread that is itself waiting for the lock.
    gsOut = 0;
    if (!g_cfg.flatEnable || !t_ctx || t_ownShaders || !R.GetAttachedShaders || !R.DetachShader) return 0;
    GLuint shaders[8];
    GLsizei n = 0;
    R.GetAttachedShaders(prog, 8, &n, shaders);
    GLint types[8] = {};
    for (GLsizei i = 0; i < n; ++i) R.GetShaderiv(shaders[i], GL_SHADER_TYPE, &types[i]);
    // Every vertex shader piece of the program counts: WineD3D links a main() piece with a piece that
    // writes the outputs. The geometry shader passes the outputs of all of them.
    GLuint vs = 0;
    VsInfo info;
    bool incomplete = false;
    std::vector<GLuint> detach, renamed;
    {
        std::lock_guard<std::recursive_mutex> lk(g_lock);
        for (GLsizei i = 0; i < n; ++i) {
            if ((GLenum)types[i] == GL_GEOMETRY_SHADER) {
                bool ours = false;
                for (auto& kv : g_flatGs) ours |= kv.second == shaders[i] && kv.first.first == t_ctx->rc;
                if (!ours) return 0;   // the game has its own geometry shader
                detach.push_back(shaders[i]);   // relink: attach the one matching the current vertex shaders
            } else if ((GLenum)types[i] == GL_VERTEX_SHADER) {
                auto it = g_vsInfo.find(shaders[i]);
                if (it == g_vsInfo.end()) {
                    incomplete = true;   // not analysed (or failed): its outputs would not get through
                    continue;
                }
                renamed.push_back(shaders[i]);
                if (!vs) {
                    vs = shaders[i];
                    info = it->second;
                } else {
                    MergeVsInfo(info, it->second);
                }
            }
        }
    }
    for (GLuint d : detach) R.DetachShader(prog, d);
    if (!vs) return 0;
    if (incomplete) {
        Log("flat depth: program %u has a vertex shader piece that could not be analysed - no geometry shader", prog);
        for (GLuint r : renamed) UnrenameVertexShader(r);
        return 0;
    }
    Ctx* c = t_ctx;
    std::string src = BuildFlatGs(info, !c || c->compat);
    GLuint gs = 0;
    {
        std::lock_guard<std::recursive_mutex> lk(g_lock);
        auto it = g_flatGs.find({c->rc, src});
        if (it != g_flatGs.end()) gs = it->second;
    }
    if (!gs) {
        gs = R.CreateShader(GL_GEOMETRY_SHADER);
        const GLchar* p = src.c_str();
        R.ShaderSource(gs, 1, &p, nullptr);
        R.CompileShader(gs);
        GLint ok = 0;
        R.GetShaderiv(gs, GL_COMPILE_STATUS, &ok);
        if (!ok) {
            char log[2048] = {};
            R.GetShaderInfoLog(gs, sizeof(log) - 1, nullptr, log);
            Log("flat depth: geometry shader failed to compile:\n%s\n%s", log, src.c_str());
            R.DeleteShader(gs);
            UnrenameVertexShader(vs);
            return 0;
        }
        static int logged;
        if (logged++ < 2) Log("flat depth: geometry shader %u for vertex shader %u:\n%s", gs, vs, src.c_str());
        std::lock_guard<std::recursive_mutex> lk(g_lock);
        GLuint& slot = g_flatGs[{c->rc, src}];
        if (slot) {
            R.DeleteShader(gs);   // another thread made the same one meanwhile
            gs = slot;
        } else {
            slot = gs;
        }
    }
    R.AttachShader(prog, gs);
    gsOut = gs;
    return vs;
}

// [Flat] silhouette: rewrites the fragment shader of a listed program before it links (see
// InjectSilhouette). The shader object keeps the rewrite in every program it is linked into; s3d_S
// stays 0 there, which leaves the picture as it was.
void SilhouetteRewrite(GLuint prog)
{
    if (g_cfg.silhouette.empty() || !t_ctx || t_ownShaders || !R.GetAttachedShaders) return;
    GLuint shaders[8];
    GLsizei n = 0;
    R.GetAttachedShaders(prog, 8, &n, shaders);
    GLint types[8] = {};
    for (GLsizei i = 0; i < n; ++i) R.GetShaderiv(shaders[i], GL_SHADER_TYPE, &types[i]);
    GLuint fs = 0;
    uint32_t hash = 0;
    std::string src;
    {
        std::lock_guard<std::recursive_mutex> lk(g_lock);
        std::vector<uint32_t> hashes;
        for (GLsizei i = 0; i < n; ++i) {
            auto it = g_shaderHash.find(shaders[i]);
            if (it != g_shaderHash.end()) hashes.push_back(it->second);
        }
        std::sort(hashes.begin(), hashes.end());
        hash = hashes.empty() ? 0 : Fnv1a(hashes.data(), hashes.size() * sizeof(uint32_t));
        if (std::find(g_cfg.silhouette.begin(), g_cfg.silhouette.end(), hash) == g_cfg.silhouette.end()) return;
        for (GLsizei i = 0; i < n && !fs; ++i)
            if ((GLenum)types[i] == GL_FRAGMENT_SHADER) fs = shaders[i];
        if (!fs || g_silFs.count(fs)) return;
        auto it = g_shaderSrc.find(fs);
        if (it == g_shaderSrc.end()) return;
        src = it->second;
    }
    std::string out, why;
    if (!InjectSilhouette(src, g_cfg.silUv, g_cfg.silTex, out, why)) {
        Log("silhouette depth: fragment shader %u of program %u (hash %08x) left as is: %s", fs, prog, hash, why.c_str());
        return;
    }
    const GLchar* p = out.c_str();
    R.ShaderSource(fs, 1, &p, nullptr);
    R.CompileShader(fs);
    GLint ok = 0;
    R.GetShaderiv(fs, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[2048] = {};
        R.GetShaderInfoLog(fs, sizeof(log) - 1, nullptr, log);
        Log("silhouette depth: rewritten fragment shader %u failed to compile, using the original:\n%s", fs, log);
        p = src.c_str();
        R.ShaderSource(fs, 1, &p, nullptr);
        R.CompileShader(fs);
        return;
    }
    std::lock_guard<std::recursive_mutex> lk(g_lock);
    g_silFs.insert(fs);
    Log("silhouette depth: fragment shader %u of program %u (hash %08x) rewritten", fs, prog, hash);
}

void GLAPIENTRY H_LinkProgram(GLuint prog)
{
    SilhouetteRewrite(prog);
    GLuint gs = 0, flatVs = AttachFlatGs(prog, gs);
    const ULONGLONG t0 = GetTickCount64();
    R.LinkProgram(prog);
    GLint linked = 0;
    R.GetProgramiv(prog, GL_LINK_STATUS, &linked);
    const unsigned ms = (unsigned)(GetTickCount64() - t0);
    if (gs && !linked) {
        char log[2048] = {};
        R.GetProgramInfoLog(prog, sizeof(log) - 1, nullptr, log);
        Log("flat depth: program %u did not link with the geometry shader, linking without:\n%s", prog, log);
        R.DetachShader(prog, gs);
        gs = 0;
        UnrenameVertexShader(flatVs);
        R.LinkProgram(prog);
        R.GetProgramiv(prog, GL_LINK_STATUS, &linked);
    }
    GLint loc = linked ? R.GetUniformLocation(prog, "s3d_P") : -1;
    GLuint shaders[8];
    GLsizei n = 0;
    if (R.GetAttachedShaders) R.GetAttachedShaders(prog, 8, &n, shaders);
    std::lock_guard<std::recursive_mutex> lk(g_lock);
    std::vector<uint32_t> hashes;
    for (GLsizei i = 0; i < n; ++i) {
        auto it = g_shaderHash.find(shaders[i]);
        if (it != g_shaderHash.end()) hashes.push_back(it->second);
    }
    std::sort(hashes.begin(), hashes.end());
    ProgInfo pi;
    pi.loc = loc;
    pi.locQ = linked ? R.GetUniformLocation(prog, "s3d_Q") : -1;
    pi.gs = linked && gs;
    if (linked && R.GetActiveUniform) {   // sampler uniforms, for SampledUnits
        GLint nu = 0;
        R.GetProgramiv(prog, GL_ACTIVE_UNIFORMS, &nu);
        for (GLint i = 0; i < nu; ++i) {
            char name[128];
            GLsizei len = 0;
            GLint size = 0;
            GLenum type = 0;
            R.GetActiveUniform(prog, (GLuint)i, sizeof(name), &len, &size, &type, name);
            const bool sampler = (type >= 0x8B5D && type <= 0x8B64) || (type >= 0x8DC0 && type <= 0x8DD8) ||
                                 (type >= 0x900C && type <= 0x900F) || (type >= 0x9108 && type <= 0x910D);
            if (!sampler) continue;
            const GLint loc0 = R.GetUniformLocation(prog, name);
            for (GLint k = 0; loc0 >= 0 && k < size; ++k) pi.samplerLocs.push_back(loc0 + k);
        }
        pi.samplersKnown = true;
    }
    if (pi.gs) {
        pi.locF = R.GetUniformLocation(prog, "s3d_F");
        pi.locD = R.GetUniformLocation(prog, "s3d_D");
        pi.gs = pi.locF >= 0 && pi.locD >= 0;
        if (!g_cfg.silhouette.empty()) pi.locS = R.GetUniformLocation(prog, "s3d_S");
    }
    pi.hash = hashes.empty() ? 0 : Fnv1a(hashes.data(), hashes.size() * sizeof(uint32_t));
    auto ov = pi.hash ? g_cfg.overrides.find(pi.hash) : g_cfg.overrides.end();
    if (ov != g_cfg.overrides.end()) pi.mode = ov->second;
    g_prog[prog] = pi;
    if (g_cfg.dumpShaders && pi.hash) DumpProgram(prog, pi.hash, shaders, n);
    static unsigned links;
    if (++links <= 3 || g_cfg.logLevel >= 2 || pi.mode != kModeStereo || g_cfg.flatEnable)
        Log("program %u linked=%d s3d_P=%d%s%s hash %08x%s%ls (%u ms)", prog, linked, loc, pi.gs ? " flat-gs" : "",
            pi.locS >= 0 ? " silhouette" : "", pi.hash, pi.mode != kModeStereo ? " override: " : "",
            pi.mode != kModeStereo ? ModeName(pi.mode) : L"", ms);
}

// ---- ARB assembly vertex programs (Cg arbvp1: PSSG games) ---------------------------------------
void GLAPIENTRY H_BindProgramARB(GLenum target, GLuint prog)
{
    R.BindProgramARB(target, prog);
    if (Ctx* c = t_ctx) {
        if (target == GL_VERTEX_PROGRAM_ARB) c->arbVp = prog;
        else if (target == GL_FRAGMENT_PROGRAM_ARB) c->arbFp = prog;
    }
}

void GLAPIENTRY H_DeleteProgramsARB(GLsizei n, const GLuint* progs)
{
    R.DeleteProgramsARB(n, progs);
    std::lock_guard<std::recursive_mutex> lk(g_lock);
    for (GLsizei i = 0; progs && i < n; ++i) {
        g_arbVp.erase(progs[i]);
        g_arbFpUnits.erase(progs[i]);
    }
}

void GLAPIENTRY H_ProgramStringARB(GLenum target, GLenum format, GLsizei len, const void* str)
{
    Ctx* c = t_ctx;
    if (c && target == GL_FRAGMENT_PROGRAM_ARB && format == GL_PROGRAM_FORMAT_ASCII_ARB && str && len > 0) {
        // the texture units it reads ("texture[n]"), for SampledUnits: other units may hold stale render targets
        const std::string src((const char*)str, (size_t)len);
        uint32_t m = 0;
        for (size_t at = src.find("texture["); at != std::string::npos; at = src.find("texture[", at + 8)) {
            const int u = atoi(src.c_str() + at + 8);
            if (u >= 0 && u < 32) m |= 1u << u;
        }
        std::lock_guard<std::recursive_mutex> lk(g_lock);
        g_arbFpUnits[c->arbFp] = m;
    }
    if (!c || !g_cfg.arbPrograms || target != GL_VERTEX_PROGRAM_ARB || format != GL_PROGRAM_FORMAT_ASCII_ARB || !str || len <= 0 ||
        !R.GetProgramivARB || !R.ProgramEnvParameter4fARB) {
        R.ProgramStringARB(target, format, len, str);
        return;
    }
    if (c->arbEnv < 0) {   // the last env parameter: engines use local parameters (Cg) or the first ones
        GLint n = 0;
        R.GetProgramivARB(GL_VERTEX_PROGRAM_ARB, GL_MAX_PROGRAM_ENV_PARAMETERS_ARB, &n);
        c->arbEnv = n > 0 ? n - 1 : 95;
    }
    const std::string src((const char*)str, (size_t)len);
    std::string out;
    if (!InjectArbVertexProgram(src, c->arbEnv, out)) {
        R.ProgramStringARB(target, format, len, str);
        std::lock_guard<std::recursive_mutex> lk(g_lock);
        g_arbVp.erase(c->arbVp);
        return;
    }
    R.ProgramStringARB(target, format, (GLsizei)out.size(), out.c_str());
    GLint pos = -1;
    R.GetIntegerv(GL_PROGRAM_ERROR_POSITION_ARB, &pos);
    if (pos != -1) {
        // the rewrite did not assemble: log why, load the original and clear the error it left
        const char* err = (const char*)R.GetString(GL_PROGRAM_ERROR_STRING_ARB);
        static int logged;
        if (logged++ < 3) Log("ARB vertex program %u: stereo rewrite failed at %d (%s), using the original:\n%s", c->arbVp, pos, err ? err : "", out.c_str());
        static auto getError = (GLenum(WINAPI*)())GetProcAddress(RealOpenGL(), "glGetError");
        if (getError) while (getError() != 0) {}
        R.ProgramStringARB(target, format, len, str);
        std::lock_guard<std::recursive_mutex> lk(g_lock);
        g_arbVp.erase(c->arbVp);
        return;
    }
    std::lock_guard<std::recursive_mutex> lk(g_lock);
    g_arbVp.insert(c->arbVp);
    static unsigned count;
    if (++count <= 3) Log("ARB vertex program %u rewritten for stereo (env[%d], %zu -> %zu bytes)", c->arbVp, c->arbEnv, src.size(), out.size());
}

void GLAPIENTRY H_UseProgram(GLuint prog)
{
    R.UseProgram(prog);
    if (Ctx* c = t_ctx) c->program = prog;
}

void GLAPIENTRY H_DeleteProgram(GLuint prog)
{
    {
        std::lock_guard<std::recursive_mutex> lk(g_lock);
        Ctx* c = t_ctx;
        if (!c || c->program != prog) g_prog.erase(prog);
    }
    R.DeleteProgram(prog);
}

void GLAPIENTRY H_DeleteObjectARB(GLuint obj)
{
    {
        std::lock_guard<std::recursive_mutex> lk(g_lock);
        Ctx* c = t_ctx;
        if (!c || c->program != obj) g_prog.erase(obj);
        g_injectedSrc.erase(obj);
    }
    R.DeleteObjectARB(obj);
}

void GLAPIENTRY H_ProgramBinary(GLuint prog, GLenum fmt, const void* bin, GLsizei len)
{
    if (g_cfg.inject && g_cfg.blockProgramBinary) {
        // Leave the program unlinked: the game sees LINK_STATUS false and rebuilds from source,
        // which goes through the shader rewrite.
        if (g_blockedBinaries++ < 8) Log("glProgramBinary(%u) blocked so the game compiles from source", prog);
        return;
    }
    R.ProgramBinary(prog, fmt, bin, len);
    std::lock_guard<std::recursive_mutex> lk(g_lock);
    g_prog[prog] = ProgInfo{};
}

void GLAPIENTRY H_DrawArraysInstanced(GLenum mode, GLint first, GLsizei count, GLsizei inst)
{
    TraceDraw("DrawArraysInstanced", mode, count);
    StereoDraw([&] { R.DrawArraysInstanced(mode, first, count, inst); });
}

void GLAPIENTRY H_DrawElementsInstanced(GLenum mode, GLsizei count, GLenum type, const void* idx, GLsizei inst)
{
    TraceDraw("DrawElementsInstanced", mode, count);
    StereoDraw([&] { R.DrawElementsInstanced(mode, count, type, idx, inst); });
}

void GLAPIENTRY H_DrawRangeElements(GLenum mode, GLuint start, GLuint end, GLsizei count, GLenum type, const void* idx)
{
    TraceDraw("DrawRangeElements", mode, count);
    StereoDraw([&] { R.DrawRangeElements(mode, start, end, count, type, idx); });
}

void GLAPIENTRY H_DrawElementsBaseVertex(GLenum mode, GLsizei count, GLenum type, const void* idx, GLint base)
{
    TraceDraw("DrawElementsBaseVertex", mode, count);
    StereoDraw([&] { R.DrawElementsBaseVertex(mode, count, type, idx, base); });
}

void GLAPIENTRY H_DrawRangeElementsBaseVertex(GLenum mode, GLuint start, GLuint end, GLsizei count, GLenum type,
                                              const void* idx, GLint base)
{
    t_drawMode = mode;
    StereoDraw([&] { R.DrawRangeElementsBaseVertex(mode, start, end, count, type, idx, base); });
}

void GLAPIENTRY H_DrawElementsInstancedBaseVertex(GLenum mode, GLsizei count, GLenum type, const void* idx,
                                                  GLsizei inst, GLint base)
{
    t_drawMode = mode;
    StereoDraw([&] { R.DrawElementsInstancedBaseVertex(mode, count, type, idx, inst, base); });
}

void GLAPIENTRY H_DrawArraysInstancedBaseInstance(GLenum mode, GLint first, GLsizei count, GLsizei inst, GLuint bi)
{
    TraceDraw("DrawArraysInstancedBaseInstance", mode, count);
    StereoDraw([&] { R.DrawArraysInstancedBaseInstance(mode, first, count, inst, bi); });
}

void GLAPIENTRY H_DrawElementsInstancedBaseInstance(GLenum mode, GLsizei count, GLenum type, const void* idx,
                                                    GLsizei inst, GLuint bi)
{
    TraceDraw("DrawElementsInstancedBaseInstance", mode, count);
    StereoDraw([&] { R.DrawElementsInstancedBaseInstance(mode, count, type, idx, inst, bi); });
}

void GLAPIENTRY H_DrawElementsInstancedBaseVertexBaseInstance(GLenum mode, GLsizei count, GLenum type, const void* idx,
                                                              GLsizei inst, GLint bv, GLuint bi)
{
    TraceDraw("DrawElementsInstancedBaseVertexBaseInstance", mode, count);
    StereoDraw([&] { R.DrawElementsInstancedBaseVertexBaseInstance(mode, count, type, idx, inst, bv, bi); });
}

void GLAPIENTRY H_DrawArraysIndirect(GLenum mode, const void* ind)
{
    TraceDraw("DrawArraysIndirect", mode, -1);
    StereoDraw([&] { R.DrawArraysIndirect(mode, ind); });
}

void GLAPIENTRY H_DrawElementsIndirect(GLenum mode, GLenum type, const void* ind)
{
    TraceDraw("DrawElementsIndirect", mode, -1);
    StereoDraw([&] { R.DrawElementsIndirect(mode, type, ind); });
}

void GLAPIENTRY H_MultiDrawArraysIndirect(GLenum mode, const void* ind, GLsizei n, GLsizei stride)
{
    TraceDraw("MultiDrawArraysIndirect", mode, -1);
    StereoDraw([&] { R.MultiDrawArraysIndirect(mode, ind, n, stride); });
}

void GLAPIENTRY H_MultiDrawElementsIndirect(GLenum mode, GLenum type, const void* ind, GLsizei n, GLsizei stride)
{
    TraceDraw("MultiDrawElementsIndirect", mode, -1);
    StereoDraw([&] { R.MultiDrawElementsIndirect(mode, type, ind, n, stride); });
}

void GLAPIENTRY H_MultiDrawArrays(GLenum mode, const GLint* first, const GLsizei* count, GLsizei n)
{
    t_drawMode = mode;
    StereoDraw([&] { R.MultiDrawArrays(mode, first, count, n); });
}

void GLAPIENTRY H_MultiDrawElements(GLenum mode, const GLsizei* count, GLenum type, const void* const* idx, GLsizei n)
{
    t_drawMode = mode;
    StereoDraw([&] { R.MultiDrawElements(mode, count, type, idx, n); });
}

void GLAPIENTRY H_ClearBufferfv(GLenum buf, GLint drawbuf, const GLfloat* v)
{
    StereoTargetOp([&] { R.ClearBufferfv(buf, drawbuf, v); }, buf == GL_COLOR);
}

void GLAPIENTRY H_ClearBufferiv(GLenum buf, GLint drawbuf, const GLint* v)
{
    StereoTargetOp([&] { R.ClearBufferiv(buf, drawbuf, v); }, buf == GL_COLOR);
}

void GLAPIENTRY H_ClearBufferuiv(GLenum buf, GLint drawbuf, const GLuint* v)
{
    StereoTargetOp([&] { R.ClearBufferuiv(buf, drawbuf, v); });
}

void GLAPIENTRY H_ClearBufferfi(GLenum buf, GLint drawbuf, GLfloat d, GLint s)
{
    StereoTargetOp([&] { R.ClearBufferfi(buf, drawbuf, d, s); }, false);
}

HGLRC GLAPIENTRY H_wglCreateContextAttribsARB(HDC dc, HGLRC share, const int* attribs)
{
    HGLRC rc = R.wglCreateContextAttribsARB(dc, share, attribs);
    char buf[256] = {};
    size_t n = 0;
    for (int i = 0; attribs && attribs[i] && n < sizeof(buf) - 32; i += 2)
        n += sprintf_s(buf + n, sizeof(buf) - n, " 0x%x=0x%x", attribs[i], attribs[i + 1]);
    Log("wglCreateContextAttribsARB(dc=%p, share=%p,%s) = %p", dc, share, buf, rc);
    return rc;
}

void GLAPIENTRY H_GetFramebufferAttachmentParameteriv(GLenum target, GLenum att, GLenum pname, GLint* params)
{
    Ctx* c = t_ctx;
    const GLenum bindTarget = target == GL_READ_FRAMEBUFFER ? GL_READ_FRAMEBUFFER : GL_DRAW_FRAMEBUFFER;
    const bool def = c && c->vbbLive && (bindTarget == GL_READ_FRAMEBUFFER ? c->readFb : c->drawFb) == 0;
    if (def) R.BindFramebuffer(bindTarget, 0);
    R.GetFramebufferAttachmentParameteriv(target, att, pname, params);
    if (def) R.BindFramebuffer(bindTarget, c->vbbFbo);
}

BOOL GLAPIENTRY H_wglSwapIntervalEXT(int interval)
{
    const int use = g_cfg.swapInterval >= 0 ? g_cfg.swapInterval : interval;
    static int logged;
    if (logged++ < 5) Log("wglSwapIntervalEXT(%d)%s", interval, use != interval ? " - overridden by [Display] swap_interval" : "");
    return R.wglSwapIntervalEXT(use);
}

struct HookEntry {
    const char* name;
    PROC hook;
    void** slot;
};

#define HOOK(glname, fn, rslot) {glname, (PROC)fn, (void**)&R.rslot}
const HookEntry kHooks[] = {
    HOOK("glBindFramebuffer", H_BindFramebuffer, BindFramebuffer),
    HOOK("glBindFramebufferEXT", H_BindFramebuffer, BindFramebuffer),
    HOOK("glDeleteFramebuffers", H_DeleteFramebuffers, DeleteFramebuffers),
    HOOK("glDeleteFramebuffersEXT", H_DeleteFramebuffers, DeleteFramebuffers),
    HOOK("glFramebufferTexture2D", H_FramebufferTexture2D, FramebufferTexture2D),
    HOOK("glFramebufferTexture2DEXT", H_FramebufferTexture2D, FramebufferTexture2D),
    HOOK("glFramebufferTexture", H_FramebufferTexture, FramebufferTexture),
    HOOK("glFramebufferTextureARB", H_FramebufferTexture, FramebufferTexture),
    HOOK("glFramebufferTextureEXT", H_FramebufferTexture, FramebufferTexture),
    HOOK("glFramebufferTextureLayer", H_FramebufferTextureLayer, FramebufferTextureLayer),
    HOOK("glFramebufferTextureLayerARB", H_FramebufferTextureLayer, FramebufferTextureLayer),
    HOOK("glFramebufferTextureLayerEXT", H_FramebufferTextureLayer, FramebufferTextureLayer),
    HOOK("glFramebufferRenderbuffer", H_FramebufferRenderbuffer, FramebufferRenderbuffer),
    HOOK("glFramebufferRenderbufferEXT", H_FramebufferRenderbuffer, FramebufferRenderbuffer),
    HOOK("glDrawBuffers", H_DrawBuffers, DrawBuffers),
    HOOK("glDrawBuffersARB", H_DrawBuffers, DrawBuffers),
    HOOK("glDrawBuffersATI", H_DrawBuffers, DrawBuffers),
    HOOK("glBindRenderbuffer", H_BindRenderbuffer, BindRenderbuffer),
    HOOK("glBindRenderbufferEXT", H_BindRenderbuffer, BindRenderbuffer),
    HOOK("glRenderbufferStorage", H_RenderbufferStorage, RenderbufferStorage),
    HOOK("glRenderbufferStorageEXT", H_RenderbufferStorage, RenderbufferStorage),
    HOOK("glRenderbufferStorageMultisample", H_RenderbufferStorageMultisample, RenderbufferStorageMultisample),
    HOOK("glRenderbufferStorageMultisampleEXT", H_RenderbufferStorageMultisample, RenderbufferStorageMultisample),
    HOOK("glDeleteRenderbuffers", H_DeleteRenderbuffers, DeleteRenderbuffers),
    HOOK("glDeleteRenderbuffersEXT", H_DeleteRenderbuffers, DeleteRenderbuffers),
    HOOK("glBlitFramebuffer", H_BlitFramebuffer, BlitFramebuffer),
    HOOK("glBlitFramebufferEXT", H_BlitFramebuffer, BlitFramebuffer),
    HOOK("glActiveTexture", H_ActiveTexture, ActiveTexture),
    HOOK("glActiveTextureARB", H_ActiveTexture, ActiveTexture),
    HOOK("glGenerateMipmap", H_GenerateMipmap, GenerateMipmap),
    HOOK("glGenerateMipmapEXT", H_GenerateMipmap, GenerateMipmap),
    HOOK("glTexStorage2D", H_TexStorage2D, TexStorage2D),
    HOOK("glTextureView", H_TextureView, TextureView),
    HOOK("glCopyImageSubData", H_CopyImageSubData, CopyImageSubData),
    HOOK("glTexImage2DMultisample", H_TexImage2DMultisample, TexImage2DMultisample),
    HOOK("glShaderSource", H_ShaderSource, ShaderSource),
    HOOK("glShaderSourceARB", H_ShaderSource, ShaderSource),
    HOOK("glCompileShader", H_CompileShader, CompileShader),
    HOOK("glCompileShaderARB", H_CompileShader, CompileShader),
    HOOK("glLinkProgram", H_LinkProgram, LinkProgram),
    HOOK("glLinkProgramARB", H_LinkProgram, LinkProgram),
    HOOK("glUseProgram", H_UseProgram, UseProgram),
    HOOK("glUseProgramObjectARB", H_UseProgram, UseProgram),
    HOOK("glDeleteProgram", H_DeleteProgram, DeleteProgram),
    HOOK("glDeleteObjectARB", H_DeleteObjectARB, DeleteObjectARB),
    HOOK("glProgramBinary", H_ProgramBinary, ProgramBinary),
    HOOK("glProgramStringARB", H_ProgramStringARB, ProgramStringARB),
    HOOK("glBindProgramARB", H_BindProgramARB, BindProgramARB),
    HOOK("glDeleteProgramsARB", H_DeleteProgramsARB, DeleteProgramsARB),
    HOOK("glDrawArraysInstanced", H_DrawArraysInstanced, DrawArraysInstanced),
    HOOK("glDrawArraysInstancedARB", H_DrawArraysInstanced, DrawArraysInstanced),
    HOOK("glDrawArraysInstancedEXT", H_DrawArraysInstanced, DrawArraysInstanced),
    HOOK("glDrawElementsInstanced", H_DrawElementsInstanced, DrawElementsInstanced),
    HOOK("glDrawElementsInstancedARB", H_DrawElementsInstanced, DrawElementsInstanced),
    HOOK("glDrawElementsInstancedEXT", H_DrawElementsInstanced, DrawElementsInstanced),
    HOOK("glDrawRangeElements", H_DrawRangeElements, DrawRangeElements),
    HOOK("glDrawRangeElementsEXT", H_DrawRangeElements, DrawRangeElements),
    HOOK("glDrawElementsBaseVertex", H_DrawElementsBaseVertex, DrawElementsBaseVertex),
    HOOK("glDrawRangeElementsBaseVertex", H_DrawRangeElementsBaseVertex, DrawRangeElementsBaseVertex),
    HOOK("glDrawElementsInstancedBaseVertex", H_DrawElementsInstancedBaseVertex, DrawElementsInstancedBaseVertex),
    HOOK("glDrawArraysInstancedBaseInstance", H_DrawArraysInstancedBaseInstance, DrawArraysInstancedBaseInstance),
    HOOK("glDrawElementsInstancedBaseInstance", H_DrawElementsInstancedBaseInstance, DrawElementsInstancedBaseInstance),
    HOOK("glDrawElementsInstancedBaseVertexBaseInstance", H_DrawElementsInstancedBaseVertexBaseInstance,
         DrawElementsInstancedBaseVertexBaseInstance),
    HOOK("glDrawArraysIndirect", H_DrawArraysIndirect, DrawArraysIndirect),
    HOOK("glDrawElementsIndirect", H_DrawElementsIndirect, DrawElementsIndirect),
    HOOK("glMultiDrawArraysIndirect", H_MultiDrawArraysIndirect, MultiDrawArraysIndirect),
    HOOK("glMultiDrawElementsIndirect", H_MultiDrawElementsIndirect, MultiDrawElementsIndirect),
    HOOK("glMultiDrawArrays", H_MultiDrawArrays, MultiDrawArrays),
    HOOK("glMultiDrawArraysEXT", H_MultiDrawArrays, MultiDrawArrays),
    HOOK("glMultiDrawElements", H_MultiDrawElements, MultiDrawElements),
    HOOK("glMultiDrawElementsEXT", H_MultiDrawElements, MultiDrawElements),
    HOOK("glClearBufferfv", H_ClearBufferfv, ClearBufferfv),
    HOOK("glClearBufferiv", H_ClearBufferiv, ClearBufferiv),
    HOOK("glClearBufferuiv", H_ClearBufferuiv, ClearBufferuiv),
    HOOK("glClearBufferfi", H_ClearBufferfi, ClearBufferfi),
    HOOK("wglCreateContextAttribsARB", H_wglCreateContextAttribsARB, wglCreateContextAttribsARB),
    HOOK("wglSwapIntervalEXT", H_wglSwapIntervalEXT, wglSwapIntervalEXT),
    HOOK("glGetFramebufferAttachmentParameteriv", H_GetFramebufferAttachmentParameteriv, GetFramebufferAttachmentParameteriv),
    HOOK("glGetFramebufferAttachmentParameterivEXT", H_GetFramebufferAttachmentParameteriv, GetFramebufferAttachmentParameteriv),
};
#undef HOOK

// A game display list may change state (matrices, colours...), so the right eye replays it
// first inside push/pop and the left eye's call is the one whose side effects remain.
template <class F>
void StereoList(F&& call)
{
    Ctx* c = t_ctx;
    if (c && !c->listMode && c->drawFb == 0) c->fb0Touched = true;
    if (!c || c->listMode || !Active(c)) {
        if (c && !c->listMode) {
            EyeState st;
            EyeBegin(c, 0.0f, st);
        }
        call();
        return;
    }
    std::lock_guard<std::recursive_mutex> lk(g_lock);
    GLuint right = RightFor(c, c->drawFb);
    EyeState st;
    if (!right) {
        EyeBegin(c, 0.0f, st);
        call();
        return;
    }
    if (c->compat) {
        GLint mode = GL_MODELVIEW;
        R.GetIntegerv(GL_MATRIX_MODE, &mode);
        R.PushAttrib(GL_ALL_ATTRIB_BITS);
        for (GLenum m : {GL_MODELVIEW, GL_PROJECTION, GL_TEXTURE}) {
            R.MatrixMode(m);
            R.PushMatrix();
        }
        R.MatrixMode(mode);
        R.BindFramebuffer(GL_DRAW_FRAMEBUFFER, right);
        SwapTextures(c, true);
        EyeBegin(c, 1.0f, st);
        call();
        EyeEnd(st);
        SwapTextures(c, false);
        R.BindFramebuffer(GL_DRAW_FRAMEBUFFER, RealFb(c, c->drawFb));
        for (GLenum m : {GL_MODELVIEW, GL_PROJECTION, GL_TEXTURE}) {
            R.MatrixMode(m);
            R.PopMatrix();
        }
        R.PopAttrib();   // also restores the matrix mode
    }
    EyeBegin(c, -1.0f, st);
    call();
    EyeEnd(st);
    g_stats.lists++;
}

}   // namespace

// ---------------------------------------------------------------------------------------------
// entry points from proxy.cpp

PROC HookForName(const char* name, PROC real)
{
    if (!real || !name) return real;
    for (const HookEntry& h : kHooks) {
        if (strcmp(h.name, name) != 0) continue;
        if (!*h.slot) *h.slot = (void*)real;
        return h.hook;
    }
    return real;
}

void OnMakeCurrent(HDC dc, HGLRC rc)
{
    if (!rc) {
        t_ctx = nullptr;
        return;
    }
    // Cg games ask for their profiles once a context is current: redirect that before they can
    static bool cgHooked;
    if (!cgHooked && (g_cfg.cgGlsl > 0 || (g_cfg.cgGlsl < 0 && g_cfg.flatEnable))) {
        cgHooked = true;
        InstallCgGlslHooks();
    }
    std::lock_guard<std::recursive_mutex> lk(g_lock);
    Ctx*& c = g_ctx[rc];
    if (!c) {
        c = new Ctx;
        c->rc = rc;
        LoadRealExtensions();
        const char* ver = (const char*)R.GetString(GL_VERSION);
        int major = 0, minor = 0;
        if (ver) sscanf_s(ver, "%d.%d", &major, &minor);
        if (major > 3 || (major == 3 && minor >= 2)) {
            GLint mask = 0;
            R.GetIntegerv(GL_CONTEXT_PROFILE_MASK, &mask);
            c->compat = (mask & GL_CONTEXT_CORE_PROFILE_BIT) == 0;
        }
        Log("context %p: %s | %s | GL %s | GLSL %s | %s profile", rc, (const char*)R.GetString(GL_VENDOR),
            (const char*)R.GetString(GL_RENDERER), ver ? ver : "?", (const char*)R.GetString(GL_SHADING_LANGUAGE_VERSION),
            c->compat ? "compatibility" : "core");
    }
    t_ctx = c;
    if (c->isMain) UpdateWindowSize(c, dc);
    static int traced;
    if (g_cfg.logLevel >= 2 && traced++ < 40) Log("wglMakeCurrent(dc %p, rc %p) thread %lu frame %u", dc, rc, GetCurrentThreadId(), g_frame);
}

void OnDeleteContext(HGLRC rc)
{
    std::lock_guard<std::recursive_mutex> lk(g_lock);
    auto it = g_ctx.find(rc);
    if (it == g_ctx.end()) return;
    if (t_ctx == it->second) t_ctx = nullptr;
    if (g_mainRc == rc) g_mainRc = nullptr;
    for (auto f = g_flatGs.begin(); f != g_flatGs.end();) f = f->first.first == rc ? g_flatGs.erase(f) : std::next(f);
    delete it->second;
    g_ctx.erase(it);
}

namespace {

// Hang watchdog: if the render thread finishes no frame for 4 s, log where it is (instruction
// pointer and the return addresses on its stack), up to three times.
volatile LONG g_watchFrame;
DWORD g_renderTid;

DWORD WINAPI HangWatch(void*)
{
    LONG last = -1;
    int still = 0, reports = 0;
    static ULONG_PTR words[1024];   // the stopped thread's stack (no heap use while it is suspended)
    for (;;) {
        Sleep(1000);
        LONG f = g_watchFrame;
        still = f == last ? still + 1 : 0;
        last = f;
        if (still != 4 || reports >= 3) continue;
        HANDLE t = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, g_renderTid);
        if (!t) continue;
        CONTEXT ctx{};
        ctx.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
        int n = 0;
        const void* ip = nullptr;
        // While the thread is suspended it may hold the heap, loader or log lock: only copy raw
        // values here, describe them after resuming it.
        if (SuspendThread(t) != (DWORD)-1) {
            if (GetThreadContext(t, &ctx)) {
#if defined(_M_X64)
                ip = (const void*)ctx.Rip;
                const ULONG_PTR* sp = (const ULONG_PTR*)ctx.Rsp;
#else
                ip = (const void*)(ULONG_PTR)ctx.Eip;
                const ULONG_PTR* sp = (const ULONG_PTR*)(ULONG_PTR)ctx.Esp;
#endif
                MEMORY_BASIC_INFORMATION mbi{};
                if (VirtualQuery(sp, &mbi, sizeof(mbi)) && mbi.State == MEM_COMMIT) {
                    const ULONG_PTR end = (ULONG_PTR)mbi.BaseAddress + mbi.RegionSize;
                    while (n < 1024 && (ULONG_PTR)(sp + n + 1) <= end) {
                        words[n] = sp[n];
                        ++n;
                    }
                }
            }
            ResumeThread(t);
        }
        CloseHandle(t);
        if (!ip) continue;
        ++reports;
        char where[300];
        DescribeAddress(ip, where, sizeof(where));
        std::string stack;
        int shown = 0;
        for (int i = 0; i < n && shown < 16; ++i) {
            MEMORY_BASIC_INFORMATION mbi{};
            if (words[i] < 0x10000 || !VirtualQuery((const void*)words[i], &mbi, sizeof(mbi)) || mbi.Type != MEM_IMAGE ||
                !(mbi.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)))
                continue;   // only addresses inside executable image code
            char a[300];
            DescribeAddress((const void*)words[i], a, sizeof(a));
            stack += std::string(shown ? " < " : "") + a;
            ++shown;
        }
        Log("HANG? no frame for 4 s (frame %ld): render thread at %s | stack: %s", f, where, stack.c_str());
    }
}

}   // namespace

// OGL3D.ini edited while the game runs (tuning without restarting): re-read it. Checked about once
// a second; our own saves (Ctrl+F7, auto_start) reload the same values.
void ReloadConfigIfChanged()
{
    static FILETIME last;
    static DWORD lastCheck;
    DWORD now = GetTickCount();
    if (now - lastCheck < 1000) return;
    lastCheck = now;
    WIN32_FILE_ATTRIBUTE_DATA fa;
    if (!GetFileAttributesExW(IniPath().c_str(), GetFileExInfoStandard, &fa)) return;
    if (!last.dwLowDateTime && !last.dwHighDateTime) {
        last = fa.ftLastWriteTime;
        return;
    }
    if (CompareFileTime(&fa.ftLastWriteTime, &last) == 0) return;
    last = fa.ftLastWriteTime;
    Sleep(50);   // let the editor finish writing
    LoadConfig();
    g_paramVer++;
    for (auto& kv : g_ctx) kv.second->keyDepth.clear();
    Log("OGL3D.ini changed - settings reloaded");
    OsdMessage(KoreanUi() ? L"설정 다시 읽음 (OGL3D.ini)" : L"Settings reloaded (OGL3D.ini)", 2.0);
}

bool OnSwapBuffers(HDC dc)
{
    Ctx* c = t_ctx;
    if (!c) return false;
    std::lock_guard<std::recursive_mutex> lk(g_lock);
    if (!g_mainRc) {
        g_mainRc = c->rc;
        c->isMain = true;
        Log("main context %p (first to present)", c->rc);
        UpdateStatus();
        g_renderTid = GetCurrentThreadId();
        if (HANDLE t = CreateThread(nullptr, 0, HangWatch, nullptr, 0, nullptr)) CloseHandle(t);
    }
    if (!c->isMain) {
        static int logged;
        if (logged++ < 5) Log("SwapBuffers on context %p (not the main one, not composed)", c->rc);
        return false;
    }
    InterlockedIncrement(&g_watchFrame);
    UpdateWindowSize(c, dc);
    if (c->winW <= 0) return false;
    KeyHookFor(WindowFromDC(dc));

    static LARGE_INTEGER freq, last;
    LARGE_INTEGER now;
    if (!freq.QuadPart) QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&now);
    float dt = last.QuadPart ? (float)(now.QuadPart - last.QuadPart) / (float)freq.QuadPart : 0.0f;
    last = now;
    g_frameMs = dt * 1000.0;
    if (g_cfg.flickerProbe) {
        if (g_frameMs > 50) Log("slow frame %u: %.1f ms since the previous swap", g_frame, g_frameMs);
        if (g_frame % 600 == 0) {
            float l[3], r[3];
            if (SrEyePositions(l, r))
                Log("SR eyes L (%.0f, %.0f, %.0f) R (%.0f, %.0f, %.0f) mm", l[0], l[1], l[2], r[0], r[1], r[2]);
        }
    }
    PollHotkeys(std::min(dt, 0.1f));
    HuntFrameEnd();
    ReloadConfigIfChanged();
    if (g_cfg.showValues && g_cfg.autoConvergence) UpdateStatus();   // auto keeps changing the value

    // what the game did in this frame: F = drew with a clear, p = drew without clearing, . = nothing
    {
        const unsigned draws = g_stats.stereoDraws + g_stats.monoDraws + g_stats.immediate + g_stats.lists;
        g_frameKind = draws == 0 && g_stats.clears == 0 ? '.' : g_stats.clears ? 'F' : 'p';
        g_framePattern += g_frameKind;
        if (g_framePattern.size() > 120) g_framePattern.erase(0, g_framePattern.size() - 120);
    }
    const bool presented = Compose(c, dc);
    c->batch = 0;
    c->drawsInBatch = 0;
    c->batchPending = false;
    if (c->frameGroups) c->lastGroupCount = c->frameGroups;
    ComputeLayout(c);
    c->frameGroups = 0;
    c->layoutLastFb = 0;
    c->layoutLastG = -1;
    c->groupId = 0;
    c->groupIds.clear();
    if (c->keyDepth.size() > 4096) c->keyDepth.clear();
    ++g_frame;
    // Ctrl+F12 also traces the draw calls of the frame after the dumped one
    g_traceNow = (!g_cfg.traceFrames.empty() && ListedIn(g_cfg.traceFrames, g_frame)) || g_dumpRequested;
    g_traceDraw = 0;
    if (g_dumpRequested || g_dumpAuto || FrameListed(g_frame)) {
        g_dumpAuto = false;
        DumpFrame(c);
        if (g_dumpRequested) {
            wchar_t buf[96];
            swprintf_s(buf, KoreanUi() ? L"프레임 저장: OGL3D_frame%05u.bmp" : L"Frame saved: OGL3D_frame%05u.bmp", g_frame);
            OsdMessage(buf, 2.5);
        }
        g_dumpRequested = false;
    }
    if (g_cfg.statsEvery > 0 && g_frame % g_cfg.statsEvery == 0) Log("last frames: %s", g_framePattern.c_str());
    if (g_frame <= 3 || (g_cfg.statsEvery > 0 && g_frame % g_cfg.statsEvery == 0)) {
        Log("frame %u: stereo draws %u, mono draws %u, clears %u, blits %u, copies %u, immediate %u, lists %u, "
            "matrix-eye draws %u, uniform sets %u, flushes %u, finishes %u | shaders injected %u, failed %u, fbos %zu",
            g_frame, g_stats.stereoDraws, g_stats.monoDraws, g_stats.clears, g_stats.blits, g_stats.copies,
            g_stats.immediate, g_stats.lists, g_stats.matrixDraws, g_stats.uniformSets, g_stats.flushes,
            g_stats.finishes, g_injectedCount, g_injectFailCount, c->fbo.size());
    }
    g_stats = Stats{};
    g_frameConv = g_cfg.convergence;   // what the next frame will be rendered with
    g_frameSep = SepValue();
    // remember where auto convergence settled, so the next launch does not start from a guess
    if (g_cfg.autoConvergence && g_frame % 600 == 0 &&
        (g_cfg.autoStart <= 0 || std::fabs(std::log(g_cfg.convergence / g_cfg.autoStart)) > 0.05f))
        SaveAutoStart(g_cfg.convergence);
    return presented;
}

// ---------------------------------------------------------------------------------------------
// GL 1.1 exports

extern "C" {

void WINAPI H_glDrawArrays(GLenum mode, GLint first, GLsizei count)
{
    t_drawArrays = true;
    t_drawFirst = first;
    t_drawIndexType = 0;
    TraceDraw("DrawArrays", mode, count);
    StereoDraw([&] { R.DrawArrays(mode, first, count); });
    t_drawArrays = false;
}

void WINAPI H_glDrawElements(GLenum mode, GLsizei count, GLenum type, const void* indices)
{
    t_drawArrays = true;
    t_drawIndexType = type;
    t_drawIndices = indices;
    TraceDraw("DrawElements", mode, count);
    StereoDraw([&] { R.DrawElements(mode, count, type, indices); });
    t_drawArrays = false;
}

// A scissor or viewport call after some draws starts a new batch (AGS: room, then UI overlays) -
// but only within the same framebuffer. Games that render passes into different targets (Door
// Kickers: background cache, scene, composite) reset scissor/viewport per target; those are not UI.
void NextBatch()
{
    Ctx* c = t_ctx;
    if (!c || !c->isMain || !c->drawsInBatch) return;
    c->batchPending = true;   // decided at the next draw, once its framebuffer is bound
    c->drawsInBatch = 0;
}

void WINAPI H_glScissor(GLint x, GLint y, GLsizei w, GLsizei h)
{
    R.Scissor(x, y, w, h);
    NextBatch();
    if (g_traceNow && t_ctx && t_ctx->isMain) Log("T %u.%u Scissor %d,%d,%d,%d", g_frame, g_traceDraw, x, y, w, h);
}

void WINAPI H_glViewport(GLint x, GLint y, GLsizei w, GLsizei h)
{
    R.Viewport(x, y, w, h);
    NextBatch();
    if (g_traceNow && t_ctx && t_ctx->isMain) Log("T %u.%u Viewport %d,%d,%d,%d", g_frame, g_traceDraw, x, y, w, h);
}

void WINAPI H_glClear(GLbitfield mask)
{
    TraceDraw("Clear", mask, 0);
    StereoTargetOp([&] { R.Clear(mask); }, (mask & GL_COLOR_BUFFER_BIT) != 0);
}

void WINAPI H_glBindTexture(GLenum target, GLuint texture)
{
    R.BindTexture(target, texture);
    Ctx* c = t_ctx;
    int slot = SlotOf(target);
    if (c && slot >= 0) c->bound[c->activeUnit][slot] = texture;
}

void WINAPI H_glDeleteTextures(GLsizei n, const GLuint* textures)
{
    Ctx* c = t_ctx;
    {
        std::lock_guard<std::recursive_mutex> lk(g_lock);
        for (GLsizei i = 0; i < n; ++i) {
            auto it = g_tex.find(textures[i]);
            if (it != g_tex.end()) {
                DeleteTexClone(it->second);
                g_tex.erase(it);
            }
            if (c)
                for (int u = 0; u <= c->maxUnit; ++u)
                    for (int s = 0; s < kSlotCount; ++s)
                        if (c->bound[u][s] == textures[i]) c->bound[u][s] = 0;
        }
    }
    R.DeleteTextures(n, textures);
}

void WINAPI H_glTexImage2D(GLenum target, GLint level, GLint ifmt, GLsizei w, GLsizei h, GLint border, GLenum format,
                           GLenum type, const void* pixels)
{
    R.TexImage2D(target, level, ifmt, w, h, border, format, type, pixels);
    Ctx* c = t_ctx;
    int slot = SlotOf(target);
    if (!c || slot < 0) return;
    GLuint id = c->bound[c->activeUnit][slot];
    if (!id) return;
    std::lock_guard<std::recursive_mutex> lk(g_lock);
    TexInfo& ti = g_tex[id];
    if (level == 0) {
        bool resized = ti.w != w || ti.h != h || ti.ifmt != (GLenum)ifmt;
        ti.target = target;
        ti.w = w;
        ti.h = h;
        ti.ifmt = (GLenum)ifmt;
        ti.fmt = format;
        ti.type = type;
        ti.immutable = false;
        if (resized) {
            ti.levels = 1;
            BumpStamp();
        }
    } else {
        ti.levels = std::max(ti.levels, level + 1);
    }
    if (ti.right) {   // keep the clone's storage (and uploaded content) in step
        R.BindTexture(target, ti.right);
        R.TexImage2D(target, level, ifmt, w, h, border, format, type, pixels);
        R.BindTexture(target, id);
    }
}

void WINAPI H_glTexSubImage2D(GLenum target, GLint level, GLint xo, GLint yo, GLsizei w, GLsizei h, GLenum format,
                              GLenum type, const void* pixels)
{
    R.TexSubImage2D(target, level, xo, yo, w, h, format, type, pixels);
    MirrorToClone(target, [&] { R.TexSubImage2D(target, level, xo, yo, w, h, format, type, pixels); });
}

void WINAPI H_glCopyTexImage2D(GLenum target, GLint level, GLenum ifmt, GLint x, GLint y, GLsizei w, GLsizei h,
                               GLint border)
{
    Ctx* c = t_ctx;
    int slot = SlotOf(target);
    if (c && slot >= 0 && c->bound[c->activeUnit][slot]) {
        std::lock_guard<std::recursive_mutex> lk(g_lock);
        TexInfo& ti = g_tex[c->bound[c->activeUnit][slot]];
        if (level == 0) {
            if (ti.w != w || ti.h != h || ti.ifmt != ifmt) BumpStamp();
            ti.target = target;
            ti.w = w;
            ti.h = h;
            ti.ifmt = ifmt;
            ti.immutable = false;
            if (IsDepthFormat(ifmt)) ti.fmt = GL_DEPTH_COMPONENT, ti.type = GL_FLOAT;
            else if (IsDepthStencilFormat(ifmt)) ti.fmt = GL_DEPTH_STENCIL, ti.type = GL_UNSIGNED_INT_24_8;
            else ti.fmt = GL_RGBA, ti.type = GL_UNSIGNED_BYTE;
        }
    }
    StereoCopy(target, [&] { R.CopyTexImage2D(target, level, ifmt, x, y, w, h, border); });
}

void WINAPI H_glCopyTexSubImage2D(GLenum target, GLint level, GLint xo, GLint yo, GLint x, GLint y, GLsizei w, GLsizei h)
{
    StereoCopy(target, [&] { R.CopyTexSubImage2D(target, level, xo, yo, x, y, w, h); });
}

void WINAPI H_glTexParameteri(GLenum target, GLenum pname, GLint param)
{
    R.TexParameteri(target, pname, param);
    MirrorToClone(target, [&] { R.TexParameteri(target, pname, param); });
}

void WINAPI H_glTexParameterf(GLenum target, GLenum pname, GLfloat param)
{
    R.TexParameterf(target, pname, param);
    MirrorToClone(target, [&] { R.TexParameterf(target, pname, param); });
}

void WINAPI H_glTexParameteriv(GLenum target, GLenum pname, const GLint* params)
{
    R.TexParameteriv(target, pname, params);
    MirrorToClone(target, [&] { R.TexParameteriv(target, pname, params); });
}

void WINAPI H_glTexParameterfv(GLenum target, GLenum pname, const GLfloat* params)
{
    R.TexParameterfv(target, pname, params);
    MirrorToClone(target, [&] { R.TexParameterfv(target, pname, params); });
}

void WINAPI H_glFlush()
{
    g_stats.flushes++;
    R.Flush();
}

void WINAPI H_glFinish()
{
    g_stats.finishes++;
    R.Finish();
}

void WINAPI H_glDrawBuffer(GLenum mode)
{
    Ctx* c = t_ctx;
    if (c && c->drawFb == 0) c->fb0DrawBuf = mode;
    R.DrawBuffer(c && c->drawFb == 0 && c->vbbLive ? VbbBuffer(mode) : mode);
    if (!c) return;
    if (c->drawFb == 0 && mode != GL_BACK && mode != GL_BACK_LEFT) {
        static int logged;
        if (logged++ < 5) Log("game draws into default framebuffer buffer 0x%x (not the back buffer)", mode);
    }
    if (FboInfo* f = BoundFbo(c, GL_DRAW_FRAMEBUFFER)) {
        f->nDrawBufs = 1;
        f->drawBufs[0] = mode;
        f->dirty = true;
    }
}

void WINAPI H_glReadBuffer(GLenum mode)
{
    Ctx* c = t_ctx;
    if (c && c->readFb == 0) c->fb0ReadBuf = mode;
    R.ReadBuffer(c && c->readFb == 0 && c->vbbLive ? VbbBuffer(mode) : mode);
    if (!c) return;
    if (FboInfo* f = BoundFbo(c, GL_READ_FRAMEBUFFER)) {
        f->readBuf = mode;
        f->dirty = true;
    }
}

void WINAPI H_glGetIntegerv(GLenum pname, GLint* data)
{
    if (g_cfg.inject && g_cfg.blockProgramBinary) {
        // No binary formats, so games skip their program-binary cache. The format list must stay
        // empty too: a game sizes its buffer from the count we report.
        if (pname == GL_NUM_PROGRAM_BINARY_FORMATS) {
            if (data) *data = 0;
            if (g_cfg.logLevel >= 2) Log("glGetIntegerv(GL_NUM_PROGRAM_BINARY_FORMATS) -> 0");
            return;
        }
        if (pname == GL_PROGRAM_BINARY_FORMATS) {
            Log("glGetIntegerv(GL_PROGRAM_BINARY_FORMATS) -> empty list");
            return;
        }
    }
    if (Ctx* c = t_ctx; c && c->vbbLive && data) {
        switch (pname) {
        case GL_DRAW_FRAMEBUFFER_BINDING:
            *data = (GLint)c->drawFb;
            return;
        case GL_READ_FRAMEBUFFER_BINDING:
            *data = (GLint)c->readFb;
            return;
        case GL_DRAW_BUFFER:
        case GL_DRAW_BUFFER0:
            if (c->drawFb == 0) {
                *data = (GLint)c->fb0DrawBuf;
                return;
            }
            break;
        case GL_READ_BUFFER:
            if (c->readFb == 0) {
                *data = (GLint)c->fb0ReadBuf;
                return;
            }
            break;
        }
    }
    R.GetIntegerv(pname, data);
}

// Immediate mode: the primitive is recorded into a display list while it executes for the left
// eye, then the list is replayed into the right eye.
void WINAPI H_glBegin(GLenum mode)
{
    t_drawMode = mode;
    Ctx* c = t_ctx;
    if (c && !c->listMode && c->drawFb == 0) c->fb0Touched = true;
    if (c && !c->listMode && Active(c)) {
        std::lock_guard<std::recursive_mutex> lk(g_lock);
        GLuint right = RightFor(c, c->drawFb);
        if (right) {
            if (!c->immList) c->immList = R.GenLists(1);
            if (c->immList) {
                R.GetFloatv(GL_CURRENT_COLOR, c->immColor);
                R.GetFloatv(GL_CURRENT_NORMAL, c->immNormal);
                R.GetFloatv(GL_CURRENT_TEXTURE_COORDS, c->immTex);
                EyeBegin(c, -1.0f, c->immEye);
                R.NewList(c->immList, GL_COMPILE_AND_EXECUTE);
                c->immRight = right;
            }
        } else {
            EyeState st;
            EyeBegin(c, 0.0f, st);
        }
    }
    R.Begin(mode);
}

void WINAPI H_glEnd()
{
    R.End();
    Ctx* c = t_ctx;
    if (!c || !c->immRight) return;
    std::lock_guard<std::recursive_mutex> lk(g_lock);
    R.EndList();
    EyeEnd(c->immEye);
    GLfloat color[4], normal[4], tex[4];
    R.GetFloatv(GL_CURRENT_COLOR, color);
    R.GetFloatv(GL_CURRENT_NORMAL, normal);
    R.GetFloatv(GL_CURRENT_TEXTURE_COORDS, tex);
    R.Color4fv(c->immColor);
    R.Normal3fv(c->immNormal);
    R.TexCoord4fv(c->immTex);
    R.BindFramebuffer(GL_DRAW_FRAMEBUFFER, c->immRight);
    SwapTextures(c, true);
    EyeState st;
    EyeBegin(c, 1.0f, st);
    R.CallList(c->immList);
    EyeEnd(st);
    SwapTextures(c, false);
    R.BindFramebuffer(GL_DRAW_FRAMEBUFFER, RealFb(c, c->drawFb));
    R.Color4fv(color);
    R.Normal3fv(normal);
    R.TexCoord4fv(tex);
    c->immRight = 0;
    g_stats.immediate++;
}

void WINAPI H_glNewList(GLuint list, GLenum mode)
{
    R.NewList(list, mode);
    if (Ctx* c = t_ctx) c->listMode = mode;
}

void WINAPI H_glEndList()
{
    R.EndList();
    if (Ctx* c = t_ctx) c->listMode = 0;
}

void WINAPI H_glCallList(GLuint list)
{
    StereoList([&] { R.CallList(list); });
}

void WINAPI H_glCallLists(GLsizei n, GLenum type, const void* lists)
{
    StereoList([&] { R.CallLists(n, type, lists); });
}

}   // extern "C"

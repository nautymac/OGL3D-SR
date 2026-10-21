// On-screen overlay: a few lines of text rasterized with GDI, uploaded as a texture and drawn into
// both eyes at the same position, so it sits at screen depth (readable when woven or in SBS).
// The software cursor (cursor.cpp) is drawn with the same textured quad.
// All GL calls go to the real driver (R.*); the caller saves and restores GL state.
#include "common.h"
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

namespace {

struct OsdState {
    std::mutex lock;
    std::wstring status;        // persistent lines (hunting, Ctrl+F1 values)
    std::wstring message;       // temporary lines
    ULONGLONG messageUntil = 0;
    std::wstring rendered;      // text currently in the texture
    int renderedFont = 0;
    GLuint tex = 0, prog = 0, vao = 0;
    GLint locRect = -1, locTex = -1;
    int texW = 0, texH = 0;
    bool failed = false;
} g;

const char kVs[] = R"(#version 150
uniform vec4 uRect;   // x0, y0, x1, y1 in NDC
out vec2 vUv;
void main()
{
    vec2 c = vec2(float(gl_VertexID & 1), float(gl_VertexID >> 1));
    vUv = vec2(c.x, 1.0 - c.y);
    gl_Position = vec4(mix(uRect.xy, uRect.zw, c), 0.0, 1.0);
}
)";

const char kFs[] = R"(#version 150
uniform sampler2D uTex;
in vec2 vUv;
out vec4 oColor;
void main()
{
    oColor = texture(uTex, vUv);
}
)";

GLuint Compile(GLenum type, const char* src)
{
    GLuint sh = R.CreateShader(type);
    R.ShaderSource(sh, 1, &src, nullptr);
    R.CompileShader(sh);
    GLint ok = 0;
    R.GetShaderiv(sh, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[1024] = {};
        R.GetShaderInfoLog(sh, sizeof(log) - 1, nullptr, log);
        Log("overlay shader failed to compile: %s", log);
        R.DeleteShader(sh);
        return 0;
    }
    return sh;
}

bool BuildProgram()
{
    if (!R.CreateShader || !R.CreateProgram || !R.GenVertexArrays || !R.BindVertexArray) return false;
    GLuint vs = Compile(GL_VERTEX_SHADER, kVs), fs = Compile(GL_FRAGMENT_SHADER, kFs);
    if (!vs || !fs) return false;
    g.prog = R.CreateProgram();
    R.AttachShader(g.prog, vs);
    R.AttachShader(g.prog, fs);
    R.LinkProgram(g.prog);
    R.DeleteShader(vs);
    R.DeleteShader(fs);
    GLint ok = 0;
    R.GetProgramiv(g.prog, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[1024] = {};
        R.GetProgramInfoLog(g.prog, sizeof(log) - 1, nullptr, log);
        Log("overlay program failed to link: %s", log);
        return false;
    }
    g.locRect = R.GetUniformLocation(g.prog, "uRect");
    g.locTex = R.GetUniformLocation(g.prog, "uTex");
    R.GenVertexArrays(1, &g.vao);
    return true;
}

// White-ish text on a translucent dark box, straight alpha, rows top-down.
bool Rasterize(const std::wstring& text, int fontPx, std::vector<uint8_t>& rgba, int& w, int& h)
{
    HDC dc = CreateCompatibleDC(nullptr);
    if (!dc) return false;
    HFONT font = CreateFontW(-fontPx, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                             CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, DEFAULT_PITCH, L"Malgun Gothic");
    HGDIOBJ oldFont = SelectObject(dc, font);
    RECT rc{0, 0, 0, 0};
    DrawTextW(dc, text.c_str(), -1, &rc, DT_CALCRECT | DT_NOPREFIX);
    const int pad = fontPx / 2;
    w = rc.right + 2 * pad;
    h = rc.bottom + pad;
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;   // top-down
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HBITMAP bmp = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    bool ok = bmp && bits;
    if (ok) {
        HGDIOBJ oldBmp = SelectObject(dc, bmp);
        memset(bits, 0, (size_t)w * h * 4);
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, RGB(255, 255, 255));
        RECT tr{pad, pad / 2, w - pad, h - pad / 2};
        DrawTextW(dc, text.c_str(), -1, &tr, DT_NOPREFIX);
        GdiFlush();
        rgba.resize((size_t)w * h * 4);
        const uint8_t* s = (const uint8_t*)bits;
        for (size_t i = 0; i < (size_t)w * h; ++i) {
            int cov = std::max(s[i * 4], std::max(s[i * 4 + 1], s[i * 4 + 2]));
            rgba[i * 4 + 0] = (uint8_t)(255 * cov / 255);
            rgba[i * 4 + 1] = (uint8_t)(236 * cov / 255);
            rgba[i * 4 + 2] = (uint8_t)(170 * cov / 255);
            rgba[i * 4 + 3] = (uint8_t)(150 + (255 - 150) * cov / 255);
        }
        SelectObject(dc, oldBmp);
        DeleteObject(bmp);
    }
    SelectObject(dc, oldFont);
    DeleteObject(font);
    DeleteDC(dc);
    return ok;
}

std::wstring CurrentText()
{
    std::lock_guard<std::mutex> lk(g.lock);
    std::wstring t = g.status;
    // a message that only repeats a status line (values while adjusting) is not shown twice
    if (!g.message.empty() && GetTickCount64() < g.messageUntil && t.find(g.message) == std::wstring::npos) {
        if (!t.empty()) t += L"\n";
        t += g.message;
    }
    return t;
}

}   // namespace

// Uploads w x h RGBA pixels (rows top-down) into tex, made on first use. Unpack state is kept.
void OverlayUpload(GLuint& tex, int w, int h, const uint8_t* rgba)
{
    GLint align = 4, rowLen = 0, skipRows = 0, skipPix = 0, unpackBuf = 0;
    R.GetIntegerv(GL_PIXEL_UNPACK_BUFFER_BINDING, &unpackBuf);
    R.GetIntegerv(GL_UNPACK_ALIGNMENT, &align);
    R.GetIntegerv(GL_UNPACK_ROW_LENGTH, &rowLen);
    R.GetIntegerv(GL_UNPACK_SKIP_ROWS, &skipRows);
    R.GetIntegerv(GL_UNPACK_SKIP_PIXELS, &skipPix);
    if (unpackBuf) R.BindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
    R.PixelStorei(GL_UNPACK_ALIGNMENT, 4);
    R.PixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    R.PixelStorei(GL_UNPACK_SKIP_ROWS, 0);
    R.PixelStorei(GL_UNPACK_SKIP_PIXELS, 0);
    if (!tex) R.GenTextures(1, &tex);
    R.ActiveTexture(GL_TEXTURE0);
    R.BindTexture(GL_TEXTURE_2D, tex);
    R.TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
    R.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    R.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    R.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
    R.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, 0x812F);   // CLAMP_TO_EDGE
    R.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, 0x812F);
    R.PixelStorei(GL_UNPACK_ALIGNMENT, align);
    R.PixelStorei(GL_UNPACK_ROW_LENGTH, rowLen);
    R.PixelStorei(GL_UNPACK_SKIP_ROWS, skipRows);
    R.PixelStorei(GL_UNPACK_SKIP_PIXELS, skipPix);
    if (unpackBuf) R.BindBuffer(GL_PIXEL_UNPACK_BUFFER, (GLuint)unpackBuf);
}

// Draws tex over the NDC rectangle (x0, y0)-(x1, y1) of viewport (vx, vy, vw, vh) of fbo, blended as
// straight alpha, premultiplied alpha, or inverting what is below where the (premultiplied) texture
// is white; or opaque. Rows are taken top-down: pass y0 > y1 for a framebuffer texture (bottom-up).
// The caller has saved GL state.
bool OverlayDraw(GLuint fbo, int vx, int vy, int vw, int vh, bool compat, GLuint tex, float x0, float y0, float x1,
                 float y1, OverlayBlend blend)
{
    if (g.failed) return false;
    if (!g.prog && !BuildProgram()) {
        Log("overlay unavailable");
        g.failed = true;
        return false;
    }
    R.BindFramebuffer(GL_DRAW_FRAMEBUFFER, fbo);
    R.Viewport(vx, vy, vw, vh);
    for (GLenum cap : {GL_DEPTH_TEST, GL_STENCIL_TEST, GL_CULL_FACE, GL_SCISSOR_TEST, GL_FRAMEBUFFER_SRGB})
        R.Disable(cap);
    if (compat) {
        R.Disable(GL_ALPHA_TEST);
        R.Disable(GL_COLOR_LOGIC_OP);
        R.PolygonMode(GL_FRONT_AND_BACK, GL_FILL);
    }
    R.ColorMask(1, 1, 1, 1);
    if (blend == kBlendOpaque) R.Disable(GL_BLEND);
    else R.Enable(GL_BLEND);
    if (R.BlendEquation) R.BlendEquation(GL_FUNC_ADD);
    switch (blend) {
    case kBlendStraight: R.BlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA); break;
    case kBlendPremultiplied: R.BlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA); break;
    case kBlendInvert: R.BlendFunc(GL_ONE_MINUS_DST_COLOR, GL_ONE_MINUS_SRC_ALPHA); break;
    case kBlendOpaque: break;
    }
    R.UseProgram(g.prog);
    R.Uniform4f(g.locRect, x0, y0, x1, y1);
    R.Uniform1i(g.locTex, 0);
    R.ActiveTexture(GL_TEXTURE0);
    R.BindTexture(GL_TEXTURE_2D, tex);
    if (R.BindSampler) R.BindSampler(0, 0);
    R.BindVertexArray(g.vao);
    R.DrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    return true;
}

void OsdMessage(const std::wstring& text, double seconds)
{
    std::lock_guard<std::mutex> lk(g.lock);
    g.message = text;
    g.messageUntil = GetTickCount64() + (ULONGLONG)(seconds * 1000.0);
}

void OsdStatus(const std::wstring& text)
{
    std::lock_guard<std::mutex> lk(g.lock);
    g.status = text;
}

bool OsdActive()
{
    return !CurrentText().empty();
}

// Draws the overlay into viewport (vx, vy, vw, vh) of fbo, laid out for an eye image of W x H
// (a half-width SBS viewport squeezes it like the eye images). The caller has saved GL state.
void OsdRender(GLuint fbo, int vx, int vy, int vw, int vh, int W, int H, bool compat)
{
    if (g.failed) return;
    std::wstring text = CurrentText();
    if (text.empty()) return;
    if (!g.prog && !BuildProgram()) {
        Log("overlay unavailable");
        g.failed = true;
        return;
    }
    const int fontPx = std::max(16, H / 40);
    if (text != g.rendered || fontPx != g.renderedFont || !g.tex) {
        std::vector<uint8_t> rgba;
        int w = 0, h = 0;
        if (!Rasterize(text, fontPx, rgba, w, h)) return;
        OverlayUpload(g.tex, w, h, rgba.data());
        g.texW = w;
        g.texH = h;
        g.rendered = text;
        g.renderedFont = fontPx;
    }

    const float x0 = 0.5f * (W - g.texW), yTop = 0.05f * H;   // top centre: clear of most HUD corners
    const float nx0 = -1.0f + 2.0f * x0 / W, nx1 = -1.0f + 2.0f * (x0 + g.texW) / W;
    const float ny1 = 1.0f - 2.0f * yTop / H, ny0 = 1.0f - 2.0f * (yTop + g.texH) / H;
    OverlayDraw(fbo, vx, vy, vw, vh, compat, g.tex, nx0, ny0, nx1, ny1, kBlendStraight);
}

// Vertex shader rewrite: the game's main() becomes s3d_main0() and a new main() calls it, then
// offsets gl_Position.x per eye. No per-game injection point is needed, and early returns in the
// original main are harmless.
#include "common.h"
#include <regex>

// Perspective geometry (w != 1) is offset by its view depth w. Flat geometry (w == 1: 2D, HUD,
// full-screen passes) stays at screen depth, unless ortho depth is on: then its clip z (the layer
// depth of orthographic 2D/2.5D games) drives the offset.
static const char kStereoMain[] = R"(
// --- OGL3D stereo ---
uniform vec4 s3d_P; // x = eye (-1 left, +1 right, 0 mono), y = separation, z = convergence, w = keep w==1 mono
uniform vec4 s3d_Q; // x = ortho depth scale (0 = off), y = clip z placed at the screen
void main()
{
    s3d_main0();
    if (s3d_P.x != 0.0) {
        if (abs(gl_Position.w - 1.0) > 0.00001)
            gl_Position.x += s3d_P.x * s3d_P.y * (gl_Position.w - s3d_P.z);
        else if (s3d_Q.x != 0.0)
            gl_Position.x += s3d_P.x * s3d_P.y * s3d_Q.x * (gl_Position.z - s3d_Q.y);
        else if (s3d_P.w < 0.5)
            gl_Position.x += s3d_P.x * s3d_P.y * (gl_Position.w - s3d_P.z);
    }
}
)";

// Same length as src, with comments replaced by spaces so offsets still line up.
static std::string BlankComments(const std::string& src)
{
    std::string s = src;
    for (size_t i = 0; i + 1 < s.size(); ++i) {
        if (s[i] == '/' && s[i + 1] == '/') {
            while (i < s.size() && s[i] != '\n') s[i++] = ' ';
        } else if (s[i] == '/' && s[i + 1] == '*') {
            size_t end = s.find("*/", i + 2);
            end = end == std::string::npos ? s.size() : end + 2;
            for (; i < end; ++i)
                if (s[i] != '\n') s[i] = ' ';
            --i;
        }
    }
    return s;
}

bool InjectVertexShader(const std::string& src, std::string& out)
{
    if (src.find("s3d_main0") != std::string::npos) return false;   // already ours
    std::string code = BlankComments(src);
    static const std::regex mainRe(R"(\bvoid\s+main\s*\()");
    std::vector<size_t> hits;   // offset of the "main" token
    for (auto it = std::sregex_iterator(code.begin(), code.end(), mainRe); it != std::sregex_iterator(); ++it) {
        size_t at = (size_t)it->position(0);
        hits.push_back(code.find("main", at));
    }
    if (hits.empty()) return false;
    out.clear();
    out.reserve(src.size() + sizeof(kStereoMain) + 32);
    size_t prev = 0;
    for (size_t h : hits) {
        out.append(src, prev, h - prev);
        out += "s3d_main0";
        prev = h + 4;
    }
    out.append(src, prev, std::string::npos);
    out += kStereoMain;
    return true;
}

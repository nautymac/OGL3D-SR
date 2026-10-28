// Vertex shader rewrite: the game's main() becomes s3d_main0() and a new main() calls it, then
// offsets gl_Position.x per eye. No per-game injection point is needed, and early returns in the
// original main are harmless.
#include "common.h"
#include <algorithm>
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

// ARB assembly vertex programs (!!ARBvp1.0, also NV's !!NVvp4.0/5.0; Cg's arbvp1 profile, PSSG games
// such as Rocketbirds): the program writes its position into a temporary instead of result.position,
// and a tail offsets x per eye from program.env[envIndex] before writing result.position:
//   x = eye * separation, y = convergence, z = offset per w for flat (w == 1) geometry
// Perspective geometry moves by x * (w - y) like the GLSL rewrite; flat geometry by z * w (the host
// puts the draw's flat depth there, or 0 to keep it at the screen).
bool InjectArbVertexProgram(const std::string& src, int envIndex, std::string& out)
{
    if (src.compare(0, 10, "!!ARBvp1.0") != 0 && src.compare(0, 6, "!!NVvp") != 0) return false;
    if (src.find("s3d_pos") != std::string::npos) return false;   // already ours
    // position computed by the fixed-function pipeline: result.position must not be written
    if (std::regex_search(src, std::regex(R"(OPTION\s+ARB_position_invariant)"))) return false;
    if (src.find("result.position") == std::string::npos) return false;
    // declarations go after the header line and the OPTION lines (they have to come first)
    size_t at = src.find('\n');
    if (at == std::string::npos) return false;
    ++at;
    static const std::regex optionRe(R"(^\s*(OPTION\b[^;]*;|#[^\n]*)?\s*$)");
    while (at < src.size()) {
        size_t eol = src.find('\n', at);
        std::string line = src.substr(at, eol == std::string::npos ? std::string::npos : eol - at);
        if (!std::regex_match(line, optionRe)) break;
        at = eol == std::string::npos ? src.size() : eol + 1;
    }
    size_t end = src.rfind("END");
    if (end == std::string::npos || end < at) return false;
    std::string body = src.substr(at, end - at);
    // OUTPUT oPos = result.position;  ->  ALIAS oPos = s3d_pos;
    body = std::regex_replace(body, std::regex(R"(OUTPUT\s+(\w+)\s*=\s*result\.position\s*;)"), "ALIAS $1 = s3d_pos;");
    body = std::regex_replace(body, std::regex(R"(result\.position\b)"), "s3d_pos");
    const std::string env = "program.env[" + std::to_string(envIndex) + "]";
    out = src.substr(0, at) +
          "PARAM s3d_E = " + env + ";\nPARAM s3d_c = { 1, 0.00001, 0, 0 };\nTEMP s3d_pos, s3d_t;\n" + body +
          "# --- OGL3D stereo ---\n"
          "ADD s3d_t.x, s3d_pos.w, -s3d_E.y;\n"      // w - convergence
          "MUL s3d_t.x, s3d_t.x, s3d_E.x;\n"         // perspective offset
          "MUL s3d_t.y, s3d_pos.w, s3d_E.z;\n"       // flat offset
          "ADD s3d_t.z, s3d_pos.w, -s3d_c.x;\n"
          "ABS s3d_t.z, s3d_t.z;\n"
          "SLT s3d_t.z, s3d_t.z, s3d_c.y;\n"         // 1 when w == 1 (flat geometry)
          "ADD s3d_t.y, s3d_t.y, -s3d_t.x;\n"
          "MAD s3d_t.x, s3d_t.z, s3d_t.y, s3d_t.x;\n"
          "ADD s3d_pos.x, s3d_pos.x, s3d_t.x;\n"
          "MOV result.position, s3d_pos;\n" +
          src.substr(end);
    return true;
}

// Silhouette depth ([Flat] silhouette): a picture drawn as one image (FRAMED's comic panels are
// videos) gets a second depth inside it. The fragment shader samples the picture through a
// coordinate shifted per eye wherever the shifted or the unshifted pixel is dark, so dark figures
// stand in front of the picture, and the place they left shows the picture next to them. The varying
// <uv> keeps its name (it links to the previous stage); after its declaration the game's code reads
// the global s3d_sil_uv instead, which our main() fills before it calls the game's (now s3d_main1),
// so every texture read with <uv> moves the same way.
bool InjectSilhouette(const std::string& src, const std::string& uv, const std::string& tex, std::string& out,
                      std::string& why)
{
    static const std::regex identRe(R"(^[A-Za-z_]\w*$)");
    if (!std::regex_match(uv, identRe) || !std::regex_match(tex, identRe)) { why = "bad silhouette_uv/silhouette_tex name"; return false; }
    if (src.find("s3d_main1") != std::string::npos) { why = "already rewritten"; return false; }
    const std::string code = BlankComments(src);
    std::smatch m;
    int version = 110;
    static const std::regex verRe(R"(#[ \t]*version[ \t]+(\d+))");
    if (std::regex_search(code, m, verRe)) version = std::stoi(m[1].str());
    if (!std::regex_search(code, std::regex("\\b" + tex + "\\b"))) { why = "no sampler " + tex; return false; }
    // the declaration: only its name changes, so the match may start early (at "#define varying in")
    if (!std::regex_search(code, m, std::regex("\\b(varying|in)\\b[^;{}]*?\\b(\\w+)\\s+" + uv + "\\s*;"))) {
        why = "no varying " + uv;
        return false;
    }
    if (m[2].str() != "vec2") { why = uv + " is " + m[2].str() + ", not vec2"; return false; }
    const size_t declEnd = (size_t)(m.position(0) + m.length(0));
    static const std::regex mainRe(R"(\bvoid\s+main\s*\()");
    std::vector<size_t> mains;
    for (auto it = std::sregex_iterator(code.begin(), code.end(), mainRe); it != std::sregex_iterator(); ++it)
        mains.push_back(code.find("main", (size_t)it->position(0)));
    if (mains.size() != 1 || mains[0] < declEnd) { why = "main() not found after the declaration"; return false; }
    std::string body = src.substr(declEnd, mains[0] - declEnd) + "s3d_main1" + src.substr(mains[0] + 4);
    body = std::regex_replace(body, std::regex("\\b" + uv + "\\b"), "s3d_sil_uv");
    const char* fetch = version >= 130 ? "texture" : "texture2D";
    out = src.substr(0, declEnd) + " vec2 s3d_sil_uv;" + body;
    out += "\n// --- OGL3D silhouette depth ---\n"
           "uniform vec4 s3d_S; // x = shift of dark pixels for this eye (target pixels), y = darkness threshold\n"
           "float s3d_lum(vec2 s3d_c) { vec3 s3d_t = " + std::string(fetch) + "(" + tex + ", s3d_c).rgb; return max(s3d_t.r, max(s3d_t.g, s3d_t.b)); }\n"
           "void main()\n"
           "{\n"
           "    vec2 s3d_uv = " + uv + ";\n"
           "    if (s3d_S.x != 0.0) {\n"
           "        vec2 s3d_moved = s3d_uv - dFdx(" + uv + ") * s3d_S.x;\n"
           "        if (min(s3d_lum(s3d_moved), s3d_lum(s3d_uv)) < s3d_S.y) s3d_uv = s3d_moved;\n"
           "    }\n"
           "    s3d_sil_uv = s3d_uv;\n"
           "    s3d_main1();\n"
           "}\n";
    return true;
}

// ---------------------------------------------------------------------------------------------
// Flat depth ([Flat]): a geometry shader behind the game's vertex shader sees whole triangles, so
// every 2D sprite knows its own screen rectangle (any 3 corners of a quad span it) without a CPU
// read-back. The vertex shader's outputs are renamed s3d_v_<name>; the geometry shader passes them
// on under their original names, so the fragment shader links unchanged.

enum : unsigned {
    kBiFrontColor = 1, kBiBackColor = 2, kBiFrontSecondary = 4, kBiBackSecondary = 8, kBiFog = 16,
    kBiClipVertex = 32, kBiPointSize = 64,
};

// The vertex shader's name for an output the geometry shader passes on. GLSL reserves names with
// "__", so an output that starts with "_" (LÖVE's shaders) gets "s3d_vx" instead of "s3d_v_".
static std::string RenamedOutput(const std::string& name)
{
    return (!name.empty() && name[0] == '_' ? "s3d_vx" : "s3d_v_") + name;
}

static int MaxLiteralIndex(const std::string& code, const char* name, bool& dynamic)
{
    int best = -1;
    std::regex re(std::string(R"(\b)") + name + R"(\s*\[\s*([^\]]*)\])");
    for (auto it = std::sregex_iterator(code.begin(), code.end(), re); it != std::sregex_iterator(); ++it) {
        std::string idx = (*it)[1].str();
        if (!idx.empty() && std::all_of(idx.begin(), idx.end(), [](char ch) { return ch >= '0' && ch <= '9'; }))
            best = std::max(best, atoi(idx.c_str()));
        else
            dynamic = true;
    }
    return best;
}

bool AnalyzeVertexShader(const std::string& injected, VsInfo& info, std::string& why)
{
    info = VsInfo{};
    std::string code = BlankComments(injected);
    // preprocessor lines would glue onto the next statement
    std::vector<std::pair<std::string, std::string>> defines;   // #define name, body
    for (size_t i = 0; i < code.size(); ++i) {
        while (i < code.size() && (code[i] == ' ' || code[i] == '\t')) ++i;
        bool pp = i < code.size() && code[i] == '#';
        if (pp && code.compare(i, 8, "#version") == 0) {
            info.version = atoi(code.c_str() + i + 8);
            info.core = code.find("core", i) < code.find('\n', i);
        }
        if (pp) {
            static const std::regex defineRe(R"(^#\s*define\s+(\w+)\s+(.*)$)");
            std::string line = code.substr(i, code.find('\n', i) - i);
            std::smatch m;
            if (std::regex_match(line, m, defineRe)) defines.push_back({m[1].str(), m[2].str()});
        }
        while (i < code.size() && code[i] != '\n') {
            if (pp) code[i] = ' ';
            ++i;
        }
    }
    // output interface blocks: "out Name { members } instance;" (matched by block name, so the
    // geometry shader declares the same block for its input and output; nothing is renamed)
    static const std::regex blockRe(R"(((?:layout\s*\([^)]*\)\s*)?)\bout\s+(\w+)\s*\{([^}]*)\}\s*(\w*)\s*(\[[^\]]*\])?\s*;)");
    static const std::regex memberRe(R"(^\s*((?:(?:flat|smooth|noperspective|centroid|invariant|sample|highp|mediump|lowp)\s+)*\w+)\s+(\w+)\s*(\[\s*\d+\s*\])?\s*$)");
    for (auto it = std::sregex_iterator(code.begin(), code.end(), blockRe); it != std::sregex_iterator(); ++it) {
        const std::smatch& m = *it;
        if (m[4].str().empty() || m[5].matched) {
            why = "output block without instance name or arrayed";
            return false;
        }
        VsBlock b{m[1].str(), m[2].str(), m[4].str(), m[3].str(), {}};
        std::string body = m[3].str();
        size_t p = 0;
        while (p < body.size()) {
            size_t q = body.find(';', p);
            if (q == std::string::npos) break;
            std::smatch mm;
            std::string one = body.substr(p, q - p);
            if (!std::regex_match(one, mm, memberRe)) {
                why = "unparsed block member";
                return false;
            }
            b.members.push_back(mm[2].str());
            p = q + 1;
        }
        info.blocks.push_back(b);
    }
    // blank the blocks so the statement scan below does not see their members
    for (auto it = std::sregex_iterator(code.begin(), code.end(), blockRe); it != std::sregex_iterator(); ++it)
        for (size_t k = (size_t)it->position(0); k < (size_t)(it->position(0) + it->length(0)); ++k)
            if (code[k] != '\n') code[k] = ' ';
    // global declarations: statements at brace and paren depth 0
    static const std::regex declRe(
        R"(^\s*((?:layout\s*\([^)]*\)\s*)?(?:(?:flat|smooth|noperspective|centroid|invariant|sample)\s+)*)(varying|out)\s+((?:(?:highp|mediump|lowp)\s+)?\w+)\s+([\w\s\[\],]+)$)");
    static const std::regex nameRe(R"(^\s*(\w+)\s*(\[\s*\d+\s*\])?\s*$)");
    int brace = 0, paren = 0;
    size_t start = 0;
    for (size_t i = 0; i < code.size(); ++i) {
        char ch = code[i];
        if (ch == '{') { ++brace; start = i + 1; }
        else if (ch == '}') { --brace; start = i + 1; }
        else if (ch == '(') ++paren;
        else if (ch == ')') --paren;
        else if (ch == ';') {
            if (brace == 0 && paren == 0) {
                std::string st = code.substr(start, i - start);
                std::smatch m;
                if (std::regex_match(st, m, declRe)) {
                    std::string quals = m[1].str(), type = m[3].str(), list = m[4].str();
                    size_t p = 0;
                    while (p <= list.size()) {
                        size_t q = list.find(',', p);
                        if (q == std::string::npos) q = list.size();
                        std::string one = list.substr(p, q - p);
                        std::smatch nm;
                        if (!std::regex_match(one, nm, nameRe)) {
                            why = "unparsed output declaration";
                            return false;
                        }
                        info.outs.push_back({quals, type, nm[1].str(), nm[2].matched ? nm[2].str() : ""});
                        p = q + 1;
                    }
                }
            }
            start = i + 1;
        }
    }
    // built-in outputs written through a macro (MojoShader, FNA: "#define vs_oT0 gl_TexCoord[0]"): the
    // bodies of the macros the code uses count as code
    std::string scan = code;
    for (const auto& d : defines)
        if (std::regex_search(code, std::regex(R"(\b)" + d.first + R"(\b)"))) scan += "\n" + d.second;
    auto uses = [&](const char* name) { return std::regex_search(scan, std::regex(std::string(R"(\b)") + name + R"(\b)")); };
    if (uses("gl_FrontColor")) info.builtins |= kBiFrontColor;
    if (uses("gl_BackColor")) info.builtins |= kBiBackColor;
    if (uses("gl_FrontSecondaryColor")) info.builtins |= kBiFrontSecondary;
    if (uses("gl_BackSecondaryColor")) info.builtins |= kBiBackSecondary;
    if (uses("gl_FogFragCoord")) info.builtins |= kBiFog;
    if (uses("gl_ClipVertex")) info.builtins |= kBiClipVertex;
    if (uses("gl_PointSize")) info.builtins |= kBiPointSize;
    bool dyn = false;
    info.texCoords = MaxLiteralIndex(scan, "gl_TexCoord", dyn) + 1;
    if (dyn) info.texCoords = 8;
    dyn = false;
    info.clipDistances = MaxLiteralIndex(scan, "gl_ClipDistance", dyn) + 1;
    if (dyn) info.clipDistances = 8;
    if (info.core && (info.builtins & ~kBiPointSize)) info.core = false;

    // rename the outputs everywhere (not after a '.', so struct members stay)
    info.renamed = injected;
    for (const VsOutput& o : info.outs) {
        std::regex re("(^|[^.\\w])" + o.name + "\\b");
        info.renamed = std::regex_replace(info.renamed, re, "$1" + RenamedOutput(o.name));
    }
    return true;
}

std::string BuildFlatGs(const VsInfo& vs, bool compatContext)
{
    int ver = std::max(150, vs.version);
    std::string s = "#version " + std::to_string(ver);
    if (compatContext && !vs.core) s += " compatibility";
    bool arrays = false;
    for (const VsOutput& o : vs.outs) arrays |= !o.arr.empty();
    if (arrays && ver < 430) s += "\n#extension GL_ARB_arrays_of_arrays : require";
    s += R"(
// --- OGL3D flat depth ---
layout(triangles) in;
layout(triangle_strip, max_vertices = 3) out;
uniform vec4 s3d_P; // x = eye, y = separation
uniform vec4 s3d_F; // x = 1 feet rule, 2 order rule, y = depth at the top of the screen, z = plane width (NDC), w = depth on the screen plane
uniform vec4 s3d_D; // x = 1: this draw stays at screen depth, 2: a lit sprite (at the screen only over the whole screen); feet rule: y = nearest sprite depth, z = plane height (NDC), w = height that makes a plane whatever the width (0 = off); order rule: y = depth slot of the group, z = primitives in the draw, w = group depth
uniform vec4 s3d_X; // x = pixels of the target per NDC unit to round the shift to ([Flat] snap_pixels; 0 = off)
)";
    for (const VsOutput& o : vs.outs) {
        std::string q = o.quals;
        s += q + "in " + o.type + " " + RenamedOutput(o.name) + "[]" + o.arr + ";\n";
        s += q + "out " + o.type + " " + o.name + o.arr + ";\n";
    }
    for (size_t k = 0; k < vs.blocks.size(); ++k) {
        const VsBlock& b = vs.blocks[k];
        s += b.layout + "in " + b.name + " {" + b.body + "} s3d_b" + std::to_string(k) + "[];\n";
        s += b.layout + "out " + b.name + " {" + b.body + "} " + b.inst + ";\n";
    }
    s += R"(
float s3d_t(float y) { return (y + 1.0) * 0.5 * s3d_F.y - s3d_F.w; }
void main()
{
    vec4 s3d_p0 = gl_in[0].gl_Position, s3d_p1 = gl_in[1].gl_Position, s3d_p2 = gl_in[2].gl_Position;
    vec3 s3d_sh = vec3(0.0);
    if (s3d_P.x != 0.0 && s3d_F.x != 0.0 && abs(s3d_D.x - 1.0) > 0.5 &&
        abs(s3d_p0.w - 1.0) < 0.00001 && abs(s3d_p1.w - 1.0) < 0.00001 && abs(s3d_p2.w - 1.0) < 0.00001) {
        vec2 s3d_lo = min(min(s3d_p0.xy, s3d_p1.xy), s3d_p2.xy), s3d_hi = max(max(s3d_p0.xy, s3d_p1.xy), s3d_p2.xy);
        float s3d_k = s3d_P.x * s3d_P.y;
        vec3 s3d_d;
        if (s3d_F.x > 1.5) {
            // rule order: the depth of its layer group, spread over the group's depth slot in drawing
            // order (a batch is drawn back to front, so its first sprite is the farthest). Sprites are
            // quads of two triangles that must move together, or a full-screen quad shows its diagonal.
            float s3d_n = floor(s3d_D.z * 0.5);
            float s3d_q = s3d_n > 1.5 ? clamp(floor(float(gl_PrimitiveIDIn) * 0.5) / (s3d_n - 1.0), 0.0, 1.0) : 1.0;
            s3d_d = vec3(s3d_D.w + s3d_D.y * (1.0 - s3d_q));
        } else if ((s3d_hi.x - s3d_lo.x >= s3d_F.z && s3d_hi.y - s3d_lo.y >= s3d_D.z) ||   // wide (and high): layer / overlay plane,
                   (s3d_D.w > 0.0 && s3d_hi.y - s3d_lo.y >= s3d_D.w))                    // or a tall piece of a picture
            // slopes back with the screen height; vertices far off screen (Door Kickers draws overlays
            // with corners thousands of units away) are clamped, or the slope runs away
            s3d_d = vec3(s3d_t(clamp(s3d_p0.y, -1.0, 1.0)), s3d_t(clamp(s3d_p1.y, -1.0, 1.0)), s3d_t(clamp(s3d_p2.y, -1.0, 1.0)));
        else
            s3d_d = vec3(max(s3d_t(s3d_lo.y), s3d_D.y));                        // sprite: stands at its bottom edge
        s3d_sh = s3d_k * s3d_d;
        // x = 2: a lit sprite (it reads the screen's light texture) - but a primitive over the whole
        // screen is a post-processing pass, which stays at the screen
        if (s3d_D.x > 1.5 && all(lessThanEqual(s3d_lo, vec2(-0.99))) && all(greaterThanEqual(s3d_hi, vec2(0.99))))
            s3d_sh = vec3(0.0);
        // snap_pixels: whole pixels of the target, so both eyes round a sprite at a fractional position the
        // same way (in a low-resolution target its disparity otherwise flips by a pixel while it scrolls)
        if (s3d_X.x > 0.0)
            s3d_sh = round(s3d_sh * s3d_X.x) / s3d_X.x;
    }
    for (int s3d_i = 0; s3d_i < 3; ++s3d_i) {
        gl_Position = gl_in[s3d_i].gl_Position;
        gl_Position.x += s3d_sh[s3d_i] * gl_Position.w;
)";
    for (const VsOutput& o : vs.outs) s += "        " + o.name + " = " + RenamedOutput(o.name) + "[s3d_i];\n";
    for (size_t k = 0; k < vs.blocks.size(); ++k)
        for (const std::string& m : vs.blocks[k].members)
            s += "        " + vs.blocks[k].inst + "." + m + " = s3d_b" + std::to_string(k) + "[s3d_i]." + m + ";\n";
    struct { unsigned bit; const char* name; } bi[] = {
        {kBiFrontColor, "gl_FrontColor"}, {kBiBackColor, "gl_BackColor"}, {kBiFrontSecondary, "gl_FrontSecondaryColor"},
        {kBiBackSecondary, "gl_BackSecondaryColor"}, {kBiFog, "gl_FogFragCoord"}, {kBiClipVertex, "gl_ClipVertex"},
        {kBiPointSize, "gl_PointSize"}};
    for (auto& b : bi)
        if (vs.builtins & b.bit) s += std::string("        ") + b.name + " = gl_in[s3d_i]." + b.name + ";\n";
    for (int t = 0; t < vs.texCoords; ++t)
        s += "        gl_TexCoord[" + std::to_string(t) + "] = gl_in[s3d_i].gl_TexCoord[" + std::to_string(t) + "];\n";
    for (int t = 0; t < vs.clipDistances; ++t)
        s += "        gl_ClipDistance[" + std::to_string(t) + "] = gl_in[s3d_i].gl_ClipDistance[" + std::to_string(t) + "];\n";
    s += R"(        EmitVertex();
    }
    EndPrimitive();
}
)";
    return s;
}

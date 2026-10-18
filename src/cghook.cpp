// NVIDIA Cg games ([Inject] cg_glsl): cgGLGetLatestProfile picks the newest profile the GPU has, which
// on NVIDIA hardware is an assembly program profile (gp5vp/gp5fp, vp40/fp40, arbvp1/arbfp1). The
// wrapper only rewrites GLSL, so draws of such programs get no stereo offset or flat depth. The
// game's own modules get their cgGL.dll import of cgGLGetLatestProfile redirected to answer with the
// GLSL profiles (glslv, glslf, glslg): Cg then compiles every program to GLSL, which goes through
// glShaderSource like any other shader (Rocketbirds: Hardboiled Chicken, PSSG engine).
#include "common.h"
#include <psapi.h>
#include <iterator>

namespace {

using GetLatestProfileFn = int(__cdecl*)(int);   // CGprofile cgGLGetLatestProfile(CGGLenum)
using GetProfileFn = int(__cdecl*)(const char*); // CGprofile cgGetProfile(const char*)
using IsProfileSupportedFn = int(__cdecl*)(int); // CGbool cgGLIsProfileSupported(CGprofile)

GetLatestProfileFn g_realLatest;
GetProfileFn g_getProfile;
IsProfileSupportedFn g_isSupported;
// CGprogram cgCreateProgram(CGcontext, CGenum type, const char* src, CGprofile, const char* entry, const char** args)
using CreateProgramFn = void*(__cdecl*)(void*, int, const char*, int, const char*, const char**);
using GetLastListingFn = const char*(__cdecl*)(void*);
using GetProfileStringFn = const char*(__cdecl*)(int);
CreateProgramFn g_realCreate;
GetLastListingFn g_lastListing;
GetProfileStringFn g_profileString;

// Logs how Cg compiled each program (profile, options, failures with the compiler's listing).
void* __cdecl HookCreateProgram(void* ctx, int type, const char* src, int profile, const char* entry, const char** args)
{
    void* p = g_realCreate(ctx, type, src, profile, entry, args);
    static int logged;
    std::string opts;
    for (int i = 0; args && args[i] && i < 16; ++i) opts += std::string(" ") + args[i];
    const char* listing = g_lastListing ? g_lastListing(ctx) : nullptr;
    if (!p || logged < 6) {
        ++logged;
        Log("Cg: cgCreateProgram(%s, entry %s, args%s) -> %p%s%s", g_profileString ? g_profileString(profile) : "?", entry ? entry : "-",
            opts.c_str(), p, listing && *listing ? "\n" : "", listing ? listing : "");
    }
    return p;
}
const int kCgGlVertex = 8, kCgGlFragment = 9, kCgGlGeometry = 10;   // CGGLenum

int __cdecl HookGetLatestProfile(int type)
{
    const char* name = type == kCgGlVertex ? "glslv" : type == kCgGlFragment ? "glslf" : type == kCgGlGeometry ? "glslg" : nullptr;
    int real = g_realLatest ? g_realLatest(type) : 0;
    int glsl = name && g_getProfile ? g_getProfile(name) : 0;
    if (glsl && (!g_isSupported || g_isSupported(glsl))) {
        static int logged;
        if (logged++ < 3) Log("Cg: profile for %s is %s instead of %d (GLSL goes through the shader rewrite)",
                              type == kCgGlVertex ? "vertex" : type == kCgGlFragment ? "fragment" : "geometry", name, real);
        return glsl;
    }
    return real;
}

}   // namespace

// Redirects module m's import dll!name to hook (dll compared without case; "*kernel32*"-style
// matching is up to the caller passing each name); true when patched.
bool PatchModuleImport(HMODULE m, const char* dll, const char* name, void* hook)
{
    auto base = (uint8_t*)m;
    auto dos = (IMAGE_DOS_HEADER*)base;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return false;
    auto nt = (IMAGE_NT_HEADERS*)(base + dos->e_lfanew);
    const IMAGE_DATA_DIRECTORY& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dir.VirtualAddress) return false;
    bool patched = false;
    for (auto imp = (IMAGE_IMPORT_DESCRIPTOR*)(base + dir.VirtualAddress); imp->Name; ++imp) {
        if (_stricmp((const char*)(base + imp->Name), dll) != 0 || !imp->OriginalFirstThunk) continue;
        auto names = (IMAGE_THUNK_DATA*)(base + imp->OriginalFirstThunk);
        auto slots = (IMAGE_THUNK_DATA*)(base + imp->FirstThunk);
        for (; names->u1.AddressOfData; ++names, ++slots) {
            if (IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal)) continue;
            auto byName = (IMAGE_IMPORT_BY_NAME*)(base + names->u1.AddressOfData);
            if (strcmp((const char*)byName->Name, name) != 0) continue;
            DWORD old;
            if (VirtualProtect(&slots->u1.Function, sizeof(void*), PAGE_READWRITE, &old)) {
                slots->u1.Function = (ULONG_PTR)hook;
                VirtualProtect(&slots->u1.Function, sizeof(void*), old, &old);
                patched = true;
            }
        }
    }
    return patched;
}

namespace {

bool PatchCgImport(HMODULE m)
{
    bool a = PatchModuleImport(m, "cgGL.dll", "cgGLGetLatestProfile", (void*)&HookGetLatestProfile);
    if (g_realCreate) PatchModuleImport(m, "cg.dll", "cgCreateProgram", (void*)&HookCreateProgram);
    return a;
}

}   // namespace

// [Inject] add_args: the game's modules (and the C runtime, which builds argv) get GetCommandLineW/A answering
// with the arguments appended. Runs in DllMain, before the exe's startup code reads its command line, so a
// switch the store does not pass (Machinarium's -ogl) still reaches the game.
namespace {
std::vector<wchar_t> g_cmdW;
std::vector<char> g_cmdA;
LPWSTR WINAPI HookGetCommandLineW() { return g_cmdW.data(); }
LPSTR WINAPI HookGetCommandLineA() { return g_cmdA.data(); }
}   // namespace

void InstallCommandLineArgs()
{
    const std::wstring w = std::wstring(GetCommandLineW()) + L" " + g_cfg.addArgs;
    std::string a = GetCommandLineA();
    a += ' ';
    for (wchar_t ch : g_cfg.addArgs) a += (char)(ch < 128 ? ch : '?');
    g_cmdW.assign(w.begin(), w.end());
    g_cmdW.push_back(0);
    g_cmdA.assign(a.begin(), a.end());
    g_cmdA.push_back(0);
    HMODULE self = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       (LPCWSTR)&InstallCommandLineArgs, &self);
    static const char* kDlls[] = {"kernel32.dll", "kernelbase.dll", "api-ms-win-core-processenvironment-l1-1-0.dll",
                                  "api-ms-win-core-processenvironment-l1-2-0.dll"};
    HMODULE mods[512];
    DWORD bytes = 0;
    int n = 0;
    if (K32EnumProcessModules(GetCurrentProcess(), mods, sizeof(mods), &bytes))
        for (DWORD i = 0; i < bytes / sizeof(HMODULE); ++i) {
            if (mods[i] == self) continue;
            bool any = false;
            for (const char* dll : kDlls) {
                any |= PatchModuleImport(mods[i], dll, "GetCommandLineW", (void*)&HookGetCommandLineW);
                any |= PatchModuleImport(mods[i], dll, "GetCommandLineA", (void*)&HookGetCommandLineA);
            }
            n += any;
        }
    Log("command line: \"%ls\" appended (GetCommandLine redirected in %d modules)", g_cfg.addArgs.c_str(), n);
}

// Called at load (DllMain): patches the exe and the DLLs of the game's folder that import cgGL.dll.
void InstallCgGlslHooks()
{
    HMODULE cgGL = GetModuleHandleW(L"cgGL.dll"), cg = GetModuleHandleW(L"cg.dll");
    if (!cgGL || !cg) return;   // not a Cg game (or Cg loaded later: not handled)
    g_realLatest = (GetLatestProfileFn)GetProcAddress(cgGL, "cgGLGetLatestProfile");
    g_isSupported = (IsProfileSupportedFn)GetProcAddress(cgGL, "cgGLIsProfileSupported");
    g_getProfile = (GetProfileFn)GetProcAddress(cg, "cgGetProfile");
    g_realCreate = (CreateProgramFn)GetProcAddress(cg, "cgCreateProgram");
    g_lastListing = (GetLastListingFn)GetProcAddress(cg, "cgGetLastListing");
    g_profileString = (GetProfileStringFn)GetProcAddress(cg, "cgGetProfileString");
    if (!g_realLatest || !g_getProfile) {
        Log("Cg: cgGLGetLatestProfile/cgGetProfile not found - Cg programs stay on their assembly profiles");
        return;
    }
    wchar_t gameDir[MAX_PATH];
    GetModuleFileNameW(nullptr, gameDir, MAX_PATH);
    if (wchar_t* s = wcsrchr(gameDir, L'\\')) s[1] = 0;
    const size_t dirLen = wcslen(gameDir);
    HMODULE mods[512];
    DWORD bytes = 0;
    int n = 0;
    if (K32EnumProcessModules(GetCurrentProcess(), mods, sizeof(mods), &bytes))
        for (DWORD i = 0; i < bytes / sizeof(HMODULE); ++i) {
            wchar_t path[MAX_PATH];
            GetModuleFileNameW(mods[i], path, MAX_PATH);
            if (mods[i] == cgGL || mods[i] == cg || _wcsnicmp(path, gameDir, dirLen) != 0) continue;
            if (PatchCgImport(mods[i])) ++n;
        }
    Log("Cg: cgGLGetLatestProfile redirected to the GLSL profiles in %d module(s)", n);
}

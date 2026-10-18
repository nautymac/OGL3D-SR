// Minimal OpenGL declarations for the wrapper (no <GL/gl.h>: our exports share its names).
#pragma once
#include <windows.h>
#include <cstddef>
#include <cstdint>

#define GLAPIENTRY __stdcall

typedef unsigned int GLenum;
typedef unsigned int GLbitfield;
typedef unsigned int GLuint;
typedef int GLint;
typedef int GLsizei;
typedef unsigned char GLboolean;
typedef signed char GLbyte;
typedef short GLshort;
typedef unsigned char GLubyte;
typedef unsigned short GLushort;
typedef float GLfloat;
typedef float GLclampf;
typedef double GLdouble;
typedef double GLclampd;
typedef void GLvoid;
typedef char GLchar;
typedef ptrdiff_t GLsizeiptr;
typedef ptrdiff_t GLintptr;
typedef struct __GLsync* GLsync;
typedef uint64_t GLuint64;

enum : GLenum {
    GL_NO_ERROR = 0,
    GL_NONE = 0,
    GL_DEPTH_BUFFER_BIT = 0x0100,
    GL_STENCIL_BUFFER_BIT = 0x0400,
    GL_COLOR_BUFFER_BIT = 0x4000,
    GL_ALL_ATTRIB_BITS = 0x000FFFFF,
    GL_COMPILE = 0x1300,
    GL_COMPILE_AND_EXECUTE = 0x1301,
    GL_FRONT = 0x0404,
    GL_BACK = 0x0405,
    GL_BACK_LEFT = 0x0402,
    GL_FRONT_LEFT = 0x0400,
    GL_FRONT_RIGHT = 0x0401,
    GL_BACK_RIGHT = 0x0403,
    GL_LEFT = 0x0406,
    GL_RIGHT = 0x0407,
    GL_DRAW_FRAMEBUFFER_BINDING = 0x8CA6,
    GL_READ_FRAMEBUFFER_BINDING = 0x8CAA,
    GL_DRAW_BUFFER0 = 0x8825,
    GL_DRAW_BUFFER = 0x0C01,
    GL_READ_BUFFER = 0x0C02,
    GL_CURRENT_COLOR = 0x0B00,
    GL_CURRENT_NORMAL = 0x0B02,
    GL_CURRENT_TEXTURE_COORDS = 0x0B03,
    GL_MATRIX_MODE = 0x0BA0,
    GL_PROJECTION_MATRIX = 0x0BA7,
    GL_MODELVIEW = 0x1700,
    GL_PROJECTION = 0x1701,
    GL_TEXTURE = 0x1702,
    GL_SCISSOR_TEST = 0x0C11,
    GL_COLOR_WRITEMASK = 0x0C23,
    GL_UNPACK_ALIGNMENT = 0x0CF5,
    GL_PACK_ALIGNMENT = 0x0D05,
    GL_PACK_ROW_LENGTH = 0x0D02,
    GL_PACK_SKIP_ROWS = 0x0D03,
    GL_PACK_SKIP_PIXELS = 0x0D04,
    GL_TEXTURE_2D = 0x0DE1,
    GL_TEXTURE_RECTANGLE = 0x84F5,
    GL_TEXTURE_2D_MULTISAMPLE = 0x9100,
    GL_TEXTURE_WIDTH = 0x1000,
    GL_TEXTURE_HEIGHT = 0x1001,
    GL_TEXTURE_INTERNAL_FORMAT = 0x1003,
    GL_TEXTURE_SAMPLES = 0x9106,
    GL_TEXTURE_FIXED_SAMPLE_LOCATIONS = 0x9107,
    GL_TEXTURE_MAG_FILTER = 0x2800,
    GL_TEXTURE_MIN_FILTER = 0x2801,
    GL_TEXTURE_WRAP_S = 0x2802,
    GL_TEXTURE_WRAP_T = 0x2803,
    GL_TEXTURE_BASE_LEVEL = 0x813C,
    GL_TEXTURE_MAX_LEVEL = 0x813D,
    GL_TEXTURE_COMPARE_MODE = 0x884C,
    GL_TEXTURE_COMPARE_FUNC = 0x884D,
    GL_NEAREST = 0x2600,
    GL_LINEAR = 0x2601,
    GL_UNSIGNED_BYTE = 0x1401,
    GL_UNSIGNED_SHORT = 0x1403,
    GL_UNSIGNED_INT = 0x1405,
    GL_FLOAT = 0x1406,
    GL_RGBA = 0x1908,
    GL_BGRA = 0x80E1,
    GL_RGBA8 = 0x8058,
    GL_SRGB = 0x8C40,
    GL_SRGB8_ALPHA8 = 0x8C43,
    GL_DEPTH_COMPONENT = 0x1902,
    GL_DEPTH_COMPONENT16 = 0x81A5,
    GL_DEPTH_COMPONENT24 = 0x81A6,
    GL_DEPTH_COMPONENT32 = 0x81A7,
    GL_DEPTH_COMPONENT32F = 0x8CAC,
    GL_DEPTH_STENCIL = 0x84F9,
    GL_DEPTH24_STENCIL8 = 0x88F0,
    GL_DEPTH32F_STENCIL8 = 0x8CAD,
    GL_UNSIGNED_INT_24_8 = 0x84FA,
    GL_FLOAT_32_UNSIGNED_INT_24_8_REV = 0x8DAD,
    GL_VENDOR = 0x1F00,
    GL_RENDERER = 0x1F01,
    GL_VERSION = 0x1F02,
    GL_SHADING_LANGUAGE_VERSION = 0x8B8C,
    GL_TEXTURE0 = 0x84C0,
    GL_FRAMEBUFFER = 0x8D40,
    GL_READ_FRAMEBUFFER = 0x8CA8,
    GL_DRAW_FRAMEBUFFER = 0x8CA9,
    GL_RENDERBUFFER = 0x8D41,
    GL_COLOR_ATTACHMENT0 = 0x8CE0,
    GL_DEPTH_ATTACHMENT = 0x8D00,
    GL_STENCIL_ATTACHMENT = 0x8D20,
    GL_DEPTH_STENCIL_ATTACHMENT = 0x821A,
    GL_FRAMEBUFFER_COMPLETE = 0x8CD5,
    GL_FRAMEBUFFER_SRGB = 0x8DB9,
    GL_FRAMEBUFFER_ATTACHMENT_COLOR_ENCODING = 0x8210,
    GL_COLOR = 0x1800,
    GL_DEPTH = 0x1801,
    GL_STENCIL = 0x1802,
    GL_DEPTH_STENCIL_BUF = 0x84F9,   // glClearBufferfi buffer
    GL_VERTEX_SHADER = 0x8B31,
    GL_FRAGMENT_SHADER = 0x8B30,
    GL_SHADER_TYPE = 0x8B4F,
    GL_COMPILE_STATUS = 0x8B81,
    GL_LINK_STATUS = 0x8B82,
    GL_INFO_LOG_LENGTH = 0x8B84,
    GL_NUM_PROGRAM_BINARY_FORMATS = 0x87FE,
    GL_PROGRAM_BINARY_FORMATS = 0x87FF,
    GL_PIXEL_PACK_BUFFER = 0x88EB,
    GL_PIXEL_PACK_BUFFER_BINDING = 0x88ED,
    GL_SAMPLES = 0x80A9,
    GL_CONTEXT_PROFILE_MASK = 0x9126,
    GL_CONTEXT_CORE_PROFILE_BIT = 0x1,
    // state saved around the SR weaver
    GL_VIEWPORT = 0x0BA2,
    GL_CURRENT_PROGRAM = 0x8B8D,
    GL_VERTEX_ARRAY_BINDING = 0x85B5,
    GL_ARRAY_BUFFER = 0x8892,
    GL_ELEMENT_ARRAY_BUFFER = 0x8893,
    GL_ARRAY_BUFFER_BINDING = 0x8894,
    GL_ELEMENT_ARRAY_BUFFER_BINDING = 0x8895,
    GL_PIXEL_UNPACK_BUFFER = 0x88EC,
    GL_PIXEL_UNPACK_BUFFER_BINDING = 0x88EF,
    GL_SAMPLER_BINDING = 0x8919,
    GL_BLEND = 0x0BE2,
    GL_DEPTH_TEST = 0x0B71,
    GL_CULL_FACE = 0x0B44,
    GL_STENCIL_TEST = 0x0B90,
    GL_DEPTH_WRITEMASK = 0x0B72,
    GL_BLEND_DST_RGB = 0x80C8,
    GL_BLEND_SRC_RGB = 0x80C9,
    GL_BLEND_DST_ALPHA = 0x80CA,
    GL_BLEND_SRC_ALPHA = 0x80CB,
    GL_CLIENT_ALL_ATTRIB_BITS = 0xFFFFFFFF,
    // overlay
    GL_TRIANGLE_STRIP = 0x0005,
    GL_SRC_ALPHA = 0x0302,
    GL_ONE_MINUS_SRC_ALPHA = 0x0303,
    GL_UNPACK_ROW_LENGTH = 0x0CF2,
    GL_UNPACK_SKIP_ROWS = 0x0CF3,
    GL_UNPACK_SKIP_PIXELS = 0x0CF4,
    GL_ALPHA_TEST = 0x0BC0,
    GL_CLIP_DISTANCE0 = 0x3000,
    GL_POLYGON_MODE = 0x0B40,
    GL_RASTERIZER_DISCARD = 0x8C89,
    GL_SAMPLE_ALPHA_TO_COVERAGE = 0x809E,
    GL_COLOR_LOGIC_OP = 0x0BF2,
    GL_FRONT_AND_BACK = 0x0408,
    GL_FILL = 0x1B02,
    GL_FUNC_ADD = 0x8006,
    GL_BLEND_EQUATION_RGB = 0x8009,
    GL_BLEND_EQUATION_ALPHA = 0x883D,
    // auto convergence readback
    GL_STREAM_READ = 0x88E1,
    GL_READ_ONLY = 0x88B8,
    // upscale presentation (cross-context sync)
    GL_SYNC_GPU_COMMANDS_COMPLETE = 0x9117,
    GL_ALREADY_SIGNALED = 0x911A,
    GL_TIMEOUT_EXPIRED = 0x911B,
    GL_CONDITION_SATISFIED = 0x911C,
    // draw trace
    GL_SCISSOR_BOX = 0x0C10,
    GL_ACTIVE_UNIFORMS = 0x8B86,
    GL_FLOAT_MAT4 = 0x8B5C,
    GL_TEXTURE_BINDING_2D = 0x8069,
    GL_GEOMETRY_SHADER = 0x8DD9,
    GL_CLIP_ORIGIN = 0x935C,
    GL_CLIP_DEPTH_MODE = 0x935D,
    GL_LOWER_LEFT = 0x8CA1,
    GL_NEGATIVE_ONE_TO_ONE = 0x935E,
    GL_TEXTURE_IMMUTABLE_FORMAT = 0x912F,
    GL_TEXTURE_IMMUTABLE_LEVELS = 0x82DF,
    GL_ATTACHED_SHADERS = 0x8B85,
    GL_TRIANGLES = 0x0004,
    GL_TRIANGLE_FAN = 0x0006,
    // fixed-function client arrays (flat depth reads a draw's vertices)
    GL_MODELVIEW_MATRIX = 0x0BA6,
    GL_VERTEX_ARRAY = 0x8074,
    GL_TEXTURE_COORD_ARRAY = 0x8078,
    GL_VERTEX_ARRAY_SIZE = 0x807A,
    GL_VERTEX_ARRAY_TYPE = 0x807B,
    GL_VERTEX_ARRAY_STRIDE = 0x807C,
    GL_TEXTURE_COORD_ARRAY_SIZE = 0x8088,
    GL_TEXTURE_COORD_ARRAY_TYPE = 0x8089,
    GL_TEXTURE_COORD_ARRAY_STRIDE = 0x808A,
    GL_VERTEX_ARRAY_POINTER = 0x808E,
    GL_TEXTURE_COORD_ARRAY_POINTER = 0x8092,
    GL_CLIENT_ACTIVE_TEXTURE = 0x84E1,
    GL_VERTEX_ARRAY_BUFFER_BINDING = 0x8896,
    GL_TEXTURE_COORD_ARRAY_BUFFER_BINDING = 0x889A,
};

constexpr GLuint64 GL_TIMEOUT_IGNORED = 0xFFFFFFFFFFFFFFFFull;

constexpr int kMaxColorAttachments = 8;
constexpr int kMaxUnits = 32;

// Real entry points the wrapper calls. GL 1.1 ones come from System32 opengl32.dll's exports,
// the rest from its wglGetProcAddress once a context is current.
#define S3D_GL11_FUNCS(X)                                                                            \
    X(void, DrawArrays, (GLenum mode, GLint first, GLsizei count))                                   \
    X(void, DrawElements, (GLenum mode, GLsizei count, GLenum type, const void* indices))            \
    X(void, Clear, (GLbitfield mask))                                                                \
    X(void, BindTexture, (GLenum target, GLuint texture))                                            \
    X(void, DeleteTextures, (GLsizei n, const GLuint* textures))                                     \
    X(void, GenTextures, (GLsizei n, GLuint* textures))                                              \
    X(void, TexImage2D, (GLenum target, GLint level, GLint ifmt, GLsizei w, GLsizei h, GLint border, \
                         GLenum format, GLenum type, const void* pixels))                            \
    X(void, TexSubImage2D, (GLenum target, GLint level, GLint xo, GLint yo, GLsizei w, GLsizei h,   \
                            GLenum format, GLenum type, const void* pixels))                         \
    X(void, CopyTexImage2D, (GLenum target, GLint level, GLenum ifmt, GLint x, GLint y, GLsizei w,  \
                             GLsizei h, GLint border))                                               \
    X(void, CopyTexSubImage2D, (GLenum target, GLint level, GLint xo, GLint yo, GLint x, GLint y,   \
                                GLsizei w, GLsizei h))                                               \
    X(void, TexParameteri, (GLenum target, GLenum pname, GLint param))                               \
    X(void, TexParameterf, (GLenum target, GLenum pname, GLfloat param))                             \
    X(void, TexParameteriv, (GLenum target, GLenum pname, const GLint* params))                      \
    X(void, TexParameterfv, (GLenum target, GLenum pname, const GLfloat* params))                    \
    X(void, GetTexParameteriv, (GLenum target, GLenum pname, GLint* params))                         \
    X(void, GetTexLevelParameteriv, (GLenum target, GLint level, GLenum pname, GLint* params))       \
    X(void, DrawBuffer, (GLenum mode))                                                               \
    X(void, ReadBuffer, (GLenum mode))                                                               \
    X(void, GetIntegerv, (GLenum pname, GLint* data))                                                \
    X(void, GetPointerv, (GLenum pname, void** params))                                              \
    X(void, GetBooleanv, (GLenum pname, GLboolean* data))                                            \
    X(void, GetFloatv, (GLenum pname, GLfloat* data))                                                \
    X(GLboolean, IsEnabled, (GLenum cap))                                                            \
    X(void, Enable, (GLenum cap))                                                                    \
    X(void, Disable, (GLenum cap))                                                                   \
    X(void, ColorMask, (GLboolean r, GLboolean g, GLboolean b, GLboolean a))                         \
    X(void, PixelStorei, (GLenum pname, GLint param))                                                \
    X(void, ReadPixels, (GLint x, GLint y, GLsizei w, GLsizei h, GLenum format, GLenum type,       \
                         void* pixels))                                                              \
    X(const GLubyte*, GetString, (GLenum name))                                                      \
    X(void, Begin, (GLenum mode))                                                                    \
    X(void, End, ())                                                                                 \
    X(void, Flush, ())                                                                               \
    X(void, Finish, ())                                                                              \
    X(void, MatrixMode, (GLenum mode))                                                               \
    X(void, LoadMatrixf, (const GLfloat* m))                                                         \
    X(void, PushMatrix, ())                                                                          \
    X(void, PopMatrix, ())                                                                           \
    X(void, PushAttrib, (GLbitfield mask))                                                           \
    X(void, PopAttrib, ())                                                                           \
    X(void, PushClientAttrib, (GLbitfield mask))                                                     \
    X(void, PopClientAttrib, ())                                                                     \
    X(void, Viewport, (GLint x, GLint y, GLsizei w, GLsizei h))                                      \
    X(void, Scissor, (GLint x, GLint y, GLsizei w, GLsizei h))                                       \
    X(void, DepthMask, (GLboolean flag))                                                             \
    X(void, BlendFunc, (GLenum src, GLenum dst))                                                     \
    X(void, PolygonMode, (GLenum face, GLenum mode))                                                 \
    X(void, NewList, (GLuint list, GLenum mode))                                                     \
    X(void, EndList, ())                                                                             \
    X(void, CallList, (GLuint list))                                                                 \
    X(void, CallLists, (GLsizei n, GLenum type, const void* lists))                                  \
    X(GLuint, GenLists, (GLsizei range))                                                             \
    X(void, Color4fv, (const GLfloat* v))                                                            \
    X(void, Normal3fv, (const GLfloat* v))                                                           \
    X(void, TexCoord4fv, (const GLfloat* v))                                                         \
    X(PROC, wglGetProcAddress, (LPCSTR name))                                                        \
    X(BOOL, wglMakeCurrent, (HDC dc, HGLRC rc))                                                      \
    X(HGLRC, wglCreateContext, (HDC dc))                                                             \
    X(BOOL, wglDeleteContext, (HGLRC rc))                                                            \
    X(BOOL, wglShareLists, (HGLRC a, HGLRC b))                                                       \
    X(BOOL, wglSwapBuffers, (HDC dc))                                                                \
    X(BOOL, wglSwapLayerBuffers, (HDC dc, UINT planes))                                              \
    X(DWORD, wglSwapMultipleBuffers, (UINT n, const WGLSWAP* swaps))

#define S3D_EXT_FUNCS(X)                                                                             \
    X(void, BindFramebuffer, (GLenum target, GLuint fb))                                             \
    X(void, GenFramebuffers, (GLsizei n, GLuint* ids))                                               \
    X(void, DeleteFramebuffers, (GLsizei n, const GLuint* ids))                                      \
    X(GLenum, CheckFramebufferStatus, (GLenum target))                                               \
    X(void, FramebufferTexture2D, (GLenum target, GLenum att, GLenum textarget, GLuint tex,          \
                                   GLint level))                                                     \
    X(void, FramebufferTexture, (GLenum target, GLenum att, GLuint tex, GLint level))                \
    X(void, FramebufferTextureLayer, (GLenum target, GLenum att, GLuint tex, GLint level,            \
                                      GLint layer))                                                  \
    X(void, FramebufferRenderbuffer, (GLenum target, GLenum att, GLenum rbtarget, GLuint rb))        \
    X(void, GetFramebufferAttachmentParameteriv, (GLenum target, GLenum att, GLenum pname,          \
                                                  GLint* v))                                         \
    X(void, DrawBuffers, (GLsizei n, const GLenum* bufs))                                            \
    X(void, BindRenderbuffer, (GLenum target, GLuint rb))                                            \
    X(void, GenRenderbuffers, (GLsizei n, GLuint* ids))                                              \
    X(void, DeleteRenderbuffers, (GLsizei n, const GLuint* ids))                                     \
    X(void, RenderbufferStorage, (GLenum target, GLenum ifmt, GLsizei w, GLsizei h))                 \
    X(void, RenderbufferStorageMultisample, (GLenum target, GLsizei samples, GLenum ifmt, GLsizei w, \
                                             GLsizei h))                                             \
    X(void, BlitFramebuffer, (GLint sx0, GLint sy0, GLint sx1, GLint sy1, GLint dx0, GLint dy0,     \
                              GLint dx1, GLint dy1, GLbitfield mask, GLenum filter))                 \
    X(void, ActiveTexture, (GLenum unit))                                                            \
    X(void, BindBuffer, (GLenum target, GLuint buffer))                                              \
    X(void, GenBuffers, (GLsizei n, GLuint* ids))                                                    \
    X(void, BufferData, (GLenum target, GLsizeiptr size, const void* data, GLenum usage))            \
    X(void*, MapBuffer, (GLenum target, GLenum access))                                              \
    X(GLboolean, UnmapBuffer, (GLenum target))                                                       \
    X(GLsync, FenceSync, (GLenum condition, GLbitfield flags))                                       \
    X(void, WaitSync, (GLsync sync, GLbitfield flags, GLuint64 timeout))                             \
    X(void, DeleteSync, (GLsync sync))                                                               \
    X(GLenum, ClientWaitSync, (GLsync sync, GLbitfield flags, GLuint64 timeout))                     \
    X(void, BindVertexArray, (GLuint vao))                                                           \
    X(void, BindSampler, (GLuint unit, GLuint sampler))                                              \
    X(void, BlendFuncSeparate, (GLenum srcRgb, GLenum dstRgb, GLenum srcA, GLenum dstA))             \
    X(void, BlendEquation, (GLenum mode))                                                            \
    X(void, BlendEquationSeparate, (GLenum rgb, GLenum alpha))                                       \
    X(void, GenerateMipmap, (GLenum target))                                                         \
    X(void, CopyImageSubData, (GLuint src, GLenum srcTarget, GLint srcLevel, GLint srcX, GLint srcY, \
                               GLint srcZ, GLuint dst, GLenum dstTarget, GLint dstLevel, GLint dstX, \
                               GLint dstY, GLint dstZ, GLsizei w, GLsizei h, GLsizei d))             \
    X(void, TexStorage2D, (GLenum target, GLsizei levels, GLenum ifmt, GLsizei w, GLsizei h))        \
    X(void, TexImage2DMultisample, (GLenum target, GLsizei samples, GLenum ifmt, GLsizei w,          \
                                    GLsizei h, GLboolean fixed))                                     \
    X(void, ShaderSource, (GLuint sh, GLsizei count, const GLchar* const* str, const GLint* len))    \
    X(void, GetShaderiv, (GLuint sh, GLenum pname, GLint* v))                                        \
    X(GLuint, CreateShader, (GLenum type))                                                           \
    X(void, DeleteShader, (GLuint sh))                                                               \
    X(GLuint, CreateProgram, ())                                                                     \
    X(void, AttachShader, (GLuint prog, GLuint sh))                                                  \
    X(void, GetAttachedShaders, (GLuint prog, GLsizei max, GLsizei* count, GLuint* shaders))         \
    X(void, GetProgramInfoLog, (GLuint prog, GLsizei max, GLsizei* len, GLchar* log))                \
    X(void, Uniform1i, (GLint loc, GLint v))                                                         \
    X(void, GenVertexArrays, (GLsizei n, GLuint* ids))                                               \
    X(void, GetShaderInfoLog, (GLuint sh, GLsizei max, GLsizei* len, GLchar* log))                   \
    X(void, CompileShader, (GLuint sh))                                                              \
    X(void, LinkProgram, (GLuint prog))                                                              \
    X(void, UseProgram, (GLuint prog))                                                               \
    X(void, DeleteProgram, (GLuint prog))                                                            \
    X(void, DeleteObjectARB, (GLuint obj))                                                           \
    X(void, GetProgramiv, (GLuint prog, GLenum pname, GLint* v))                                     \
    X(GLint, GetUniformLocation, (GLuint prog, const GLchar* name))                                  \
    X(void, Uniform4f, (GLint loc, GLfloat a, GLfloat b, GLfloat c, GLfloat d))                      \
    X(void, ProgramBinary, (GLuint prog, GLenum fmt, const void* bin, GLsizei len))                  \
    X(void, GetProgramBinary, (GLuint prog, GLsizei bufSize, GLsizei* len, GLenum* fmt, void* bin)) \
    X(void, DrawArraysInstanced, (GLenum mode, GLint first, GLsizei count, GLsizei inst))            \
    X(void, DrawElementsInstanced, (GLenum mode, GLsizei count, GLenum type, const void* idx,        \
                                    GLsizei inst))                                                   \
    X(void, DrawRangeElements, (GLenum mode, GLuint start, GLuint end, GLsizei count, GLenum type,   \
                                const void* idx))                                                    \
    X(void, DrawElementsBaseVertex, (GLenum mode, GLsizei count, GLenum type, const void* idx,       \
                                     GLint base))                                                    \
    X(void, DrawRangeElementsBaseVertex, (GLenum mode, GLuint start, GLuint end, GLsizei count,      \
                                          GLenum type, const void* idx, GLint base))                 \
    X(void, DrawElementsInstancedBaseVertex, (GLenum mode, GLsizei count, GLenum type,              \
                                              const void* idx, GLsizei inst, GLint base))            \
    X(void, MultiDrawArrays, (GLenum mode, const GLint* first, const GLsizei* count, GLsizei n))     \
    X(void, MultiDrawElements, (GLenum mode, const GLsizei* count, GLenum type,                      \
                                const void* const* idx, GLsizei n))                                  \
    X(void, ClearBufferfv, (GLenum buf, GLint drawbuf, const GLfloat* v))                            \
    X(void, ClearBufferiv, (GLenum buf, GLint drawbuf, const GLint* v))                              \
    X(void, ClearBufferuiv, (GLenum buf, GLint drawbuf, const GLuint* v))                            \
    X(void, ClearBufferfi, (GLenum buf, GLint drawbuf, GLfloat d, GLint s))                          \
    X(HGLRC, wglCreateContextAttribsARB, (HDC dc, HGLRC share, const int* attribs))                  \
    X(BOOL, wglSwapIntervalEXT, (int interval))                                                      \
    X(void, GetActiveUniform, (GLuint prog, GLuint index, GLsizei bufSize, GLsizei* length, GLint* size,\
                               GLenum* type, GLchar* name))                                          \
    X(void, GetUniformfv, (GLuint prog, GLint loc, GLfloat* params))                                     X(void, GetUniformiv, (GLuint prog, GLint loc, GLint* params))                                   \
    X(void, DetachShader, (GLuint prog, GLuint sh))                                                  \
    X(void, ProgramStringARB, (GLenum target, GLenum format, GLsizei len, const void* str))          \
    X(void, BindProgramARB, (GLenum target, GLuint prog))                                            \
    X(void, DeleteProgramsARB, (GLsizei n, const GLuint* progs))                                     \
    X(void, ProgramEnvParameter4fARB, (GLenum target, GLuint index, GLfloat x, GLfloat y, GLfloat z, GLfloat w))\
    X(void, GetProgramivARB, (GLenum target, GLenum pname, GLint* params))                           \
    X(void, ClipControl, (GLenum origin, GLenum depth))                                              \
    X(void, TextureView, (GLuint tex, GLenum target, GLuint orig, GLenum ifmt, GLuint minLevel,      \
                          GLuint numLevels, GLuint minLayer, GLuint numLayers))                                                  \
    X(void, DrawArraysInstancedBaseInstance, (GLenum mode, GLint first, GLsizei count, GLsizei inst, GLuint bi))\
    X(void, DrawElementsInstancedBaseInstance, (GLenum mode, GLsizei count, GLenum type, const void* idx,\
                                                GLsizei inst, GLuint bi))                            \
    X(void, DrawElementsInstancedBaseVertexBaseInstance, (GLenum mode, GLsizei count, GLenum type,   \
                                                          const void* idx, GLsizei inst, GLint bv, GLuint bi))\
    X(void, DrawArraysIndirect, (GLenum mode, const void* ind))                                      \
    X(void, DrawElementsIndirect, (GLenum mode, GLenum type, const void* ind))                       \
    X(void, MultiDrawArraysIndirect, (GLenum mode, const void* ind, GLsizei n, GLsizei stride))      \
    X(void, MultiDrawElementsIndirect, (GLenum mode, GLenum type, const void* ind, GLsizei n, GLsizei stride))

struct RealGL {
#define S3D_DECL(ret, name, args) ret(GLAPIENTRY* name) args = nullptr;
    S3D_GL11_FUNCS(S3D_DECL)
    S3D_EXT_FUNCS(S3D_DECL)
#undef S3D_DECL
};
extern RealGL R;

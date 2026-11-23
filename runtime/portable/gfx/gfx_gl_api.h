/* The OpenGL 4.3 core API gfx_gl.c uses: its types, the constants, and the functions as pointers
 * loaded by name (gl_load) from whatever gives them - SDL_GL_GetProcAddress on the desktop,
 * eglGetProcAddress on the Switch. No system GL header is needed: Windows ships GL 1.1's, and the
 * Switch's Mesa only EGL's and GLES's. */
#pragma once

#include <stddef.h>
#include <stdint.h>

#if defined(_WIN32)
#define GLAPI_CALL __stdcall
#else
#define GLAPI_CALL
#endif

typedef unsigned int GLenum;
typedef unsigned int GLuint;
typedef int GLint;
typedef int GLsizei;
typedef unsigned char GLboolean;
typedef unsigned int GLbitfield;
typedef float GLfloat;
typedef double GLdouble;
typedef char GLchar;
typedef unsigned char GLubyte;
typedef intptr_t GLintptr;
typedef intptr_t GLsizeiptr;
typedef uint64_t GLuint64;
typedef int64_t GLint64;
typedef struct __GLsync* GLsync;
typedef void(GLAPI_CALL* GLDEBUGPROC)(GLenum source, GLenum type, GLuint id, GLenum severity, GLsizei length,
    const GLchar* message, const void* user);

enum
{
    GL_NO_ERROR = 0,
    GL_FALSE = 0,
    GL_TRUE = 1,
    GL_ZERO = 0,
    GL_ONE = 1,
    GL_NONE = 0,

    GL_DEPTH_BUFFER_BIT = 0x0100,
    GL_STENCIL_BUFFER_BIT = 0x0400,
    GL_COLOR_BUFFER_BIT = 0x4000,

    GL_POINTS = 0x0000,
    GL_LINES = 0x0001,
    GL_LINE_STRIP = 0x0003,
    GL_TRIANGLES = 0x0004,
    GL_TRIANGLE_STRIP = 0x0005,
    GL_TRIANGLE_FAN = 0x0006,

    GL_NEVER = 0x0200,
    GL_LESS = 0x0201,
    GL_EQUAL = 0x0202,
    GL_LEQUAL = 0x0203,
    GL_GREATER = 0x0204,
    GL_NOTEQUAL = 0x0205,
    GL_GEQUAL = 0x0206,
    GL_ALWAYS = 0x0207,

    GL_SRC_COLOR = 0x0300,
    GL_ONE_MINUS_SRC_COLOR = 0x0301,
    GL_SRC_ALPHA = 0x0302,
    GL_ONE_MINUS_SRC_ALPHA = 0x0303,
    GL_DST_ALPHA = 0x0304,
    GL_ONE_MINUS_DST_ALPHA = 0x0305,
    GL_DST_COLOR = 0x0306,
    GL_ONE_MINUS_DST_COLOR = 0x0307,
    GL_SRC_ALPHA_SATURATE = 0x0308,
    GL_FUNC_ADD = 0x8006,
    GL_MIN = 0x8007,
    GL_MAX = 0x8008,
    GL_FUNC_SUBTRACT = 0x800A,
    GL_FUNC_REVERSE_SUBTRACT = 0x800B,

    GL_FRONT = 0x0404,
    GL_BACK = 0x0405,
    GL_FRONT_AND_BACK = 0x0408,
    GL_CW = 0x0900,
    GL_CCW = 0x0901,
    GL_POINT = 0x1B00,
    GL_LINE = 0x1B01,
    GL_FILL = 0x1B02,

    GL_CULL_FACE = 0x0B44,
    GL_DEPTH_TEST = 0x0B71,
    GL_STENCIL_TEST = 0x0B90,
    GL_DITHER = 0x0BD0,
    GL_BLEND = 0x0BE2,
    GL_SCISSOR_TEST = 0x0C11,
    GL_POLYGON_OFFSET_FILL = 0x8037,
    GL_POLYGON_OFFSET_LINE = 0x2A02,
    GL_POLYGON_OFFSET_POINT = 0x2A01,
    GL_PROGRAM_POINT_SIZE = 0x8642,
    GL_TEXTURE_CUBE_MAP_SEAMLESS = 0x884F,
    GL_FRAMEBUFFER_SRGB = 0x8DB9,
    GL_DEBUG_OUTPUT_SYNCHRONOUS = 0x8242,
    GL_DEBUG_OUTPUT = 0x92E0,

    GL_KEEP = 0x1E00,
    GL_REPLACE = 0x1E01,
    GL_INCR = 0x1E02,
    GL_DECR = 0x1E03,
    GL_INVERT = 0x150A,
    GL_INCR_WRAP = 0x8507,
    GL_DECR_WRAP = 0x8508,

    GL_VENDOR = 0x1F00,
    GL_RENDERER = 0x1F01,
    GL_VERSION = 0x1F02,
    GL_EXTENSIONS = 0x1F03,
    GL_SHADING_LANGUAGE_VERSION = 0x8B8C,
    GL_NUM_EXTENSIONS = 0x821D,
    GL_MAJOR_VERSION = 0x821B,
    GL_MINOR_VERSION = 0x821C,
    GL_MAX_TEXTURE_SIZE = 0x0D33,
    GL_UNIFORM_BUFFER_OFFSET_ALIGNMENT = 0x8A34,
    GL_SHADER_STORAGE_BUFFER_OFFSET_ALIGNMENT = 0x90DF,
    GL_MAX_SHADER_STORAGE_BLOCK_SIZE = 0x90DE,
    GL_MAX_VERTEX_SHADER_STORAGE_BLOCKS = 0x90D6,
    GL_MAX_TEXTURE_MAX_ANISOTROPY = 0x84FF,

    GL_BYTE = 0x1400,
    GL_UNSIGNED_BYTE = 0x1401,
    GL_UNSIGNED_SHORT = 0x1403,
    GL_UNSIGNED_INT = 0x1405,
    GL_FLOAT = 0x1406,
    GL_UNSIGNED_SHORT_5_6_5 = 0x8363,
    GL_UNSIGNED_SHORT_4_4_4_4_REV = 0x8365,
    GL_UNSIGNED_SHORT_1_5_5_5_REV = 0x8366,
    GL_UNSIGNED_INT_24_8 = 0x84FA,

    GL_RED = 0x1903,
    GL_GREEN = 0x1904,
    GL_BLUE = 0x1905,
    GL_ALPHA = 0x1906,
    GL_RG = 0x8227,
    GL_RGB = 0x1907,
    GL_RGBA = 0x1908,
    GL_BGRA = 0x80E1,
    GL_DEPTH_COMPONENT = 0x1902,
    GL_DEPTH_STENCIL = 0x84F9,
    GL_R8 = 0x8229,
    GL_RG8 = 0x822B,
    GL_RGBA8 = 0x8058,
    GL_RGB565 = 0x8D62,
    GL_RGB5_A1 = 0x8057,
    GL_RGBA4 = 0x8056,
    GL_RG8_SNORM = 0x8F95,
    GL_DEPTH_COMPONENT16 = 0x81A5,
    GL_DEPTH24_STENCIL8 = 0x88F0,
    GL_COMPRESSED_RGBA_S3TC_DXT1_EXT = 0x83F1,
    GL_COMPRESSED_RGBA_S3TC_DXT3_EXT = 0x83F2,
    GL_COMPRESSED_RGBA_S3TC_DXT5_EXT = 0x83F3,

    GL_TEXTURE_2D = 0x0DE1,
    GL_TEXTURE_CUBE_MAP = 0x8513,
    GL_TEXTURE_BUFFER = 0x8C2A,
    GL_MAX_TEXTURE_BUFFER_SIZE = 0x8C2B,
    GL_R32UI = 0x8236,
    GL_TEXTURE_CUBE_MAP_POSITIVE_X = 0x8515,
    GL_TEXTURE0 = 0x84C0,
    GL_TEXTURE_MAG_FILTER = 0x2800,
    GL_TEXTURE_MIN_FILTER = 0x2801,
    GL_TEXTURE_WRAP_S = 0x2802,
    GL_TEXTURE_WRAP_T = 0x2803,
    GL_TEXTURE_WRAP_R = 0x8072,
    GL_TEXTURE_MIN_LOD = 0x813A,
    GL_TEXTURE_MAX_LOD = 0x813B,
    GL_TEXTURE_BASE_LEVEL = 0x813C,
    GL_TEXTURE_MAX_LEVEL = 0x813D,
    GL_TEXTURE_BORDER_COLOR = 0x1004,
    GL_TEXTURE_MAX_ANISOTROPY = 0x84FE,
    GL_TEXTURE_SWIZZLE_R = 0x8E42,
    GL_TEXTURE_SWIZZLE_G = 0x8E43,
    GL_TEXTURE_SWIZZLE_B = 0x8E44,
    GL_TEXTURE_SWIZZLE_A = 0x8E45,
    GL_NEAREST = 0x2600,
    GL_LINEAR = 0x2601,
    GL_NEAREST_MIPMAP_NEAREST = 0x2700,
    GL_LINEAR_MIPMAP_NEAREST = 0x2701,
    GL_NEAREST_MIPMAP_LINEAR = 0x2702,
    GL_LINEAR_MIPMAP_LINEAR = 0x2703,
    GL_REPEAT = 0x2901,
    GL_CLAMP_TO_BORDER = 0x812D,
    GL_CLAMP_TO_EDGE = 0x812F,
    GL_MIRRORED_REPEAT = 0x8370,
    GL_MIRROR_CLAMP_TO_EDGE = 0x8743,

    GL_UNPACK_ROW_LENGTH = 0x0CF2,
    GL_UNPACK_ALIGNMENT = 0x0CF5,
    GL_PACK_ROW_LENGTH = 0x0D02,
    GL_PACK_ALIGNMENT = 0x0D05,

    GL_ARRAY_BUFFER = 0x8892,
    GL_ELEMENT_ARRAY_BUFFER = 0x8893,
    GL_PIXEL_PACK_BUFFER = 0x88EB,
    GL_PIXEL_UNPACK_BUFFER = 0x88EC,
    GL_UNIFORM_BUFFER = 0x8A11,
    GL_SHADER_STORAGE_BUFFER = 0x90D2,
    GL_TRANSFORM_FEEDBACK_BUFFER = 0x8C8E,
    GL_INTERLEAVED_ATTRIBS = 0x8C8C,
    GL_RASTERIZER_DISCARD = 0x8C89,
    GL_TIMESTAMP = 0x8E28,
    GL_QUERY_RESULT = 0x8866,
    GL_QUERY_RESULT_AVAILABLE = 0x8867,
    GL_COPY_READ_BUFFER = 0x8F36,
    GL_COPY_WRITE_BUFFER = 0x8F37,
    GL_STREAM_DRAW = 0x88E0,
    GL_STREAM_READ = 0x88E1,
    GL_STATIC_DRAW = 0x88E4,
    GL_DYNAMIC_DRAW = 0x88E8,
    GL_MAP_READ_BIT = 0x0001,
    GL_MAP_WRITE_BIT = 0x0002,
    GL_MAP_INVALIDATE_RANGE_BIT = 0x0004,
    GL_MAP_UNSYNCHRONIZED_BIT = 0x0020,
    GL_MAP_PERSISTENT_BIT = 0x0040,
    GL_MAP_COHERENT_BIT = 0x0080,
    GL_DYNAMIC_STORAGE_BIT = 0x0100,
    GL_CLIENT_STORAGE_BIT = 0x0200,

    GL_FRAMEBUFFER = 0x8D40,
    GL_READ_FRAMEBUFFER = 0x8CA8,
    GL_DRAW_FRAMEBUFFER = 0x8CA9,
    GL_COLOR_ATTACHMENT0 = 0x8CE0,
    GL_DEPTH_ATTACHMENT = 0x8D00,
    GL_DEPTH_STENCIL_ATTACHMENT = 0x821A,
    GL_FRAMEBUFFER_COMPLETE = 0x8CD5,
    GL_BACK_LEFT = 0x0402,

    GL_FRAGMENT_SHADER = 0x8B30,
    GL_VERTEX_SHADER = 0x8B31,
    GL_COMPILE_STATUS = 0x8B81,
    GL_LINK_STATUS = 0x8B82,
    GL_INFO_LOG_LENGTH = 0x8B84,
    GL_PROGRAM_BINARY_LENGTH = 0x8741,
    GL_PROGRAM_BINARY_RETRIEVABLE_HINT = 0x8257,

    GL_SYNC_GPU_COMMANDS_COMPLETE = 0x9117,
    GL_SYNC_FLUSH_COMMANDS_BIT = 0x0001,
    GL_ALREADY_SIGNALED = 0x911A,
    GL_TIMEOUT_EXPIRED = 0x911B,
    GL_CONDITION_SATISFIED = 0x911C,
    GL_WAIT_FAILED = 0x911D,

    GL_LOWER_LEFT = 0x8CA1,
    GL_UPPER_LEFT = 0x8CA2,
    GL_NEGATIVE_ONE_TO_ONE = 0x935E,
    GL_ZERO_TO_ONE = 0x935F,

    GL_DEBUG_SEVERITY_HIGH = 0x9146,
    GL_DEBUG_SEVERITY_MEDIUM = 0x9147,
    GL_DEBUG_SEVERITY_LOW = 0x9148,
    GL_DEBUG_SEVERITY_NOTIFICATION = 0x826B,
};

/* name, return type, parameters, and whether the back end can do without it (optional ones are NULL
 * when the driver lacks them) */
#define GL_FUNCS(X)                                                                                                   \
    X(GetError, GLenum, (void), 0)                                                                                    \
    X(GetString, const GLubyte*, (GLenum), 0)                                                                         \
    X(GetStringi, const GLubyte*, (GLenum, GLuint), 0)                                                                \
    X(GetIntegerv, void, (GLenum, GLint*), 0)                                                                         \
    X(GetFloatv, void, (GLenum, GLfloat*), 0)                                                                         \
    X(Enable, void, (GLenum), 0)                                                                                      \
    X(Disable, void, (GLenum), 0)                                                                                     \
    X(Viewport, void, (GLint, GLint, GLsizei, GLsizei), 0)                                                            \
    X(Scissor, void, (GLint, GLint, GLsizei, GLsizei), 0)                                                             \
    X(DepthRangef, void, (GLfloat, GLfloat), 0)                                                                       \
    X(ClearColor, void, (GLfloat, GLfloat, GLfloat, GLfloat), 0)                                                      \
    X(ClearDepthf, void, (GLfloat), 0)                                                                                \
    X(ClearStencil, void, (GLint), 0)                                                                                 \
    X(Clear, void, (GLbitfield), 0)                                                                                   \
    X(ColorMask, void, (GLboolean, GLboolean, GLboolean, GLboolean), 0)                                               \
    X(DepthMask, void, (GLboolean), 0)                                                                                \
    X(DepthFunc, void, (GLenum), 0)                                                                                   \
    X(StencilFunc, void, (GLenum, GLint, GLuint), 0)                                                                  \
    X(StencilOp, void, (GLenum, GLenum, GLenum), 0)                                                                   \
    X(StencilMask, void, (GLuint), 0)                                                                                 \
    X(BlendFuncSeparate, void, (GLenum, GLenum, GLenum, GLenum), 0)                                                   \
    X(BlendEquation, void, (GLenum), 0)                                                                               \
    X(CullFace, void, (GLenum), 0)                                                                                    \
    X(FrontFace, void, (GLenum), 0)                                                                                   \
    X(PolygonOffset, void, (GLfloat, GLfloat), 0)                                                                     \
    X(PolygonMode, void, (GLenum, GLenum), 0)                                                                         \
    X(PixelStorei, void, (GLenum, GLint), 0)                                                                          \
    X(ReadPixels, void, (GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, void*), 0)                                   \
    X(Finish, void, (void), 0)                                                                                        \
    X(Flush, void, (void), 0)                                                                                         \
    X(GenTextures, void, (GLsizei, GLuint*), 0)                                                                       \
    X(DeleteTextures, void, (GLsizei, const GLuint*), 0)                                                              \
    X(BindTexture, void, (GLenum, GLuint), 0)                                                                         \
    X(ActiveTexture, void, (GLenum), 0)                                                                               \
    X(TexStorage2D, void, (GLenum, GLsizei, GLenum, GLsizei, GLsizei), 0)                                             \
    X(TexSubImage2D, void, (GLenum, GLint, GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, const void*), 0)           \
    X(CompressedTexSubImage2D, void, (GLenum, GLint, GLint, GLint, GLsizei, GLsizei, GLenum, GLsizei, const void*), 0) \
    X(TexParameteri, void, (GLenum, GLenum, GLint), 0)                                                                \
    X(TexBuffer, void, (GLenum, GLenum, GLuint), 0)                                                                   \
    X(GetTexImage, void, (GLenum, GLint, GLenum, GLenum, void*), 0)                                                   \
    X(GetCompressedTexImage, void, (GLenum, GLint, void*), 0)                                                         \
    X(GenSamplers, void, (GLsizei, GLuint*), 0)                                                                       \
    X(DeleteSamplers, void, (GLsizei, const GLuint*), 0)                                                              \
    X(BindSampler, void, (GLuint, GLuint), 0)                                                                         \
    X(SamplerParameteri, void, (GLuint, GLenum, GLint), 0)                                                            \
    X(SamplerParameterf, void, (GLuint, GLenum, GLfloat), 0)                                                          \
    X(SamplerParameterfv, void, (GLuint, GLenum, const GLfloat*), 0)                                                  \
    X(GenBuffers, void, (GLsizei, GLuint*), 0)                                                                        \
    X(DeleteBuffers, void, (GLsizei, const GLuint*), 0)                                                               \
    X(BindBuffer, void, (GLenum, GLuint), 0)                                                                          \
    X(BufferData, void, (GLenum, GLsizeiptr, const void*, GLenum), 0)                                                 \
    X(BufferSubData, void, (GLenum, GLintptr, GLsizeiptr, const void*), 0)                                            \
    X(MapBufferRange, void*, (GLenum, GLintptr, GLsizeiptr, GLbitfield), 0)                                           \
    X(UnmapBuffer, GLboolean, (GLenum), 0)                                                                            \
    X(BindBufferRange, void, (GLenum, GLuint, GLuint, GLintptr, GLsizeiptr), 0)                                       \
    X(BindBufferBase, void, (GLenum, GLuint, GLuint), 0)                                                              \
    X(GenVertexArrays, void, (GLsizei, GLuint*), 0)                                                                   \
    X(BindVertexArray, void, (GLuint), 0)                                                                             \
    X(GenFramebuffers, void, (GLsizei, GLuint*), 0)                                                                   \
    X(DeleteFramebuffers, void, (GLsizei, const GLuint*), 0)                                                          \
    X(BindFramebuffer, void, (GLenum, GLuint), 0)                                                                     \
    X(FramebufferTexture2D, void, (GLenum, GLenum, GLenum, GLuint, GLint), 0)                                         \
    X(CheckFramebufferStatus, GLenum, (GLenum), 0)                                                                    \
    X(BlitFramebuffer, void, (GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLbitfield, GLenum), 0)         \
    X(DrawBuffer, void, (GLenum), 0)                                                                                  \
    X(ReadBuffer, void, (GLenum), 0)                                                                                  \
    X(CreateShader, GLuint, (GLenum), 0)                                                                              \
    X(ShaderSource, void, (GLuint, GLsizei, const GLchar* const*, const GLint*), 0)                                   \
    X(CompileShader, void, (GLuint), 0)                                                                               \
    X(GetShaderiv, void, (GLuint, GLenum, GLint*), 0)                                                                 \
    X(GetShaderInfoLog, void, (GLuint, GLsizei, GLsizei*, GLchar*), 0)                                                \
    X(DeleteShader, void, (GLuint), 0)                                                                                \
    X(CreateProgram, GLuint, (void), 0)                                                                               \
    X(AttachShader, void, (GLuint, GLuint), 0)                                                                        \
    X(DetachShader, void, (GLuint, GLuint), 0)                                                                        \
    X(LinkProgram, void, (GLuint), 0)                                                                                 \
    X(GetProgramiv, void, (GLuint, GLenum, GLint*), 0)                                                                \
    X(GetProgramInfoLog, void, (GLuint, GLsizei, GLsizei*, GLchar*), 0)                                               \
    X(DeleteProgram, void, (GLuint), 0)                                                                               \
    X(UseProgram, void, (GLuint), 0)                                                                                  \
    X(DrawArrays, void, (GLenum, GLint, GLsizei), 0)                                                                  \
    X(Uniform4f, void, (GLint, GLfloat, GLfloat, GLfloat, GLfloat), 0)                                                \
    X(DrawElements, void, (GLenum, GLsizei, GLenum, const void*), 0)                                                  \
    X(DrawElementsInstanced, void, (GLenum, GLsizei, GLenum, const void*, GLsizei), 1)                                \
    X(FenceSync, GLsync, (GLenum, GLbitfield), 0)                                                                     \
    X(ClientWaitSync, GLenum, (GLsync, GLbitfield, GLuint64), 0)                                                      \
    X(DeleteSync, void, (GLsync), 0)                                                                                  \
    X(CopyImageSubData, void,                                                                                         \
        (GLuint, GLenum, GLint, GLint, GLint, GLint, GLuint, GLenum, GLint, GLint, GLint, GLint, GLsizei, GLsizei,    \
            GLsizei),                                                                                                 \
        0)                                                                                                            \
    X(BufferStorage, void, (GLenum, GLsizeiptr, const void*, GLbitfield), 1)                                          \
    X(ClipControl, void, (GLenum, GLenum), 1)                                                                         \
    X(DebugMessageCallback, void, (GLDEBUGPROC, const void*), 1)                                                      \
    X(GetProgramBinary, void, (GLuint, GLsizei, GLsizei*, GLenum*, void*), 1)                                         \
    X(ProgramBinary, void, (GLuint, GLenum, const void*, GLsizei), 1)                                                 \
    X(ProgramParameteri, void, (GLuint, GLenum, GLint), 1)                                                         \
    X(TransformFeedbackVaryings, void, (GLuint, GLsizei, const GLchar* const*, GLenum), 1)                            \
    X(BeginTransformFeedback, void, (GLenum), 1)                                                                      \
    X(EndTransformFeedback, void, (void), 1)                                                                          \
    X(GenQueries, void, (GLsizei, GLuint*), 1)                                                                        \
    X(QueryCounter, void, (GLuint, GLenum), 1)                                                                        \
    X(GetQueryObjectiv, void, (GLuint, GLenum, GLint*), 1)                                                            \
    X(GetQueryObjectui64v, void, (GLuint, GLenum, GLuint64*), 1)                                                      \
    X(IsBuffer, GLboolean, (GLuint), 1)

#define GL_DECLARE(name, ret, params, optional) typedef ret(GLAPI_CALL* PFN_gl##name) params;
GL_FUNCS(GL_DECLARE)
#undef GL_DECLARE

typedef struct GlApi
{
#define GL_MEMBER(name, ret, params, optional) PFN_gl##name name;
    GL_FUNCS(GL_MEMBER)
#undef GL_MEMBER
} GlApi;

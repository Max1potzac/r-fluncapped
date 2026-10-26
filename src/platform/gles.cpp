#include "platform/gles.h"

#include "core/log.h"
#include "platform/etc.h"
#include "shims/common.h"

// SDL's copies of the Khronos headers, so no GLES SDK is needed on any platform.
#define SDL_USE_BUILTIN_OPENGL_DEFINITIONS 1
#define GL_GLES_PROTOTYPES 0
#include <SDL_opengles2.h>

#include <algorithm>
#include <atomic>
#include <bit>
#include <set>
#include <thread>

namespace rfu::platform {

namespace {

// Every GL function the game imports (and nothing else: it gets no others).
#define RFU_GL_FUNCTIONS(X)                                                                                         \
    X(ActiveTexture, ACTIVETEXTURE)                                                                                 \
    X(AttachShader, ATTACHSHADER)                                                                                   \
    X(BindAttribLocation, BINDATTRIBLOCATION)                                                                       \
    X(BindFramebuffer, BINDFRAMEBUFFER)                                                                             \
    X(BindTexture, BINDTEXTURE)                                                                                     \
    X(BlendFunc, BLENDFUNC)                                                                                         \
    X(CheckFramebufferStatus, CHECKFRAMEBUFFERSTATUS)                                                               \
    X(Clear, CLEAR)                                                                                                 \
    X(ClearColor, CLEARCOLOR)                                                                                       \
    X(ClearDepthf, CLEARDEPTHF)                                                                                     \
    X(ColorMask, COLORMASK)                                                                                         \
    X(CompileShader, COMPILESHADER)                                                                                 \
    X(CompressedTexImage2D, COMPRESSEDTEXIMAGE2D)                                                                   \
    X(CreateProgram, CREATEPROGRAM)                                                                                 \
    X(CreateShader, CREATESHADER)                                                                                   \
    X(CullFace, CULLFACE)                                                                                           \
    X(DeleteFramebuffers, DELETEFRAMEBUFFERS)                                                                       \
    X(DeleteProgram, DELETEPROGRAM)                                                                                 \
    X(DeleteShader, DELETESHADER)                                                                                   \
    X(DeleteTextures, DELETETEXTURES)                                                                               \
    X(DepthFunc, DEPTHFUNC)                                                                                         \
    X(DepthMask, DEPTHMASK)                                                                                         \
    X(DepthRangef, DEPTHRANGEF)                                                                                     \
    X(Disable, DISABLE)                                                                                             \
    X(DisableVertexAttribArray, DISABLEVERTEXATTRIBARRAY)                                                           \
    X(DrawArrays, DRAWARRAYS)                                                                                       \
    X(Enable, ENABLE)                                                                                               \
    X(EnableVertexAttribArray, ENABLEVERTEXATTRIBARRAY)                                                             \
    X(FramebufferTexture2D, FRAMEBUFFERTEXTURE2D)                                                                   \
    X(FrontFace, FRONTFACE)                                                                                         \
    X(GenFramebuffers, GENFRAMEBUFFERS)                                                                             \
    X(GenTextures, GENTEXTURES)                                                                                     \
    X(GetError, GETERROR)                                                                                           \
    X(GetIntegerv, GETINTEGERV)                                                                                     \
    X(GetProgramInfoLog, GETPROGRAMINFOLOG)                                                                         \
    X(GetProgramiv, GETPROGRAMIV)                                                                                   \
    X(GetShaderInfoLog, GETSHADERINFOLOG)                                                                           \
    X(GetShaderiv, GETSHADERIV)                                                                                     \
    X(GetString, GETSTRING)                                                                                         \
    X(GetUniformLocation, GETUNIFORMLOCATION)                                                                       \
    X(IsProgram, ISPROGRAM)                                                                                         \
    X(IsShader, ISSHADER)                                                                                           \
    X(IsTexture, ISTEXTURE)                                                                                         \
    X(LineWidth, LINEWIDTH)                                                                                         \
    X(LinkProgram, LINKPROGRAM)                                                                                     \
    X(PixelStorei, PIXELSTOREI)                                                                                     \
    X(Scissor, SCISSOR)                                                                                             \
    X(ShaderSource, SHADERSOURCE)                                                                                   \
    X(TexImage2D, TEXIMAGE2D)                                                                                       \
    X(TexParameteri, TEXPARAMETERI)                                                                                 \
    X(TexSubImage2D, TEXSUBIMAGE2D)                                                                                 \
    X(Uniform1i, UNIFORM1I)                                                                                         \
    X(Uniform4fv, UNIFORM4FV)                                                                                       \
    X(UniformMatrix4fv, UNIFORMMATRIX4FV)                                                                           \
    X(UseProgram, USEPROGRAM)                                                                                       \
    X(VertexAttribPointer, VERTEXATTRIBPOINTER)                                                                     \
    X(Viewport, VIEWPORT)

struct Gl {
#define RFU_GL_MEMBER(name, upper) PFNGL##upper##PROC name = nullptr;
    RFU_GL_FUNCTIONS(RFU_GL_MEMBER)
#undef RFU_GL_MEMBER
    std::set<std::uint32_t> compressed_formats;  // what the driver takes as compressed textures
    std::thread::id thread;                       // where the context is current
};

Gl g_gl;
std::atomic<bool> g_loaded{false};
std::atomic<GLuint> g_default_fbo{0};

// The game makes every GL call from its render thread, which is where rfu made the context
// current; a call from anywhere else would go to no context at all.
const Gl& gl(CallContext& c) {
    if (!g_loaded.load(std::memory_order_acquire)) throw GuestFault("GL call " + c.thunk().name + " before the GL context exists");
    if (std::this_thread::get_id() != g_gl.thread && c.thunk().calls.load() < 4)
        log::warn("{} called from guest thread {}, which has no GL context", c.thunk().name, c.thread().id);
    return g_gl;
}

GLenum e(CallContext& c, int i) { return static_cast<GLenum>(c.x(i)); }
GLint i32(CallContext& c, int i) { return static_cast<GLint>(c.x(i)); }
GLuint u32(CallContext& c, int i) { return static_cast<GLuint>(c.x(i)); }
GLboolean b(CallContext& c, int i) { return static_cast<GLboolean>(c.x(i) & 0xFF); }
template <typename T = void>
T* p(CallContext& c, int i) {
    return c.ptr<T>(i);
}
template <typename T = void>
T* p_stack(CallContext& c, int slot) {
    GuestAddr a = c.stack_arg(slot);
    return a ? c.mem().host_as<T>(a) : nullptr;
}

void compressed_tex_image_2d(CallContext& c) {
    // (target, level, internalformat, width, height, border, imageSize, data)
    const Gl& g = gl(c);
    GLenum format = e(c, 2);
    GLsizei width = i32(c, 3), height = i32(c, 4), size = i32(c, 6);
    if (is_etc_format(format) && !g.compressed_formats.count(format)) {
        auto rgba = decode_etc(format, {p<const std::uint8_t>(c, 7), static_cast<std::size_t>(size)}, width, height);
        if (rgba.empty()) {
            log::warn("glCompressedTexImage2D: {}x{} texture of format {:#x} has only {} bytes", width, height, format, size);
            return;
        }
        GLint align = 4;
        g.GetIntegerv(GL_UNPACK_ALIGNMENT, &align);
        g.PixelStorei(GL_UNPACK_ALIGNMENT, 4);
        g.TexImage2D(e(c, 0), i32(c, 1), GL_RGBA, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
        g.PixelStorei(GL_UNPACK_ALIGNMENT, align);
        return;
    }
    g.CompressedTexImage2D(e(c, 0), i32(c, 1), format, width, height, i32(c, 5), size, p(c, 7));
}

void get_integerv(CallContext& c) {
    const Gl& g = gl(c);
    GLenum pname = e(c, 0);
    GLint* out = p<GLint>(c, 1);
    // Report the formats the guest can use: what the driver takes, plus the ETC formats
    // glCompressedTexImage2D decodes itself.
    std::vector<GLint> formats(g.compressed_formats.begin(), g.compressed_formats.end());
    for (std::uint32_t f : {kGL_ETC1_RGB8_OES, kGL_COMPRESSED_RGB8_ETC2, kGL_COMPRESSED_RGBA8_ETC2_EAC})
        if (!g.compressed_formats.count(f)) formats.push_back(static_cast<GLint>(f));
    if (pname == GL_NUM_COMPRESSED_TEXTURE_FORMATS) {
        *out = static_cast<GLint>(formats.size());
        return;
    }
    if (pname == GL_COMPRESSED_TEXTURE_FORMATS) {
        std::copy(formats.begin(), formats.end(), out);
        return;
    }
    g.GetIntegerv(pname, out);
    // The offscreen framebuffer stands in for the window's: the game sees it as 0.
    if (pname == GL_FRAMEBUFFER_BINDING && *out == static_cast<GLint>(g_default_fbo.load())) *out = 0;
}

void get_string(CallContext& c) {
    const Gl& g = gl(c);
    GLenum name = e(c, 0);
    const char* s = reinterpret_cast<const char*>(g.GetString(name));
    std::string text = s ? s : "";
    if (name == GL_EXTENSIONS && text.find("GL_OES_compressed_ETC1_RGB8_texture") == std::string::npos)
        text += (text.empty() || text.back() == ' ' ? "" : " ") + std::string("GL_OES_compressed_ETC1_RGB8_texture");
    c.ret(s || name == GL_EXTENSIONS ? intern_string(c.rt(), text) : 0);
}

void shader_source(CallContext& c) {
    // (shader, count, const char* const* strings, const GLint* lengths): an array of guest pointers.
    GLsizei count = i32(c, 1);
    std::vector<const GLchar*> strings(static_cast<std::size_t>(std::max(count, 0)));
    GuestAddr array = c.x(2);
    for (GLsizei i = 0; i < count; ++i) {
        GuestAddr s = c.mem().read<GuestAddr>(array + 8ull * i);
        strings[i] = s ? c.mem().host_as<const GLchar>(s) : nullptr;
    }
    gl(c).ShaderSource(u32(c, 0), count, strings.data(), p<const GLint>(c, 3));
}

}  // namespace

void load_gles(void* (*get_proc)(const char*)) {
#define RFU_GL_LOAD(name, upper)                                                               \
    g_gl.name = reinterpret_cast<PFNGL##upper##PROC>(get_proc("gl" #name));                    \
    if (!g_gl.name) throw std::runtime_error("the GL driver has no gl" #name);
    RFU_GL_FUNCTIONS(RFU_GL_LOAD)
#undef RFU_GL_LOAD
    GLint n = 0;
    g_gl.GetIntegerv(GL_NUM_COMPRESSED_TEXTURE_FORMATS, &n);
    std::vector<GLint> formats(static_cast<std::size_t>(std::max(n, 0)));
    if (n > 0) g_gl.GetIntegerv(GL_COMPRESSED_TEXTURE_FORMATS, formats.data());
    g_gl.compressed_formats = {formats.begin(), formats.end()};
    g_gl.thread = std::this_thread::get_id();
    auto str = [](GLenum e) {
        auto s = reinterpret_cast<const char*>(g_gl.GetString(e));
        return std::string(s ? s : "?");
    };
    log::info("GL: {} / {} / {}", str(GL_VENDOR), str(GL_RENDERER), str(GL_VERSION));
    log::info("GL: driver decodes ETC1 {}, ETC2 {}; the rest is decoded in software",
              g_gl.compressed_formats.count(kGL_ETC1_RGB8_OES) ? "yes" : "no",
              g_gl.compressed_formats.count(kGL_COMPRESSED_RGBA8_ETC2_EAC) ? "yes" : "no");
    g_loaded.store(true, std::memory_order_release);
}

void set_default_framebuffer(unsigned fbo) { g_default_fbo.store(fbo); }

void register_gles(ShimRegistry& reg) {
    reg.add("glActiveTexture", [](CallContext& c) { gl(c).ActiveTexture(e(c, 0)); });
    reg.add("glAttachShader", [](CallContext& c) { gl(c).AttachShader(u32(c, 0), u32(c, 1)); });
    reg.add("glBindAttribLocation", [](CallContext& c) { gl(c).BindAttribLocation(u32(c, 0), u32(c, 1), c.str(2)); });
    reg.add("glBindFramebuffer", [](CallContext& c) {
        GLuint fbo = u32(c, 1);
        gl(c).BindFramebuffer(e(c, 0), fbo ? fbo : g_default_fbo.load());
    });
    reg.add("glBindTexture", [](CallContext& c) { gl(c).BindTexture(e(c, 0), u32(c, 1)); });
    reg.add("glBlendFunc", [](CallContext& c) { gl(c).BlendFunc(e(c, 0), e(c, 1)); });
    reg.add("glCheckFramebufferStatus", [](CallContext& c) { c.ret(gl(c).CheckFramebufferStatus(e(c, 0))); });
    reg.add("glClear", [](CallContext& c) { gl(c).Clear(static_cast<GLbitfield>(c.x(0))); });
    reg.add("glClearColor", [](CallContext& c) { gl(c).ClearColor(c.s(0), c.s(1), c.s(2), c.s(3)); });
    reg.add("glClearDepthf", [](CallContext& c) { gl(c).ClearDepthf(c.s(0)); });
    reg.add("glColorMask", [](CallContext& c) { gl(c).ColorMask(b(c, 0), b(c, 1), b(c, 2), b(c, 3)); });
    reg.add("glCompileShader", [](CallContext& c) { gl(c).CompileShader(u32(c, 0)); });
    reg.add("glCompressedTexImage2D", compressed_tex_image_2d);
    reg.add("glCreateProgram", [](CallContext& c) { c.ret(gl(c).CreateProgram()); });
    reg.add("glCreateShader", [](CallContext& c) { c.ret(gl(c).CreateShader(e(c, 0))); });
    reg.add("glCullFace", [](CallContext& c) { gl(c).CullFace(e(c, 0)); });
    reg.add("glDeleteFramebuffers", [](CallContext& c) { gl(c).DeleteFramebuffers(i32(c, 0), p<const GLuint>(c, 1)); });
    reg.add("glDeleteProgram", [](CallContext& c) { gl(c).DeleteProgram(u32(c, 0)); });
    reg.add("glDeleteShader", [](CallContext& c) { gl(c).DeleteShader(u32(c, 0)); });
    reg.add("glDeleteTextures", [](CallContext& c) { gl(c).DeleteTextures(i32(c, 0), p<const GLuint>(c, 1)); });
    reg.add("glDepthFunc", [](CallContext& c) { gl(c).DepthFunc(e(c, 0)); });
    reg.add("glDepthMask", [](CallContext& c) { gl(c).DepthMask(b(c, 0)); });
    reg.add("glDepthRangef", [](CallContext& c) { gl(c).DepthRangef(c.s(0), c.s(1)); });
    reg.add("glDisable", [](CallContext& c) { gl(c).Disable(e(c, 0)); });
    reg.add("glDisableVertexAttribArray", [](CallContext& c) { gl(c).DisableVertexAttribArray(u32(c, 0)); });
    reg.add("glDrawArrays", [](CallContext& c) { gl(c).DrawArrays(e(c, 0), i32(c, 1), i32(c, 2)); });
    reg.add("glEnable", [](CallContext& c) { gl(c).Enable(e(c, 0)); });
    reg.add("glEnableVertexAttribArray", [](CallContext& c) { gl(c).EnableVertexAttribArray(u32(c, 0)); });
    reg.add("glFramebufferTexture2D",
            [](CallContext& c) { gl(c).FramebufferTexture2D(e(c, 0), e(c, 1), e(c, 2), u32(c, 3), i32(c, 4)); });
    reg.add("glFrontFace", [](CallContext& c) { gl(c).FrontFace(e(c, 0)); });
    reg.add("glGenFramebuffers", [](CallContext& c) { gl(c).GenFramebuffers(i32(c, 0), p<GLuint>(c, 1)); });
    reg.add("glGenTextures", [](CallContext& c) { gl(c).GenTextures(i32(c, 0), p<GLuint>(c, 1)); });
    reg.add("glGetError", [](CallContext& c) { c.ret(gl(c).GetError()); });
    reg.add("glGetIntegerv", get_integerv);
    reg.add("glGetProgramInfoLog",
            [](CallContext& c) { gl(c).GetProgramInfoLog(u32(c, 0), i32(c, 1), p<GLsizei>(c, 2), p<GLchar>(c, 3)); });
    reg.add("glGetProgramiv", [](CallContext& c) { gl(c).GetProgramiv(u32(c, 0), e(c, 1), p<GLint>(c, 2)); });
    reg.add("glGetShaderInfoLog",
            [](CallContext& c) { gl(c).GetShaderInfoLog(u32(c, 0), i32(c, 1), p<GLsizei>(c, 2), p<GLchar>(c, 3)); });
    reg.add("glGetShaderiv", [](CallContext& c) { gl(c).GetShaderiv(u32(c, 0), e(c, 1), p<GLint>(c, 2)); });
    reg.add("glGetString", get_string);
    reg.add("glGetUniformLocation", [](CallContext& c) {
        c.ret(static_cast<std::uint32_t>(gl(c).GetUniformLocation(u32(c, 0), c.str(1))));
    });
    reg.add("glIsProgram", [](CallContext& c) { c.ret(gl(c).IsProgram(u32(c, 0))); });
    reg.add("glIsShader", [](CallContext& c) { c.ret(gl(c).IsShader(u32(c, 0))); });
    reg.add("glIsTexture", [](CallContext& c) { c.ret(gl(c).IsTexture(u32(c, 0))); });
    reg.add("glLineWidth", [](CallContext& c) { gl(c).LineWidth(c.s(0)); });
    reg.add("glLinkProgram", [](CallContext& c) { gl(c).LinkProgram(u32(c, 0)); });
    reg.add("glPixelStorei", [](CallContext& c) { gl(c).PixelStorei(e(c, 0), i32(c, 1)); });
    reg.add("glScissor", [](CallContext& c) { gl(c).Scissor(i32(c, 0), i32(c, 1), i32(c, 2), i32(c, 3)); });
    reg.add("glShaderSource", shader_source);
    reg.add("glTexImage2D", [](CallContext& c) {
        // (target, level, internalformat, width, height, border, format, type, pixels): the
        // ninth argument is on the stack.
        gl(c).TexImage2D(e(c, 0), i32(c, 1), i32(c, 2), i32(c, 3), i32(c, 4), i32(c, 5), e(c, 6), e(c, 7), p_stack(c, 0));
    });
    reg.add("glTexParameteri", [](CallContext& c) { gl(c).TexParameteri(e(c, 0), e(c, 1), i32(c, 2)); });
    reg.add("glTexSubImage2D", [](CallContext& c) {
        // (target, level, xoffset, yoffset, width, height, format, type, pixels)
        gl(c).TexSubImage2D(e(c, 0), i32(c, 1), i32(c, 2), i32(c, 3), i32(c, 4), i32(c, 5), e(c, 6), e(c, 7), p_stack(c, 0));
    });
    reg.add("glUniform1i", [](CallContext& c) { gl(c).Uniform1i(i32(c, 0), i32(c, 1)); });
    reg.add("glUniform4fv", [](CallContext& c) { gl(c).Uniform4fv(i32(c, 0), i32(c, 1), p<const GLfloat>(c, 2)); });
    reg.add("glUniformMatrix4fv",
            [](CallContext& c) { gl(c).UniformMatrix4fv(i32(c, 0), i32(c, 1), b(c, 2), p<const GLfloat>(c, 3)); });
    reg.add("glUseProgram", [](CallContext& c) { gl(c).UseProgram(u32(c, 0)); });
    reg.add("glVertexAttribPointer", [](CallContext& c) {
        // The game never binds a buffer object (it doesn't import glBindBuffer), so the
        // pointer is always a client-side array in guest memory.
        gl(c).VertexAttribPointer(u32(c, 0), i32(c, 1), e(c, 2), b(c, 3), i32(c, 4), p(c, 5));
    });
    reg.add("glViewport", [](CallContext& c) { gl(c).Viewport(i32(c, 0), i32(c, 1), i32(c, 2), i32(c, 3)); });
}

}  // namespace rfu::platform

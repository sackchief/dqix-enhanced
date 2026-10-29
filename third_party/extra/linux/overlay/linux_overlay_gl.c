#include "linux_overlay_gl.h"
#include "linux_overlay.h"
#include <SDL.h>
#include <SDL_opengl.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#ifndef GL_VERTEX_ARRAY_BINDING
#define GL_VERTEX_ARRAY_BINDING 0x85B5
#endif
#ifndef GL_DRAW_FRAMEBUFFER_BINDING
#define GL_DRAW_FRAMEBUFFER_BINDING 0x8CA6
#endif
#ifndef GL_DRAW_FRAMEBUFFER
#define GL_DRAW_FRAMEBUFFER 0x8CA9
#endif

typedef struct OverlayGlApi {
    GLuint (*CreateShader)(GLenum);
    void (*ShaderSource)(GLuint, GLsizei, const char *const *, const GLint *);
    void (*CompileShader)(GLuint);
    void (*GetShaderiv)(GLuint, GLenum, GLint *);
    void (*GetShaderInfoLog)(GLuint, GLsizei, GLsizei *, char *);
    void (*DeleteShader)(GLuint);
    GLuint (*CreateProgram)(void);
    void (*AttachShader)(GLuint, GLuint);
    void (*BindAttribLocation)(GLuint, GLuint, const char *);
    void (*LinkProgram)(GLuint);
    void (*GetProgramiv)(GLuint, GLenum, GLint *);
    void (*GetProgramInfoLog)(GLuint, GLsizei, GLsizei *, char *);
    void (*DeleteProgram)(GLuint);
    void (*UseProgram)(GLuint);
    GLint (*GetUniformLocation)(GLuint, const char *);
    void (*Uniform1i)(GLint, GLint);
    void (*Uniform4f)(GLint, GLfloat, GLfloat, GLfloat, GLfloat);
    void (*GenTextures)(GLsizei, GLuint *);
    void (*DeleteTextures)(GLsizei, const GLuint *);
    void (*ActiveTexture)(GLenum);
    void (*BindTexture)(GLenum, GLuint);
    void (*TexParameteri)(GLenum, GLenum, GLint);
    void (*TexImage2D)(GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum, const void *);
    void (*TexSubImage2D)(GLenum, GLint, GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, const void *);
    void (*GenVertexArrays)(GLsizei, GLuint *);
    void (*DeleteVertexArrays)(GLsizei, const GLuint *);
    void (*BindVertexArray)(GLuint);
    void (*GenBuffers)(GLsizei, GLuint *);
    void (*DeleteBuffers)(GLsizei, const GLuint *);
    void (*BindBuffer)(GLenum, GLuint);
    void (*BufferData)(GLenum, ptrdiff_t, const void *, GLenum);
    void (*EnableVertexAttribArray)(GLuint);
    void (*VertexAttribPointer)(GLuint, GLint, GLenum, GLboolean, GLsizei, const void *);
    void (*DrawArrays)(GLenum, GLint, GLsizei);
    void (*GetIntegerv)(GLenum, GLint *);
    GLboolean (*IsEnabled)(GLenum);
    void (*Enable)(GLenum);
    void (*Disable)(GLenum);
    void (*Viewport)(GLint, GLint, GLsizei, GLsizei);
    void (*BlendFuncSeparate)(GLenum, GLenum, GLenum, GLenum);
    void (*BlendEquationSeparate)(GLenum, GLenum);
    void (*BindFramebuffer)(GLenum, GLuint);
    void (*PixelStorei)(GLenum, GLint);
} OverlayGlApi;

static OverlayGlApi g_api;
static GLuint g_program, g_texture, g_vao, g_vbo;
static GLint g_sampler = -1, g_dim_only = -1, g_dim_color = -1;
static int g_initialized, g_api_failed, g_init_failed;

#define LOAD_GL(member, symbol) do { \
    *(void **)(&g_api.member) = SDL_GL_GetProcAddress(symbol); \
    if (!g_api.member) { \
        if (!g_api_failed) fprintf(stderr, "linux-overlay: missing OpenGL entry point %s\n", symbol); \
        g_api_failed = 1; return 0; \
    } \
} while (0)

static int load_api(void) {
    if (g_api_failed) return 0;
    LOAD_GL(CreateShader, "glCreateShader");
    LOAD_GL(ShaderSource, "glShaderSource");
    LOAD_GL(CompileShader, "glCompileShader");
    LOAD_GL(GetShaderiv, "glGetShaderiv");
    LOAD_GL(GetShaderInfoLog, "glGetShaderInfoLog");
    LOAD_GL(DeleteShader, "glDeleteShader");
    LOAD_GL(CreateProgram, "glCreateProgram");
    LOAD_GL(AttachShader, "glAttachShader");
    LOAD_GL(BindAttribLocation, "glBindAttribLocation");
    LOAD_GL(LinkProgram, "glLinkProgram");
    LOAD_GL(GetProgramiv, "glGetProgramiv");
    LOAD_GL(GetProgramInfoLog, "glGetProgramInfoLog");
    LOAD_GL(DeleteProgram, "glDeleteProgram");
    LOAD_GL(UseProgram, "glUseProgram");
    LOAD_GL(GetUniformLocation, "glGetUniformLocation");
    LOAD_GL(Uniform1i, "glUniform1i");
    LOAD_GL(Uniform4f, "glUniform4f");
    LOAD_GL(GenTextures, "glGenTextures");
    LOAD_GL(DeleteTextures, "glDeleteTextures");
    LOAD_GL(ActiveTexture, "glActiveTexture");
    LOAD_GL(BindTexture, "glBindTexture");
    LOAD_GL(TexParameteri, "glTexParameteri");
    LOAD_GL(TexImage2D, "glTexImage2D");
    LOAD_GL(TexSubImage2D, "glTexSubImage2D");
    LOAD_GL(GenVertexArrays, "glGenVertexArrays");
    LOAD_GL(DeleteVertexArrays, "glDeleteVertexArrays");
    LOAD_GL(BindVertexArray, "glBindVertexArray");
    LOAD_GL(GenBuffers, "glGenBuffers");
    LOAD_GL(DeleteBuffers, "glDeleteBuffers");
    LOAD_GL(BindBuffer, "glBindBuffer");
    LOAD_GL(BufferData, "glBufferData");
    LOAD_GL(EnableVertexAttribArray, "glEnableVertexAttribArray");
    LOAD_GL(VertexAttribPointer, "glVertexAttribPointer");
    LOAD_GL(DrawArrays, "glDrawArrays");
    LOAD_GL(GetIntegerv, "glGetIntegerv");
    LOAD_GL(IsEnabled, "glIsEnabled");
    LOAD_GL(Enable, "glEnable");
    LOAD_GL(Disable, "glDisable");
    LOAD_GL(Viewport, "glViewport");
    LOAD_GL(BlendFuncSeparate, "glBlendFuncSeparate");
    LOAD_GL(BlendEquationSeparate, "glBlendEquationSeparate");
    LOAD_GL(BindFramebuffer, "glBindFramebuffer");
    LOAD_GL(PixelStorei, "glPixelStorei");
    return 1;
}

static GLuint compile_shader(GLenum type, const char *source) {
    GLuint shader = g_api.CreateShader(type);
    GLint ok = GL_FALSE;
    if (!shader) return 0;
    g_api.ShaderSource(shader, 1, &source, NULL);
    g_api.CompileShader(shader);
    g_api.GetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (ok != GL_TRUE) {
        char log[1024] = {0};
        GLsizei length = 0;
        g_api.GetShaderInfoLog(shader, (GLsizei)sizeof log - 1, &length, log);
        fprintf(stderr, "linux-overlay: shader compilation failed: %s\n", log);
        g_api.DeleteShader(shader);
        return 0;
    }
    return shader;
}

static int initialize_overlay(void) {
    static const char vertex_source[] =
        "#version 330 core\n"
        "layout(location=0) in vec2 position;\n"
        "layout(location=1) in vec2 texcoord;\n"
        "out vec2 uv;\n"
        "void main(){ gl_Position=vec4(position,0.0,1.0); uv=texcoord; }\n";
    static const char fragment_source[] =
        "#version 330 core\n"
        "in vec2 uv;\n"
        "uniform sampler2D overlay_texture;\n"
        "uniform int dim_only;\n"
        "uniform vec4 dim_color;\n"
        "out vec4 out_color;\n"
        "void main(){ out_color=dim_only != 0 ? dim_color : texture(overlay_texture,uv); }\n";
    static const float vertices[16] = {
        -1.0f, -1.0f, 0.0f, 1.0f,
         1.0f, -1.0f, 1.0f, 1.0f,
        -1.0f,  1.0f, 0.0f, 0.0f,
         1.0f,  1.0f, 1.0f, 0.0f
    };
    GLuint vs = 0, fs = 0;
    GLint ok = GL_FALSE, old_vao = 0, old_buffer = 0, old_active = GL_TEXTURE0, old_texture = 0;
    if (g_initialized) return 1;
    if (g_init_failed || !load_api()) return 0;

    vs = compile_shader(GL_VERTEX_SHADER, vertex_source);
    fs = compile_shader(GL_FRAGMENT_SHADER, fragment_source);
    if (!vs || !fs) goto fail;
    g_program = g_api.CreateProgram();
    if (!g_program) goto fail;
    g_api.AttachShader(g_program, vs);
    g_api.AttachShader(g_program, fs);
    g_api.BindAttribLocation(g_program, 0, "position");
    g_api.BindAttribLocation(g_program, 1, "texcoord");
    g_api.LinkProgram(g_program);
    g_api.GetProgramiv(g_program, GL_LINK_STATUS, &ok);
    if (ok != GL_TRUE) {
        char log[1024] = {0};
        GLsizei length = 0;
        g_api.GetProgramInfoLog(g_program, (GLsizei)sizeof log - 1, &length, log);
        fprintf(stderr, "linux-overlay: shader link failed: %s\n", log);
        goto fail;
    }
    g_sampler = g_api.GetUniformLocation(g_program, "overlay_texture");
    g_dim_only = g_api.GetUniformLocation(g_program, "dim_only");
    g_dim_color = g_api.GetUniformLocation(g_program, "dim_color");
    if (g_sampler < 0 || g_dim_only < 0 || g_dim_color < 0) goto fail;

    g_api.GetIntegerv(GL_VERTEX_ARRAY_BINDING, &old_vao);
    g_api.GetIntegerv(GL_ARRAY_BUFFER_BINDING, &old_buffer);
    g_api.GetIntegerv(GL_ACTIVE_TEXTURE, &old_active);
    g_api.ActiveTexture(GL_TEXTURE0);
    g_api.GetIntegerv(GL_TEXTURE_BINDING_2D, &old_texture);

    g_api.GenVertexArrays(1, &g_vao);
    g_api.BindVertexArray(g_vao);
    g_api.GenBuffers(1, &g_vbo);
    g_api.BindBuffer(GL_ARRAY_BUFFER, g_vbo);
    g_api.BufferData(GL_ARRAY_BUFFER, (ptrdiff_t)sizeof vertices, vertices, GL_STATIC_DRAW);
    g_api.EnableVertexAttribArray(0);
    g_api.VertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, (GLsizei)(4 * sizeof(float)), (const void *)0);
    g_api.EnableVertexAttribArray(1);
    g_api.VertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, (GLsizei)(4 * sizeof(float)), (const void *)(2 * sizeof(float)));

    g_api.GenTextures(1, &g_texture);
    g_api.BindTexture(GL_TEXTURE_2D, g_texture);
    g_api.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    g_api.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    g_api.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    g_api.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    g_api.TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 512, 768, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);

    g_api.BindTexture(GL_TEXTURE_2D, (GLuint)old_texture);
    g_api.ActiveTexture((GLenum)old_active);
    g_api.BindBuffer(GL_ARRAY_BUFFER, (GLuint)old_buffer);
    g_api.BindVertexArray((GLuint)old_vao);
    g_api.DeleteShader(vs);
    g_api.DeleteShader(fs);
    g_initialized = 1;
    return 1;

fail:
    if (vs) g_api.DeleteShader(vs);
    if (fs) g_api.DeleteShader(fs);
    if (g_program && g_api.DeleteProgram) g_api.DeleteProgram(g_program);
    if (g_texture && g_api.DeleteTextures) g_api.DeleteTextures(1, &g_texture);
    if (g_vbo && g_api.DeleteBuffers) g_api.DeleteBuffers(1, &g_vbo);
    if (g_vao && g_api.DeleteVertexArrays) g_api.DeleteVertexArrays(1, &g_vao);
    g_program = g_texture = g_vbo = g_vao = 0;
    g_init_failed = 1;
    fprintf(stderr, "linux-overlay: OpenGL overlay initialization failed\n");
    return 0;
}

int dqix_linux_overlay_gl_draw(const uint8_t *rgba, int width, int height,
    int viewport_width, int viewport_height) {
    GLint old_program = 0, old_vao = 0, old_buffer = 0, old_active = GL_TEXTURE0;
    GLint old_texture = 0, old_viewport[4] = {0}, old_unpack = 4, old_draw_fbo = 0;
    GLint fit_x = 0, fit_y = 0, fit_width = 0, fit_height = 0;
    GLint old_blend_src_rgb = GL_ONE, old_blend_dst_rgb = GL_ZERO;
    GLint old_blend_src_alpha = GL_ONE, old_blend_dst_alpha = GL_ZERO;
    GLint old_blend_eq_rgb = GL_FUNC_ADD, old_blend_eq_alpha = GL_FUNC_ADD;
    GLboolean blend_on, depth_on, cull_on, scissor_on;
    if (!rgba || width <= 0 || height <= 0 || viewport_width <= 0 || viewport_height <= 0) return 0;
    if (!initialize_overlay()) return 0;
    if (width != DQIX_LINUX_OVERLAY_WIDTH || height != DQIX_LINUX_OVERLAY_HEIGHT) return 0;
    if (!dqix_linux_overlay_fit_rect(viewport_width, viewport_height,
                                     &fit_x, &fit_y, &fit_width, &fit_height)) return 0;

    g_api.GetIntegerv(GL_CURRENT_PROGRAM, &old_program);
    g_api.GetIntegerv(GL_VERTEX_ARRAY_BINDING, &old_vao);
    g_api.GetIntegerv(GL_ARRAY_BUFFER_BINDING, &old_buffer);
    g_api.GetIntegerv(GL_VIEWPORT, old_viewport);
    g_api.GetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &old_draw_fbo);
    blend_on = g_api.IsEnabled(GL_BLEND);
    depth_on = g_api.IsEnabled(GL_DEPTH_TEST);
    cull_on = g_api.IsEnabled(GL_CULL_FACE);
    scissor_on = g_api.IsEnabled(GL_SCISSOR_TEST);
    g_api.GetIntegerv(GL_BLEND_SRC_RGB, &old_blend_src_rgb);
    g_api.GetIntegerv(GL_BLEND_DST_RGB, &old_blend_dst_rgb);
    g_api.GetIntegerv(GL_BLEND_SRC_ALPHA, &old_blend_src_alpha);
    g_api.GetIntegerv(GL_BLEND_DST_ALPHA, &old_blend_dst_alpha);
    g_api.GetIntegerv(GL_BLEND_EQUATION_RGB, &old_blend_eq_rgb);
    g_api.GetIntegerv(GL_BLEND_EQUATION_ALPHA, &old_blend_eq_alpha);
    g_api.GetIntegerv(GL_UNPACK_ALIGNMENT, &old_unpack);
    g_api.GetIntegerv(GL_ACTIVE_TEXTURE, &old_active);
    g_api.ActiveTexture(GL_TEXTURE0);
    g_api.GetIntegerv(GL_TEXTURE_BINDING_2D, &old_texture);

    g_api.BindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
    g_api.Viewport(0, 0, viewport_width, viewport_height);
    g_api.Disable(GL_DEPTH_TEST);
    g_api.Disable(GL_CULL_FACE);
    g_api.Disable(GL_SCISSOR_TEST);
    g_api.Enable(GL_BLEND);
    g_api.BlendEquationSeparate(GL_FUNC_ADD, GL_FUNC_ADD);
    g_api.BlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    g_api.UseProgram(g_program);
    g_api.BindVertexArray(g_vao);
    g_api.ActiveTexture(GL_TEXTURE0);
    g_api.BindTexture(GL_TEXTURE_2D, g_texture);
    g_api.PixelStorei(GL_UNPACK_ALIGNMENT, 1);
    g_api.TexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, rgba);

    /* Dim the entire gameplay window before drawing the menu at its native aspect ratio. */
    g_api.Uniform4f(g_dim_color, 0.0f, 0.0f, 0.0f, 118.0f / 255.0f);
    g_api.Uniform1i(g_dim_only, 1);
    g_api.DrawArrays(GL_TRIANGLE_STRIP, 0, 4);

    g_api.Viewport(fit_x, fit_y, fit_width, fit_height);
    g_api.Uniform1i(g_dim_only, 0);
    g_api.Uniform1i(g_sampler, 0);
    g_api.DrawArrays(GL_TRIANGLE_STRIP, 0, 4);

    g_api.PixelStorei(GL_UNPACK_ALIGNMENT, old_unpack);
    g_api.BindTexture(GL_TEXTURE_2D, (GLuint)old_texture);
    g_api.ActiveTexture((GLenum)old_active);
    g_api.BindBuffer(GL_ARRAY_BUFFER, (GLuint)old_buffer);
    g_api.BindVertexArray((GLuint)old_vao);
    g_api.UseProgram((GLuint)old_program);
    g_api.Viewport(old_viewport[0], old_viewport[1], old_viewport[2], old_viewport[3]);
    g_api.BindFramebuffer(GL_DRAW_FRAMEBUFFER, (GLuint)old_draw_fbo);
    g_api.BlendFuncSeparate((GLenum)old_blend_src_rgb, (GLenum)old_blend_dst_rgb,
        (GLenum)old_blend_src_alpha, (GLenum)old_blend_dst_alpha);
    g_api.BlendEquationSeparate((GLenum)old_blend_eq_rgb, (GLenum)old_blend_eq_alpha);
    if (blend_on) g_api.Enable(GL_BLEND); else g_api.Disable(GL_BLEND);
    if (depth_on) g_api.Enable(GL_DEPTH_TEST); else g_api.Disable(GL_DEPTH_TEST);
    if (cull_on) g_api.Enable(GL_CULL_FACE); else g_api.Disable(GL_CULL_FACE);
    if (scissor_on) g_api.Enable(GL_SCISSOR_TEST); else g_api.Disable(GL_SCISSOR_TEST);
    return 1;
}

void dqix_linux_overlay_gl_shutdown(void) {
    if (!g_initialized || SDL_GL_GetCurrentContext() == NULL) return;
    g_api.DeleteTextures(1, &g_texture);
    g_api.DeleteBuffers(1, &g_vbo);
    g_api.DeleteVertexArrays(1, &g_vao);
    g_api.DeleteProgram(g_program);
    g_program = g_texture = g_vbo = g_vao = 0;
    g_initialized = 0;
}

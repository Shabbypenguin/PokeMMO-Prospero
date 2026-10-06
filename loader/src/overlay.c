// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 PokeMMO-Prospero contributors
//
// Overlay drawing (see overlay.h).
//
// loader-9: everything drawn between overlayBegin and overlayEnd is collected into one vertex list and drawn with this
// file's own small shader, in as few draw calls as there are texture changes (one or two per frame). loader-8 drew each
// rectangle with fixed-function glBegin/glEnd: on the PS5 every glBegin/glEnd is a separate draw, and the QR code alone
// made hundreds per frame, which slowed the game down.
//
// The game's state is kept: its program, framebuffer and array buffer are noted and put back, everything else is pushed
// with glPushAttrib/glPushClientAttrib (the game's context is OpenGL 2.1 with compatibility, as the cursor relies on too).
// Objects (program, textures) belong to a context: they are made again when the context changes, or when the platform says
// the previous one is gone (overlayContextLost: a new context can get the address of the one just destroyed).
#include "overlay.h"
#include "diagnostics.h"
#include "overlay_assets.h"
#include <EGL/egl.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>

typedef unsigned int GLenum, GLbitfield, GLuint;
typedef int GLint, GLsizei;
typedef float GLfloat;
typedef unsigned char GLboolean;
typedef char GLchar;
enum {
    GL_TRIANGLES = 0x0004,
    GL_TEXTURE_2D = 0x0DE1,
    GL_BLEND = 0x0BE2,
    GL_SRC_ALPHA = 0x0302,
    GL_ONE_MINUS_SRC_ALPHA = 0x0303,
    GL_FUNC_ADD = 0x8006,
    GL_CURRENT_PROGRAM = 0x8B8D,
    GL_FRAMEBUFFER = 0x8D40,
    GL_FRAMEBUFFER_BINDING = 0x8CA6,
    GL_ARRAY_BUFFER = 0x8892,
    GL_ARRAY_BUFFER_BINDING = 0x8894,
    GL_PIXEL_UNPACK_BUFFER = 0x88EC,
    GL_PIXEL_UNPACK_BUFFER_BINDING = 0x88EF,
    GL_PIXEL_PACK_BUFFER = 0x88EB,
    GL_PIXEL_PACK_BUFFER_BINDING = 0x88ED,
    GL_TEXTURE0 = 0x84C0,
    GL_ACTIVE_TEXTURE = 0x84E0,
    GL_RGBA = 0x1908,
    GL_UNSIGNED_BYTE = 0x1401,
    GL_FLOAT = 0x1406,
    GL_TEXTURE_MIN_FILTER = 0x2801,
    GL_TEXTURE_MAG_FILTER = 0x2800,
    GL_TEXTURE_WRAP_S = 0x2802,
    GL_TEXTURE_WRAP_T = 0x2803,
    GL_LINEAR = 0x2601,
    GL_CLAMP_TO_EDGE = 0x812F,
    GL_UNPACK_SWAP_BYTES = 0x0CF0,
    GL_UNPACK_LSB_FIRST = 0x0CF1,
    GL_UNPACK_ROW_LENGTH = 0x0CF2,
    GL_UNPACK_SKIP_ROWS = 0x0CF3,
    GL_UNPACK_SKIP_PIXELS = 0x0CF4,
    GL_UNPACK_ALIGNMENT = 0x0CF5,
    GL_PACK_ALIGNMENT = 0x0D05,
    GL_PACK_ROW_LENGTH = 0x0D02,
    GL_DEPTH_TEST = 0x0B71,
    GL_CULL_FACE = 0x0B44,
    GL_SCISSOR_TEST = 0x0C11,
    GL_STENCIL_TEST = 0x0B90,
    GL_FRONT_AND_BACK = 0x0408,
    GL_FILL = 0x1B02,
    GL_COLOR_BUFFER_BIT = 0x4000,
    GL_VERTEX_SHADER = 0x8B31,
    GL_FRAGMENT_SHADER = 0x8B30,
    GL_COMPILE_STATUS = 0x8B81,
    GL_LINK_STATUS = 0x8B82,
    GL_ALL_ATTRIB_BITS = 0x000FFFFF,
    GL_CLIENT_ALL_ATTRIB_BITS = 0xFFFFFFFF,
};
static struct {
    void (*PushAttrib)(GLbitfield);
    void (*PopAttrib)(void);
    void (*PushClientAttrib)(GLbitfield);
    void (*PopClientAttrib)(void);
    void (*GetIntegerv)(GLenum, GLint *);
    GLenum (*GetError)(void);
    GLboolean (*IsTexture)(GLuint);
    GLboolean (*IsProgram)(GLuint);
    void (*ActiveTexture)(GLenum);
    void (*BindTexture)(GLenum, GLuint);
    void (*GenTextures)(GLsizei, GLuint *);
    void (*TexImage2D)(GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum, const void *);
    void (*TexParameteri)(GLenum, GLenum, GLint);
    void (*PixelStorei)(GLenum, GLint);
    void (*BindBuffer)(GLenum, GLuint);
    void (*BlendFunc)(GLenum, GLenum);
    void (*BlendEquation)(GLenum);
    void (*ColorMask)(GLboolean, GLboolean, GLboolean, GLboolean);
    void (*PolygonMode)(GLenum, GLenum);
    void (*Viewport)(GLint, GLint, GLsizei, GLsizei);
    void (*BindFramebuffer)(GLenum, GLuint);
    void (*Enable)(GLenum);
    void (*Disable)(GLenum);
    void (*ClearColor)(GLfloat, GLfloat, GLfloat, GLfloat);
    void (*Clear)(GLbitfield);
    void (*ReadPixels)(GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, void *);
    GLuint (*CreateShader)(GLenum);
    void (*ShaderSource)(GLuint, GLsizei, const GLchar *const *, const GLint *);
    void (*CompileShader)(GLuint);
    void (*GetShaderiv)(GLuint, GLenum, GLint *);
    void (*GetShaderInfoLog)(GLuint, GLsizei, GLsizei *, GLchar *);
    GLuint (*CreateProgram)(void);
    void (*AttachShader)(GLuint, GLuint);
    void (*BindAttribLocation)(GLuint, GLuint, const GLchar *);
    void (*LinkProgram)(GLuint);
    void (*GetProgramiv)(GLuint, GLenum, GLint *);
    GLint (*GetUniformLocation)(GLuint, const GLchar *);
    void (*UseProgram)(GLuint);
    void (*Uniform1i)(GLint, GLint);
    void (*Uniform2f)(GLint, GLfloat, GLfloat);
    void (*EnableVertexAttribArray)(GLuint);
    void (*DisableVertexAttribArray)(GLuint);
    void (*VertexAttribPointer)(GLuint, GLint, GLenum, GLboolean, GLsizei, const void *);
    void (*DrawArrays)(GLenum, GLint, GLsizei);
} gl;

static int gl_state;  // 0 not tried, 1 ready, -1 unusable
static EGLContext objects_context;
static GLuint program, font_texture, logo_texture;
static GLint uniform_texture, uniform_view;
static uint32_t *font_pixels, *logo_pixels;
static GLint saved_program, saved_framebuffer, saved_array_buffer, saved_unpack, saved_active;
static unsigned view_width, view_height;

// One batch of triangles, all with the same texture.
typedef struct {
    float x, y, u, v;
    uint8_t rgba[4];
} Vertex;
#define MAX_VERTICES 12288u  // a multiple of 6
static Vertex vertices[MAX_VERTICES];
static unsigned vertex_count;
static GLuint batch_texture;
static float white_u, white_v;  // a texel of the font atlas that is solid white: rectangles use it

static bool loadFunction(void *slot, const char *name) {
    void *address = (void *)eglGetProcAddress(name);
    if (address) memcpy(slot, &address, sizeof(address));
    return address != NULL;
}
static bool loadGl(void) {
#define LOAD(field)                                                                                                                                            \
    if (!loadFunction(&gl.field, "gl" #field)) {                                                                                                               \
        diagnosticsTrace("overlay: gl" #field " missing");                                                                                                    \
        return false;                                                                                                                                          \
    }
    LOAD(PushAttrib) LOAD(PopAttrib) LOAD(PushClientAttrib) LOAD(PopClientAttrib) LOAD(GetIntegerv) LOAD(GetError) LOAD(IsTexture) LOAD(IsProgram)
    LOAD(ActiveTexture) LOAD(BindTexture) LOAD(GenTextures) LOAD(TexImage2D) LOAD(TexParameteri) LOAD(PixelStorei) LOAD(BindBuffer) LOAD(BlendFunc)
    LOAD(BlendEquation) LOAD(ColorMask) LOAD(PolygonMode) LOAD(Viewport) LOAD(BindFramebuffer) LOAD(Enable) LOAD(Disable) LOAD(ClearColor) LOAD(Clear)
    LOAD(ReadPixels) LOAD(CreateShader) LOAD(ShaderSource) LOAD(CompileShader) LOAD(GetShaderiv) LOAD(GetShaderInfoLog) LOAD(CreateProgram)
    LOAD(AttachShader) LOAD(BindAttribLocation) LOAD(LinkProgram) LOAD(GetProgramiv) LOAD(GetUniformLocation) LOAD(UseProgram) LOAD(Uniform1i)
    LOAD(Uniform2f) LOAD(EnableVertexAttribArray) LOAD(DisableVertexAttribArray) LOAD(VertexAttribPointer) LOAD(DrawArrays)
#undef LOAD
    return true;
}

// The assets are inflated once and kept: each new context gets its textures from them.
static bool inflateAssets(void) {
    if (font_pixels && logo_pixels) return true;
    uLongf font_bytes = (uLongf)overlay_font_width * overlay_font_height, logo_bytes = (uLongf)overlay_logo_width * overlay_logo_height * 4u;
    unsigned char *alpha = malloc(font_bytes);
    font_pixels = malloc(font_bytes * 4u);
    logo_pixels = malloc(logo_bytes);
    if (!alpha || !font_pixels || !logo_pixels || uncompress(alpha, &font_bytes, overlay_font_alpha, overlay_font_alpha_size) != Z_OK ||
        uncompress((Bytef *)logo_pixels, &logo_bytes, overlay_logo_rgba, overlay_logo_rgba_size) != Z_OK) {
        free(alpha);
        free(font_pixels);
        free(logo_pixels);
        font_pixels = logo_pixels = NULL;
        return false;
    }
    for (uLongf i = 0; i < font_bytes; ++i) font_pixels[i] = 0x00FFFFFFu | ((uint32_t)alpha[i] << 24);  // white, coverage as alpha (RGBA in memory)
    free(alpha);
    // The atlas's bottom-right 4x4 corner (unused by glyphs) becomes solid white for rectangles.
    for (unsigned y = overlay_font_height - 4; y < overlay_font_height; ++y)
        for (unsigned x = overlay_font_width - 4; x < overlay_font_width; ++x) font_pixels[y * overlay_font_width + x] = 0xFFFFFFFFu;
    white_u = ((float)overlay_font_width - 2.0f) / (float)overlay_font_width;
    white_v = ((float)overlay_font_height - 2.0f) / (float)overlay_font_height;
    return true;
}
static GLuint upload(const uint32_t *pixels, unsigned width, unsigned height) {
    GLuint texture = 0;
    gl.GenTextures(1, &texture);
    gl.BindTexture(GL_TEXTURE_2D, texture);
    gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    gl.TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, (GLsizei)width, (GLsizei)height, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
    return texture;
}
static GLuint compile(GLenum type, const char *source) {
    GLuint shader = gl.CreateShader(type);
    gl.ShaderSource(shader, 1, &source, NULL);
    gl.CompileShader(shader);
    GLint ok = 0;
    gl.GetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[512] = "";
        gl.GetShaderInfoLog(shader, sizeof(log), NULL, log);
        diagnosticsTrace("overlay: shader compile failed: %s", log);
    }
    return shader;
}
// GLSL 1.20: the game's context is OpenGL 2.1. Positions are in virtual-screen pixels.
static bool makeObjects(void) {
    static const char *const vertex_source = "#version 120\n"
                                             "attribute vec2 position; attribute vec2 coordinate; attribute vec4 colour;\n"
                                             "uniform vec2 view; varying vec2 uv; varying vec4 tint;\n"
                                             "void main() { uv = coordinate; tint = colour;\n"
                                             "  gl_Position = vec4(position.x * 2.0 / view.x - 1.0, 1.0 - position.y * 2.0 / view.y, 0.0, 1.0); }\n";
    static const char *const fragment_source = "#version 120\n"
                                               "uniform sampler2D image; varying vec2 uv; varying vec4 tint;\n"
                                               "void main() { gl_FragColor = texture2D(image, uv) * tint; }\n";
    program = gl.CreateProgram();
    gl.AttachShader(program, compile(GL_VERTEX_SHADER, vertex_source));
    gl.AttachShader(program, compile(GL_FRAGMENT_SHADER, fragment_source));
    gl.BindAttribLocation(program, 0, "position");
    gl.BindAttribLocation(program, 1, "coordinate");
    gl.BindAttribLocation(program, 2, "colour");
    gl.LinkProgram(program);
    GLint linked = 0;
    gl.GetProgramiv(program, GL_LINK_STATUS, &linked);
    if (!linked) {
        diagnosticsTrace("overlay: program link failed");
        return false;
    }
    uniform_texture = gl.GetUniformLocation(program, "image");
    uniform_view = gl.GetUniformLocation(program, "view");
    font_texture = upload(font_pixels, overlay_font_width, overlay_font_height);
    logo_texture = upload(logo_pixels, overlay_logo_width, overlay_logo_height);
    diagnosticsTrace("overlay: objects made program=%u font=%u logo=%u gl_error=0x%x", program, font_texture, logo_texture, gl.GetError());
    return true;
}

void overlayContextLost(void) { objects_context = EGL_NO_CONTEXT; }

bool overlayBegin(unsigned width, unsigned height) {
    if (!gl_state) gl_state = loadGl() && inflateAssets() ? 1 : -1;
    EGLContext context = eglGetCurrentContext();
    if (gl_state < 0 || context == EGL_NO_CONTEXT) return false;
    while (gl.GetError()) {}  // the game's errors are not ours
    saved_program = saved_framebuffer = saved_array_buffer = saved_unpack = 0;
    saved_active = GL_TEXTURE0;
    gl.GetIntegerv(GL_CURRENT_PROGRAM, &saved_program);
    gl.GetIntegerv(GL_FRAMEBUFFER_BINDING, &saved_framebuffer);
    gl.GetIntegerv(GL_ARRAY_BUFFER_BINDING, &saved_array_buffer);
    gl.GetIntegerv(GL_PIXEL_UNPACK_BUFFER_BINDING, &saved_unpack);
    gl.GetIntegerv(GL_ACTIVE_TEXTURE, &saved_active);
    gl.ActiveTexture(GL_TEXTURE0);
    gl.PushAttrib(GL_ALL_ATTRIB_BITS);
    gl.PushClientAttrib(GL_CLIENT_ALL_ATTRIB_BITS);
    gl.BindFramebuffer(GL_FRAMEBUFFER, 0);
    gl.BindBuffer(GL_ARRAY_BUFFER, 0);
    gl.BindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
    static const GLenum unpack[][2] = {{GL_UNPACK_SWAP_BYTES, 0}, {GL_UNPACK_LSB_FIRST, 0}, {GL_UNPACK_ROW_LENGTH, 0},
                                       {GL_UNPACK_SKIP_ROWS, 0},  {GL_UNPACK_SKIP_PIXELS, 0}, {GL_UNPACK_ALIGNMENT, 4}};
    for (unsigned i = 0; i < sizeof(unpack) / sizeof(unpack[0]); ++i) gl.PixelStorei(unpack[i][0], (GLint)unpack[i][1]);
    if (context != objects_context || !gl.IsProgram(program) || !gl.IsTexture(font_texture)) {
        if (!makeObjects()) {
            gl.PopClientAttrib();
            gl.PopAttrib();
            gl.UseProgram((GLuint)saved_program);
            gl.BindFramebuffer(GL_FRAMEBUFFER, (GLuint)saved_framebuffer);
            gl_state = -1;
            return false;
        }
        objects_context = context;
    }
    view_width = width;
    view_height = height;
    gl.Viewport(0, 0, (GLsizei)width, (GLsizei)height);
    const GLenum off[] = {GL_DEPTH_TEST, GL_CULL_FACE, GL_SCISSOR_TEST, GL_STENCIL_TEST};
    for (unsigned i = 0; i < sizeof(off) / sizeof(off[0]); ++i) gl.Disable(off[i]);
    gl.ColorMask(1, 1, 1, 1);
    gl.PolygonMode(GL_FRONT_AND_BACK, GL_FILL);
    gl.Enable(GL_BLEND);
    gl.BlendEquation(GL_FUNC_ADD);
    gl.BlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    gl.UseProgram(program);
    gl.Uniform1i(uniform_texture, 0);
    gl.Uniform2f(uniform_view, OVERLAY_WIDTH, OVERLAY_HEIGHT);
    for (GLuint i = 0; i < 16; ++i) gl.DisableVertexAttribArray(i);  // the game's arrays (pushed above) stay out of our draws
    gl.EnableVertexAttribArray(0);
    gl.EnableVertexAttribArray(1);
    gl.EnableVertexAttribArray(2);
    vertex_count = 0;
    batch_texture = font_texture;
    return true;
}
static void flush(void) {
    if (!vertex_count) return;
    gl.BindTexture(GL_TEXTURE_2D, batch_texture);
    gl.VertexAttribPointer(0, 2, GL_FLOAT, 0, sizeof(Vertex), &vertices[0].x);
    gl.VertexAttribPointer(1, 2, GL_FLOAT, 0, sizeof(Vertex), &vertices[0].u);
    gl.VertexAttribPointer(2, 4, GL_UNSIGNED_BYTE, 1, sizeof(Vertex), vertices[0].rgba);
    gl.DrawArrays(GL_TRIANGLES, 0, (GLsizei)vertex_count);
    vertex_count = 0;
}
void overlayEnd(void) {
    flush();
    static unsigned reported;
    GLenum error = gl.GetError();
    if (error && reported++ < 5) diagnosticsTrace("overlay: gl_error=0x%x", error);
    gl.PopClientAttrib();
    gl.PopAttrib();
    gl.ActiveTexture((GLenum)saved_active);
    gl.BindBuffer(GL_ARRAY_BUFFER, (GLuint)saved_array_buffer);
    gl.BindBuffer(GL_PIXEL_UNPACK_BUFFER, (GLuint)saved_unpack);
    gl.UseProgram((GLuint)saved_program);
    gl.BindFramebuffer(GL_FRAMEBUFFER, (GLuint)saved_framebuffer);
}

static void quad(GLuint texture, float x0, float y0, float x1, float y1, float u0, float v0, float u1, float v1, uint32_t rgba) {
    if (texture != batch_texture || vertex_count + 6 > MAX_VERTICES) {
        flush();
        batch_texture = texture;
    }
    const uint8_t c[4] = {(uint8_t)(rgba >> 24), (uint8_t)(rgba >> 16), (uint8_t)(rgba >> 8), (uint8_t)rgba};
    const float corners[6][4] = {{x0, y0, u0, v0}, {x1, y0, u1, v0}, {x1, y1, u1, v1}, {x0, y0, u0, v0}, {x1, y1, u1, v1}, {x0, y1, u0, v1}};
    for (unsigned i = 0; i < 6; ++i) {
        Vertex *v = &vertices[vertex_count++];
        v->x = corners[i][0];
        v->y = corners[i][1];
        v->u = corners[i][2];
        v->v = corners[i][3];
        memcpy(v->rgba, c, 4);
    }
}
void overlayClear(uint32_t rgba) {
    flush();
    gl.ClearColor((float)(rgba >> 24) / 255.0f, (float)((rgba >> 16) & 255) / 255.0f, (float)((rgba >> 8) & 255) / 255.0f, (float)(rgba & 255) / 255.0f);
    gl.Clear(GL_COLOR_BUFFER_BIT);
}
void overlayRect(float x, float y, float w, float h, uint32_t rgba) { quad(font_texture, x, y, x + w, y + h, white_u, white_v, white_u, white_v, rgba); }
void overlayFrame(float x, float y, float w, float h, float t, uint32_t rgba) {
    overlayRect(x, y, w, t, rgba);
    overlayRect(x, y + h - t, w, t, rgba);
    overlayRect(x, y + t, t, h - 2 * t, rgba);
    overlayRect(x + w - t, y + t, t, h - 2 * t, rgba);
}
static const OverlayGlyph *glyph(unsigned char code) {
    const OverlayGlyph *g = code < 128 ? &overlay_glyphs[code] : NULL;
    return g && g->width ? g : &overlay_glyphs['?'];
}
float overlayTextWidth(const char *text, float size) {
    float scale = size / (float)overlay_font_line, width = 0;
    for (const unsigned char *p = (const unsigned char *)text; *p; ++p) width += glyph(*p)->advance * scale;
    return width;
}
float overlayText(float x, float y, const char *text, float size, uint32_t rgba) {
    float scale = size / (float)overlay_font_line, pen = x;
    float u_scale = 1.0f / (float)overlay_font_width, v_scale = 1.0f / (float)overlay_font_height;
    for (const unsigned char *p = (const unsigned char *)text; *p; ++p) {
        const OverlayGlyph *g = glyph(*p);
        if (*p != ' ') {
            float x0 = pen - g->bearing * scale;
            quad(font_texture, x0, y, x0 + (float)g->width * scale, y + (float)g->height * scale, (float)g->x * u_scale, (float)g->y * v_scale,
                 (float)(g->x + g->width) * u_scale, (float)(g->y + g->height) * v_scale, rgba);
        }
        pen += g->advance * scale;
    }
    return pen - x;
}
float overlayTextCentered(float centre_x, float y, const char *text, float size, uint32_t rgba) {
    return overlayText(centre_x - overlayTextWidth(text, size) / 2, y, text, size, rgba);
}
void overlayTextFit(float x, float y, const char *text, float size, float max_width, uint32_t rgba) {
    if (overlayTextWidth(text, size) <= max_width) {
        overlayText(x, y, text, size, rgba);
        return;
    }
    char line[512];
    size_t length = strlen(text);
    if (length > sizeof(line) - 4) length = sizeof(line) - 4;
    memcpy(line, text, length);
    for (; length; --length) {
        memcpy(line + length, "...", 4);
        if (overlayTextWidth(line, size) <= max_width) break;
    }
    overlayText(x, y, length ? line : "...", size, rgba);
}
void overlayLogo(float x, float y, float height) {
    float width = height * (float)overlay_logo_width / (float)overlay_logo_height;
    quad(logo_texture, x, y, x + width, y + height, 0, 0, 1, 1, 0xFFFFFFFFu);
}

// Whether the picture drawn so far shows anything: a few pixels are read back, so call it seldom.
bool overlayPictureHasContent(unsigned width, unsigned height) {
    if (gl_state <= 0 || eglGetCurrentContext() == EGL_NO_CONTEXT) return false;
    GLint pack_buffer = 0, framebuffer = 0;
    gl.GetIntegerv(GL_PIXEL_PACK_BUFFER_BINDING, &pack_buffer);
    gl.GetIntegerv(GL_FRAMEBUFFER_BINDING, &framebuffer);
    gl.PushClientAttrib(GL_CLIENT_ALL_ATTRIB_BITS);
    gl.BindBuffer(GL_PIXEL_PACK_BUFFER, 0);
    gl.BindFramebuffer(GL_FRAMEBUFFER, 0);
    gl.PixelStorei(GL_PACK_ALIGNMENT, 4);
    gl.PixelStorei(GL_PACK_ROW_LENGTH, 0);
    static const float points[][2] = {{0.5f, 0.5f}, {0.25f, 0.25f}, {0.75f, 0.25f}, {0.25f, 0.75f}, {0.75f, 0.75f}, {0.5f, 0.15f}, {0.5f, 0.85f}};
    bool content = false;
    for (unsigned i = 0; i < sizeof(points) / sizeof(points[0]) && !content; ++i) {
        uint8_t pixel[4] = {0, 0, 0, 0};
        gl.ReadPixels((GLint)(points[i][0] * (float)width), (GLint)(points[i][1] * (float)height), 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixel);
        content = pixel[0] + pixel[1] + pixel[2] > 48;
    }
    gl.PopClientAttrib();
    gl.BindBuffer(GL_PIXEL_PACK_BUFFER, (GLuint)pack_buffer);
    gl.BindFramebuffer(GL_FRAMEBUFFER, (GLuint)framebuffer);
    return content;
}

// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 PokeMMO-Prospero contributors
//
// Overlay drawing (see overlay.h). The state handling follows the file chooser and cursor adapted from PokeMMO-NX: the
// game's program and framebuffer are noted, everything else is pushed with glPushAttrib/glPushClientAttrib. Textures belong
// to a context: they are made again when the current context changes (the loading screen's context is gone once the game
// has the display).
#include "overlay.h"
#include "overlay_assets.h"
#include <EGL/egl.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>

typedef unsigned int GLenum, GLbitfield, GLuint;
typedef int GLint, GLsizei;
typedef float GLfloat;
typedef double GLdouble;
enum {
    GL_QUADS = 0x0007,
    GL_TEXTURE_2D = 0x0DE1,
    GL_BLEND = 0x0BE2,
    GL_SRC_ALPHA = 0x0302,
    GL_ONE_MINUS_SRC_ALPHA = 0x0303,
    GL_MODELVIEW = 0x1700,
    GL_PROJECTION = 0x1701,
    GL_TEXTURE = 0x1702,
    GL_CURRENT_PROGRAM = 0x8B8D,
    GL_FRAMEBUFFER = 0x8D40,
    GL_FRAMEBUFFER_BINDING = 0x8CA6,
    GL_TEXTURE0 = 0x84C0,
    GL_RGBA = 0x1908,
    GL_UNSIGNED_BYTE = 0x1401,
    GL_TEXTURE_MIN_FILTER = 0x2801,
    GL_TEXTURE_MAG_FILTER = 0x2800,
    GL_TEXTURE_WRAP_S = 0x2802,
    GL_TEXTURE_WRAP_T = 0x2803,
    GL_LINEAR = 0x2601,
    GL_CLAMP_TO_EDGE = 0x812F,
    GL_TEXTURE_ENV = 0x2300,
    GL_TEXTURE_ENV_MODE = 0x2200,
    GL_MODULATE = 0x2100,
    GL_PIXEL_UNPACK_BUFFER = 0x88EC,
    GL_PIXEL_UNPACK_BUFFER_BINDING = 0x88EF,
    GL_SAMPLER_BINDING = 0x8919,
    GL_UNPACK_ALIGNMENT = 0x0CF5,
    GL_UNPACK_ROW_LENGTH = 0x0CF2,
    GL_VERTEX_ARRAY_BINDING = 0x85B5,
    GL_DEPTH_TEST = 0x0B71,
    GL_CULL_FACE = 0x0B44,
    GL_SCISSOR_TEST = 0x0C11,
    GL_STENCIL_TEST = 0x0B90,
    GL_LIGHTING = 0x0B50,
    GL_ALPHA_TEST = 0x0BC0,
    GL_FOG = 0x0B60,
    GL_FRAMEBUFFER_SRGB = 0x8DB9,
    GL_COLOR_BUFFER_BIT = 0x4000,
};
static struct {
    void (*Begin)(GLenum);
    void (*End)(void);
    void (*Vertex2f)(GLfloat, GLfloat);
    void (*TexCoord2f)(GLfloat, GLfloat);
    void (*Color4f)(GLfloat, GLfloat, GLfloat, GLfloat);
    void (*Ortho)(GLdouble, GLdouble, GLdouble, GLdouble, GLdouble, GLdouble);
    void (*PushAttrib)(GLbitfield);
    void (*PopAttrib)(void);
    void (*PushClientAttrib)(GLbitfield);
    void (*PopClientAttrib)(void);
    void (*MatrixMode)(GLenum);
    void (*PushMatrix)(void);
    void (*PopMatrix)(void);
    void (*LoadIdentity)(void);
    void (*UseProgram)(GLuint);
    void (*GetIntegerv)(GLenum, GLint *);
    void (*ActiveTexture)(GLenum);
    void (*BindTexture)(GLenum, GLuint);
    void (*GenTextures)(GLsizei, GLuint *);
    void (*TexImage2D)(GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum, const void *);
    void (*TexParameteri)(GLenum, GLenum, GLint);
    void (*TexEnvi)(GLenum, GLenum, GLint);
    void (*PixelStorei)(GLenum, GLint);
    void (*BindBuffer)(GLenum, GLuint);
    void (*BindSampler)(GLuint, GLuint);
    void (*BindVertexArray)(GLuint);
    void (*BlendFunc)(GLenum, GLenum);
    void (*Viewport)(GLint, GLint, GLsizei, GLsizei);
    void (*BindFramebuffer)(GLenum, GLuint);
    void (*Enable)(GLenum);
    void (*Disable)(GLenum);
    void (*ClearColor)(GLfloat, GLfloat, GLfloat, GLfloat);
    void (*Clear)(GLbitfield);
} gl;
static int gl_state;  // 0 not tried, 1 ready, -1 unusable
static EGLContext texture_context;
static GLuint font_texture, logo_texture;
static uint32_t *font_pixels, *logo_pixels;
static GLint saved_program, saved_framebuffer, saved_unpack, saved_sampler, saved_vao;

static bool loadFunction(void *slot, const char *name) {
    void *address = (void *)eglGetProcAddress(name);
    if (address) memcpy(slot, &address, sizeof(address));
    return address != NULL;
}
static bool loadGl(void) {
#define LOAD(field)                                                                                                                                            \
    if (!loadFunction(&gl.field, "gl" #field)) return false
    LOAD(Begin);
    LOAD(End);
    LOAD(Vertex2f);
    LOAD(TexCoord2f);
    LOAD(Color4f);
    LOAD(Ortho);
    LOAD(PushAttrib);
    LOAD(PopAttrib);
    LOAD(PushClientAttrib);
    LOAD(PopClientAttrib);
    LOAD(MatrixMode);
    LOAD(PushMatrix);
    LOAD(PopMatrix);
    LOAD(LoadIdentity);
    LOAD(UseProgram);
    LOAD(GetIntegerv);
    LOAD(ActiveTexture);
    LOAD(BindTexture);
    LOAD(GenTextures);
    LOAD(TexImage2D);
    LOAD(TexParameteri);
    LOAD(TexEnvi);
    LOAD(PixelStorei);
    LOAD(BlendFunc);
    LOAD(Viewport);
    LOAD(BindFramebuffer);
    LOAD(Enable);
    LOAD(Disable);
    LOAD(ClearColor);
    LOAD(Clear);
#undef LOAD
    // Newer entry points: used when the context has them.
    loadFunction(&gl.BindBuffer, "glBindBuffer");
    loadFunction(&gl.BindSampler, "glBindSampler");
    loadFunction(&gl.BindVertexArray, "glBindVertexArray");
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

bool overlayBegin(unsigned width, unsigned height) {
    if (!gl_state) gl_state = loadGl() && inflateAssets() ? 1 : -1;
    if (gl_state < 0 || eglGetCurrentContext() == EGL_NO_CONTEXT) return false;
    saved_program = saved_framebuffer = saved_unpack = saved_sampler = saved_vao = 0;
    gl.GetIntegerv(GL_CURRENT_PROGRAM, &saved_program);
    gl.GetIntegerv(GL_FRAMEBUFFER_BINDING, &saved_framebuffer);
    gl.GetIntegerv(GL_PIXEL_UNPACK_BUFFER_BINDING, &saved_unpack);
    gl.GetIntegerv(GL_VERTEX_ARRAY_BINDING, &saved_vao);
    gl.ActiveTexture(GL_TEXTURE0);
    gl.GetIntegerv(GL_SAMPLER_BINDING, &saved_sampler);
    gl.PushAttrib(0x000FFFFFu);
    gl.PushClientAttrib(0xFFFFFFFFu);
    gl.UseProgram(0);
    gl.BindFramebuffer(GL_FRAMEBUFFER, 0);
    if (gl.BindBuffer) gl.BindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
    if (gl.BindSampler) gl.BindSampler(0, 0);
    if (gl.BindVertexArray) gl.BindVertexArray(0);
    gl.PixelStorei(GL_UNPACK_ALIGNMENT, 4);
    gl.PixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    gl.Viewport(0, 0, (GLsizei)width, (GLsizei)height);
    const GLenum off[] = {GL_DEPTH_TEST, GL_CULL_FACE, GL_SCISSOR_TEST, GL_STENCIL_TEST, GL_LIGHTING, GL_ALPHA_TEST, GL_FOG, GL_FRAMEBUFFER_SRGB};
    for (unsigned i = 0; i < sizeof(off) / sizeof(off[0]); ++i) gl.Disable(off[i]);
    gl.Enable(GL_BLEND);
    gl.BlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    gl.TexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
    gl.MatrixMode(GL_TEXTURE);
    gl.PushMatrix();
    gl.LoadIdentity();
    gl.MatrixMode(GL_PROJECTION);
    gl.PushMatrix();
    gl.LoadIdentity();
    gl.Ortho(0, OVERLAY_WIDTH, OVERLAY_HEIGHT, 0, -1, 1);
    gl.MatrixMode(GL_MODELVIEW);
    gl.PushMatrix();
    gl.LoadIdentity();
    EGLContext context = eglGetCurrentContext();
    if (context != texture_context) {
        font_texture = upload(font_pixels, overlay_font_width, overlay_font_height);
        logo_texture = upload(logo_pixels, overlay_logo_width, overlay_logo_height);
        texture_context = context;
    }
    return true;
}
void overlayEnd(void) {
    gl.MatrixMode(GL_MODELVIEW);
    gl.PopMatrix();
    gl.MatrixMode(GL_PROJECTION);
    gl.PopMatrix();
    gl.MatrixMode(GL_TEXTURE);
    gl.PopMatrix();
    gl.PopClientAttrib();
    gl.PopAttrib();
    if (gl.BindVertexArray) gl.BindVertexArray((GLuint)saved_vao);
    if (gl.BindSampler) gl.BindSampler(0, (GLuint)saved_sampler);
    if (gl.BindBuffer) gl.BindBuffer(GL_PIXEL_UNPACK_BUFFER, (GLuint)saved_unpack);
    gl.UseProgram((GLuint)saved_program);
    gl.BindFramebuffer(GL_FRAMEBUFFER, (GLuint)saved_framebuffer);
}

static void color(uint32_t rgba) {
    gl.Color4f((float)(rgba >> 24) / 255.0f, (float)((rgba >> 16) & 255) / 255.0f, (float)((rgba >> 8) & 255) / 255.0f, (float)(rgba & 255) / 255.0f);
}
void overlayClear(uint32_t rgba) {
    gl.ClearColor((float)(rgba >> 24) / 255.0f, (float)((rgba >> 16) & 255) / 255.0f, (float)((rgba >> 8) & 255) / 255.0f, (float)(rgba & 255) / 255.0f);
    gl.Clear(GL_COLOR_BUFFER_BIT);
}
void overlayRect(float x, float y, float w, float h, uint32_t rgba) {
    gl.Disable(GL_TEXTURE_2D);
    color(rgba);
    gl.Begin(GL_QUADS);
    gl.Vertex2f(x, y);
    gl.Vertex2f(x + w, y);
    gl.Vertex2f(x + w, y + h);
    gl.Vertex2f(x, y + h);
    gl.End();
}
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
    gl.Enable(GL_TEXTURE_2D);
    gl.BindTexture(GL_TEXTURE_2D, font_texture);
    color(rgba);
    gl.Begin(GL_QUADS);
    for (const unsigned char *p = (const unsigned char *)text; *p; ++p) {
        const OverlayGlyph *g = glyph(*p);
        if (*p != ' ') {
            float x0 = pen - g->bearing * scale, x1 = x0 + (float)g->width * scale, y1 = y + (float)g->height * scale;
            float u0 = (float)g->x * u_scale, u1 = (float)(g->x + g->width) * u_scale, v0 = (float)g->y * v_scale, v1 = (float)(g->y + g->height) * v_scale;
            gl.TexCoord2f(u0, v0);
            gl.Vertex2f(x0, y);
            gl.TexCoord2f(u1, v0);
            gl.Vertex2f(x1, y);
            gl.TexCoord2f(u1, v1);
            gl.Vertex2f(x1, y1);
            gl.TexCoord2f(u0, v1);
            gl.Vertex2f(x0, y1);
        }
        pen += g->advance * scale;
    }
    gl.End();
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
    gl.Enable(GL_TEXTURE_2D);
    gl.BindTexture(GL_TEXTURE_2D, logo_texture);
    gl.Color4f(1, 1, 1, 1);
    gl.Begin(GL_QUADS);
    gl.TexCoord2f(0, 0);
    gl.Vertex2f(x, y);
    gl.TexCoord2f(1, 0);
    gl.Vertex2f(x + width, y);
    gl.TexCoord2f(1, 1);
    gl.Vertex2f(x + width, y + height);
    gl.TexCoord2f(0, 1);
    gl.Vertex2f(x, y + height);
    gl.End();
}

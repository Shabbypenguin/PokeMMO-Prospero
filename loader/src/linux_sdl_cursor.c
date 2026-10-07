// SPDX-License-Identifier: MIT AND GPL-3.0-or-later
// Adapted from PokeMMO-NX (https://github.com/Petit-Prince-dev/PokeMMO-NX, source/linux_sdl_cursor.c)
// Copyright (c) Petit_Prince, MIT license (LICENSES/PokeMMO-NX-MIT.txt). PS5 changes: PokeMMO-Prospero contributors.
// Changes: PS5: the controller's touchpad is a trackpad (loader-27), on whether the stick cursor is on or not; the cursor is
// drawn while either is in use.
#include "linux_sdl_cursor.h"
#include "diagnostics.h"
#include <EGL/egl.h>
#include <math.h>
#include <stdatomic.h>
#include <string.h>

#define LEFT_STICK_BUTTON 7  // SDL_GAMEPAD_BUTTON_LEFT_STICK
#define DEAD_ZONE 0.15f
#define TRIGGER_PRESSED 16000
#define SCREENS_PER_SECOND 1.2f  // speed of the cursor, in screen heights, with the stick at its limit; slower below, by the square
#define LONGEST_STEP_SECONDS 0.1f

static bool active, toggle_before;
static float cursor_x, cursor_y;
static bool placed;  // the cursor has a position (the stick cursor or the trackpad was used)
static unsigned last_width, last_height;
static uint64_t last_ns;
static atomic_bool visible;
static atomic_int shown_x, shown_y;

static float limit(float value, float most) { return value < 0 ? 0 : (value > most ? most : value); }
static void place(unsigned width, unsigned height) {
    if (placed) return;
    placed = true;
    linuxSdlEventsMouse(&cursor_x, &cursor_y);  // where the mouse last was (the middle of the screen at the start)
    if (cursor_x == 0 && cursor_y == 0) {
        cursor_x = (float)width / 2;
        cursor_y = (float)height / 2;
    }
}

// ---- PS5: the touchpad as a laptop trackpad (loader-27) ---------------------------------------------------------------------------
// A finger that slides moves the cursor from where it is (faster swipes go further). A short tap that stays in place is a left
// click, a tap with two fingers a right click. A tap and then a finger that lands again at once and slides drags (the left button
// is held until it lifts). Pressing the touchpad down stays the game's button (the bag): a touch with a press is not a tap.
// The cursor is drawn while the trackpad is in use and hides a few seconds after.
#define PAD_UNITS_PER_SCREEN 1540.0f  // touchpad units (0..1919 x 0..1079) per screen height at slow speed (loader-30: 30% slower than
                                      // loader-27, where the pad spanned the screen once)
#define PAD_FAST_SPEED 2500.0f        // touchpad units per second from which a swipe goes the furthest
#define PAD_FAST_GAIN 2.2f            // how much further, at that speed
#define PAD_TAP_NS 250000000ull       // longest touch that is a tap
#define PAD_TAP_SLOP 40.0f            // touchpad units a tap may wander
#define PAD_DRAG_NS 300000000ull      // after a tap, a finger landing within this long and sliding drags
#define PAD_CLICK_NS 50000000ull      // a click is held this long, so that the game sees it whether it reads events or button state
#define PAD_HIDE_NS 4000000000ull     // the cursor hides this long after the trackpad was last touched
#define TOUCHPAD_BUTTON 4             // SDL_GAMEPAD_BUTTON_BACK: the touchpad press

static struct {
    bool used;            // ever: from then on the game has the cursor's mouse
    bool touching, pressed, may_drag, dragging;
    int finger;           // id of the finger that moves the cursor
    float last_x, last_y, moved;
    unsigned most_fingers;
    uint64_t start_ns, last_ns, tap_ns, touched_ns, left_until_ns, right_until_ns;
} pad;

static void trackpadUpdate(LinuxInputSnapshot *snapshot, unsigned width, unsigned height) {
    uint64_t now = snapshot->timestamp_ns;
    unsigned fingers = snapshot->pad_touches > 2 ? 2 : snapshot->pad_touches;
    if (fingers) {
        place(width, height);
        pad.used = true;
        pad.touched_ns = now;
        int which = -1;
        for (unsigned i = 0; i < fingers; ++i)
            if (pad.touching && snapshot->pad_touch[i].id == pad.finger) which = (int)i;
        if (!pad.touching) {  // a new touch
            pad.touching = true;
            pad.pressed = false;
            pad.moved = 0;
            pad.most_fingers = fingers;
            pad.start_ns = now;
            pad.may_drag = pad.tap_ns && now - pad.tap_ns < PAD_DRAG_NS;
        }
        if (which < 0) {  // the first finger, or the one that moved the cursor lifted and another stayed: carry on from here, no jump
            which = 0;
            pad.finger = snapshot->pad_touch[0].id;
            pad.last_x = snapshot->pad_touch[0].x;
            pad.last_y = snapshot->pad_touch[0].y;
            pad.last_ns = now;
        }
        if (fingers > pad.most_fingers) pad.most_fingers = fingers;
        if ((snapshot->buttons >> TOUCHPAD_BUTTON) & 1u) pad.pressed = true;
        float dx = snapshot->pad_touch[which].x - pad.last_x, dy = snapshot->pad_touch[which].y - pad.last_y;
        float distance = sqrtf(dx * dx + dy * dy);
        float seconds = now > pad.last_ns ? (float)(now - pad.last_ns) * 1e-9f : 0;
        pad.last_x = snapshot->pad_touch[which].x;
        pad.last_y = snapshot->pad_touch[which].y;
        pad.last_ns = now;
        pad.moved += distance;
        if (distance > 0 && pad.most_fingers == 1 && !pad.pressed) {  // two fingers (a right tap) and presses do not move the cursor
            float speed = seconds > 0 ? distance / seconds : 0;
            float gain = 1.0f + (PAD_FAST_GAIN - 1.0f) * limit(speed / PAD_FAST_SPEED, 1.0f);
            float scale = gain * (float)height / PAD_UNITS_PER_SCREEN;
            cursor_x += dx * scale;
            cursor_y += dy * scale;
        }
        if (pad.may_drag && !pad.dragging && !pad.pressed && pad.most_fingers == 1 && pad.moved > PAD_TAP_SLOP) {
            pad.dragging = true;
            diagnosticsTrace("sdl.trackpad drag");
        }
    } else if (pad.touching) {  // lifted
        pad.touching = false;
        bool tap = !pad.dragging && !pad.pressed && now - pad.start_ns <= PAD_TAP_NS && pad.moved <= PAD_TAP_SLOP;
        pad.dragging = false;
        pad.tap_ns = 0;
        if (tap) {
            if (pad.most_fingers >= 2)
                pad.right_until_ns = now + PAD_CLICK_NS;
            else {
                pad.left_until_ns = now + PAD_CLICK_NS;
                pad.tap_ns = now;
            }
        }
    }
}

void linuxSdlCursorUpdate(LinuxInputSnapshot *snapshot, unsigned width, unsigned height) {
    bool toggle = (snapshot->buttons >> LEFT_STICK_BUTTON) & 1u;
    snapshot->buttons &= ~(1u << LEFT_STICK_BUTTON);  // the cursor's button is not also a game button
    if (toggle && !toggle_before) {
        active = !active;
        if (active) {
            place(width, height);
            last_ns = 0;
        }
        diagnosticsTrace("sdl.cursor=%s", active ? "ON" : "OFF");
    }
    toggle_before = toggle;
    if (last_width && (width != last_width || height != last_height)) {  // docked or undocked: the cursor stays at the same place of the screen
        cursor_x = cursor_x * (float)width / (float)last_width;
        cursor_y = cursor_y * (float)height / (float)last_height;
    }
    last_width = width;
    last_height = height;
    trackpadUpdate(snapshot, width, height);
    uint64_t now = snapshot->timestamp_ns;
    bool pad_left = pad.dragging || now < pad.left_until_ns, pad_right = now < pad.right_until_ns;
    bool pad_shown = pad.used && (pad.touching || now - pad.touched_ns < PAD_HIDE_NS);
    atomic_store(&visible, active || pad_shown);
    if (!active && !pad.used) return;
    if (active) {
        float seconds = last_ns && now > last_ns ? (float)(now - last_ns) * 1e-9f : 0;
        if (seconds > LONGEST_STEP_SECONDS) seconds = LONGEST_STEP_SECONDS;
        last_ns = now;
        float stick_x = (float)snapshot->axes[0] / 32767.0f, stick_y = (float)snapshot->axes[1] / 32767.0f;
        float tilt = sqrtf(stick_x * stick_x + stick_y * stick_y);
        if (tilt > DEAD_ZONE) {
            float strength = limit((tilt - DEAD_ZONE) / (1.0f - DEAD_ZONE), 1.0f);
            float distance = strength * strength * SCREENS_PER_SECOND * (float)height * seconds;
            cursor_x += stick_x / tilt * distance;
            cursor_y += stick_y / tilt * distance;
        }
    }
    cursor_x = limit(cursor_x, (float)width - 1);
    cursor_y = limit(cursor_y, (float)height - 1);
    atomic_store(&shown_x, (int)cursor_x);
    atomic_store(&shown_y, (int)cursor_y);
    snapshot->pointer = true;
    snapshot->pointer_x = cursor_x;
    snapshot->pointer_y = cursor_y;
    snapshot->pointer_left = pad_left || (active && snapshot->axes[5] > TRIGGER_PRESSED);   // R2
    snapshot->pointer_right = pad_right || (active && snapshot->axes[4] > TRIGGER_PRESSED);  // L2
    if (active) {  // the stick cursor has the controller: the game sees it idle
        snapshot->buttons = 0;
        memset(snapshot->axes, 0, sizeof(snapshot->axes));
    }
}

// ---- drawing: the same legacy OpenGL sandbox as the file chooser (state saved and restored around it) --------------------------------
typedef unsigned int GLenum, GLuint, GLbitfield;
typedef int GLint, GLsizei;
typedef float GLfloat;
typedef double GLdouble;
enum {
    GL_TRIANGLES = 4,
    GL_QUADS = 7,
    GL_TEXTURE_2D = 0x0DE1,
    GL_BLEND = 0x0BE2,
    GL_SRC_ALPHA = 0x0302,
    GL_ONE_MINUS_SRC_ALPHA = 0x0303,
    GL_PROJECTION = 0x1701,
    GL_MODELVIEW = 0x1700,
    GL_TEXTURE = 0x1702,
    GL_TEXTURE0 = 0x84C0,
    GL_CURRENT_PROGRAM = 0x8B8D,
    GL_FRAMEBUFFER = 0x8D40,
    GL_FRAMEBUFFER_BINDING = 0x8CA6,
    GL_DEPTH_TEST = 0x0B71,
    GL_CULL_FACE = 0x0B44,
    GL_SCISSOR_TEST = 0x0C11,
    GL_STENCIL_TEST = 0x0B90,
    GL_LIGHTING = 0x0B50,
    GL_ALPHA_TEST = 0x0BC0,
    GL_FOG = 0x0B60
};
static struct {
    void (*Begin)(GLenum);
    void (*End)(void);
    void (*Vertex2f)(GLfloat, GLfloat);
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
    void (*BlendFunc)(GLenum, GLenum);
    void (*Viewport)(GLint, GLint, GLsizei, GLsizei);
    void (*BindFramebuffer)(GLenum, GLuint);
    void (*Enable)(GLenum);
    void (*Disable)(GLenum);
} gl;
static bool gl_ready, draw_failed;

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
    LOAD(BlendFunc);
    LOAD(Viewport);
    LOAD(BindFramebuffer);
    LOAD(Enable);
    LOAD(Disable);
#undef LOAD
    return true;
}
// The arrow, in units of a 12 x 19 pointer whose tip is the origin: two triangles for the head and a quadrilateral for the tail.
static void arrow(float x, float y, float unit, float shift_x, float shift_y) {
    static const float head_a[3][2] = {{0, 0}, {0, 16}, {4, 12}}, head_b[3][2] = {{0, 0}, {4, 12}, {11, 11}}, tail[4][2] = {{4, 12}, {7, 19}, {9, 18}, {6, 11}};
    x += shift_x * unit;
    y += shift_y * unit;
    gl.Begin(GL_TRIANGLES);
    for (unsigned i = 0; i < 3; ++i) gl.Vertex2f(x + head_a[i][0] * unit, y + head_a[i][1] * unit);
    for (unsigned i = 0; i < 3; ++i) gl.Vertex2f(x + head_b[i][0] * unit, y + head_b[i][1] * unit);
    gl.End();
    gl.Begin(GL_QUADS);
    for (unsigned i = 0; i < 4; ++i) gl.Vertex2f(x + tail[i][0] * unit, y + tail[i][1] * unit);
    gl.End();
}

void linuxSdlCursorDraw(unsigned width, unsigned height) {
    if (!atomic_load(&visible) || draw_failed || eglGetCurrentContext() == EGL_NO_CONTEXT) return;
    if (!gl_ready) {
        if (!loadGl()) {
            draw_failed = true;
            diagnosticsTrace("sdl.cursor.draw=FAIL (OpenGL functions)");
            return;
        }
        gl_ready = true;
    }
    GLint program = 0, framebuffer = 0;
    gl.GetIntegerv(GL_CURRENT_PROGRAM, &program);
    gl.GetIntegerv(GL_FRAMEBUFFER_BINDING, &framebuffer);
    gl.PushAttrib(0x000FFFFFu);
    gl.PushClientAttrib(0xFFFFFFFFu);
    gl.UseProgram(0);
    gl.BindFramebuffer(GL_FRAMEBUFFER, 0);
    gl.Viewport(0, 0, (GLsizei)width, (GLsizei)height);
    const GLenum off[] = {GL_DEPTH_TEST, GL_CULL_FACE, GL_SCISSOR_TEST, GL_STENCIL_TEST, GL_LIGHTING, GL_ALPHA_TEST, GL_FOG, GL_TEXTURE_2D};
    gl.ActiveTexture(GL_TEXTURE0);
    for (unsigned i = 0; i < sizeof(off) / sizeof(off[0]); ++i) gl.Disable(off[i]);
    gl.Enable(GL_BLEND);
    gl.BlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    gl.MatrixMode(GL_TEXTURE);
    gl.PushMatrix();
    gl.LoadIdentity();
    gl.MatrixMode(GL_PROJECTION);
    gl.PushMatrix();
    gl.LoadIdentity();
    gl.Ortho(0, (GLdouble)width, (GLdouble)height, 0, -1, 1);
    gl.MatrixMode(GL_MODELVIEW);
    gl.PushMatrix();
    gl.LoadIdentity();
    float x = (float)atomic_load(&shown_x), y = (float)atomic_load(&shown_y), unit = (float)height / 540.0f;
    static const float outline[4][2] = {{-1, 0}, {1, 0}, {0, -1}, {0, 1}};
    gl.Color4f(0, 0, 0, 1);
    for (unsigned i = 0; i < 4; ++i) arrow(x, y, unit, outline[i][0], outline[i][1]);
    gl.Color4f(1, 1, 1, 1);
    arrow(x, y, unit, 0, 0);
    gl.MatrixMode(GL_MODELVIEW);
    gl.PopMatrix();
    gl.MatrixMode(GL_PROJECTION);
    gl.PopMatrix();
    gl.MatrixMode(GL_TEXTURE);
    gl.PopMatrix();
    gl.PopClientAttrib();
    gl.PopAttrib();
    gl.UseProgram((GLuint)program);
    gl.BindFramebuffer(GL_FRAMEBUFFER, (GLuint)framebuffer);
}

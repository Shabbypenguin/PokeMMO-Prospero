// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 PokeMMO-Prospero contributors
//
// Draws the loader's overlays (loading screen, on-screen keyboard, link box) with Mesa on a Linux PC and writes them as PPM
// pictures, and drives the keyboard with pretend controller input to check what it types. `make overlay-preview`.
#include "link_box.h"
#include "loading_screen.h"
#include "osk.h"
#include "overlay.h"
#include <EGL/egl.h>
#define GL_GLEXT_PROTOTYPES 1
#include <GL/gl.h>
#include <GL/glext.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

// ---- what the overlays need from the rest of the loader ---------------------------------------------------------------------------
static char typed[512];
void diagnosticsTrace(const char *format, ...) { (void)format; }
void linuxSdlEventsPushKey(uint32_t scancode, uint32_t keycode, bool down, uint64_t timestamp_ns) {
    (void)keycode;
    (void)timestamp_ns;
    if (!down) return;
    char name[16];
    snprintf(name, sizeof(name), "<%u>", scancode);
    strncat(typed, name, sizeof(typed) - strlen(typed) - 1);
}
unsigned linuxSdlEventsTextChanged(const char *previous, const char *current, uint64_t timestamp_ns) {
    (void)previous;
    (void)timestamp_ns;
    strncat(typed, current, sizeof(typed) - strlen(typed) - 1);
    return 1;
}

static const int W = 1920, H = 1080;
static void save(const char *path) {
    unsigned char *pixels = malloc((size_t)W * H * 3);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, W, H, GL_RGB, GL_UNSIGNED_BYTE, pixels);
    FILE *file = fopen(path, "wb");
    fprintf(file, "P6\n%d %d\n255\n", W, H);
    for (int y = H - 1; y >= 0; --y) fwrite(pixels + (size_t)y * W * 3, 1, (size_t)W * 3, file);
    fclose(file);
    free(pixels);
    printf("wrote %s\n", path);
}
static void gameFrame(void) {  // something busy behind the overlays
    glClearColor(0.85f, 0.88f, 0.92f, 1);
    glClear(GL_COLOR_BUFFER_BIT);
}
static uint64_t now_ns = 1000000000ull;
static void pad(uint32_t buttons) {  // one controller sample, 20 ms apart
    LinuxInputSnapshot s = {0};
    s.gamepad = true;
    s.buttons = buttons;
    s.timestamp_ns = (now_ns += 20000000ull);
    oskFeed(&s);
}
static void tap(uint32_t bit) {
    pad(1u << bit);
    pad(0);
}

int main(int argc, char **argv) {
    const char *out = argc > 1 ? argv[1] : "build/overlay-preview";
    EGLDisplay display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    EGLint count = 0;
    EGLConfig config;
    static const EGLint attributes[] = {EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT, EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8,
                                        EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8, EGL_NONE};
    const EGLint pbuffer[] = {EGL_WIDTH, W, EGL_HEIGHT, H, EGL_NONE};
    if (!eglInitialize(display, NULL, NULL) || !eglBindAPI(EGL_OPENGL_API) || !eglChooseConfig(display, attributes, &config, 1, &count) || !count) {
        fprintf(stderr, "no EGL\n");
        return 1;
    }
    EGLSurface surface = eglCreatePbufferSurface(display, config, pbuffer);
    EGLContext context = eglCreateContext(display, config, EGL_NO_CONTEXT, NULL);
    eglMakeCurrent(display, surface, surface, context);
    char path[512];

    static const LoadingStep steps[] = {{"fs.list.app0", 1}, {"fs.list.roms", 1}, {"fs.romread", 1}, {"sys.modules", 1}, {"net.https", 3},
                                        {"client.install", 4}, {"client.map", 0}, {"client.start", 0}, {"client.end", 0}};
    LoadingView view = {.fraction = 0.35f, .status = "Installing PokeMMO", .detail = "212 of 498 MB", .version = "Prospero loader-8 (v0.8-dev)",
                        .revision = "32920", .log_path = "/data/homebrew/PPSA98001/prospero.log", .steps = steps, .step_count = 9, .frame = 70};
    struct {
        const char *name;
        void (*change)(LoadingView *);
    } screens[] = {{"loading-install", NULL}, {"loading-problem", NULL}, {"loading-details", NULL}, {"loading-update", NULL}};
    for (unsigned i = 0; i < 4; ++i) {
        LoadingView v = view;
        if (i == 1) {
            v.problem = "PokeMMO is not installed yet";
            v.advice = "Run the installer on your computer and pick this console (developer builds: --client PokeMMO-Client.zip).";
            v.warning = "No ROMs found: add them with the installer to play.";
        }
        if (i == 2) v.details = true;
        if (i == 3) {
            v.fraction = 0.08f;
            v.question = "PokeMMO update: revision 32920 to 32951 (92 MB)";
            v.choices = "\x01 Download     \x02 Skip          (downloading in 4)";
        }
        overlayBegin(W, H);
        loadingScreenDraw(&v);
        overlayEnd();
        snprintf(path, sizeof(path), "%s/%s.ppm", out, screens[i].name);
        save(path);
    }

    // The ROM screen over a folder of pretend ROMs (headers only).
    {
        const char *folder = "build/overlay-preview/roms";
        mkdir(folder, 0755);
        static const struct {
            const char *file, *code;
            bool gba;
        } fakes[] = {{"Pokemon - FireRed Version (USA, Europe) (Rev 1).gba", "BPRE", true},
                     {"Pokemon - HeartGold Version (USA).nds", "IPKE", false},
                     {"Pokemon - Black Version 2 (USA, Europe).nds", "IRE?", false},
                     {"Pokemon - Emerald.zip", "", false},
                     {"notes.txt", "", false}};
        for (unsigned i = 0; i < 5; ++i) {
            unsigned char header[0x200] = {0};
            if (fakes[i].code[0]) {
                memcpy(fakes[i].gba ? header + 0xAC : header + 0x0C, fakes[i].code, 4);
                if (fakes[i].code[3] == '?') (fakes[i].gba ? header + 0xAC : header + 0x0C)[3] = 'O';
                if (fakes[i].gba) header[0xB2] = 0x96, header[0xBC] = 1;
            } else
                memcpy(header, "hello", 5);
            char path[512];
            snprintf(path, sizeof(path), "%s/%s", folder, fakes[i].file);
            FILE *f = fopen(path, "wb");
            fwrite(header, 1, sizeof(header), f);
            fclose(f);
        }
        RomScan scan;
        romsScan(folder, &scan);
        for (unsigned i = 0; i < scan.count; ++i) printf("rom: %s -> game %d: %s\n", scan.files[i].file, scan.files[i].game, scan.files[i].note);
        overlayBegin(W, H);
        RomUploadInfo info = {.address = "192.168.1.20", .web = true, .ftp_port = 2121, .folder = "/data/homebrew/PPSA98001/roms/",
                              .receiving = "Receiving Pokemon - Black Version (USA, Europe).nds: 84 of 256 MB"};
        romScreenDraw(&scan, &info, true);
        overlayEnd();
        snprintf(path, sizeof(path), "%s/roms.ppm", out);
        save(path);
    }

    // A game's leftovers that must neither break the overlay nor be lost: a buffer, an enabled array, odd unpack settings.
    GLuint game_buffer, unpack_buffer;
    glGenBuffers(1, &game_buffer);
    glBindBuffer(GL_ARRAY_BUFFER, game_buffer);
    glBufferData(GL_ARRAY_BUFFER, 64, NULL, GL_STATIC_DRAW);
    glEnableVertexAttribArray(3);
    glVertexAttribPointer(3, 4, GL_FLOAT, GL_FALSE, 0, NULL);
    glGenBuffers(1, &unpack_buffer);
    glBindBuffer(GL_PIXEL_UNPACK_BUFFER, unpack_buffer);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 7);
    glEnable(GL_SCISSOR_TEST);
    glScissor(0, 0, 10, 10);

    // The keyboard: type "Ab1!" then Tab, Enter; R2 is an axis, so shift goes through the Shift key.
    oskShow(true);
    pad(0);           // first sample after opening is ignored
    tap(12);          // down to q
    tap(12);          // down to a
    tap(0);           // a
    typed[0] = 0;
    tap(12), tap(12);           // to the bottom row (Shift)
    tap(0);                     // Shift once
    tap(11), tap(11);           // up to the A key
    tap(0);                     // A
    tap(12), tap(14), tap(14), tap(14), tap(14);  // b (row 3, column 4)
    tap(0);
    gameFrame();
    overlayBegin(W, H);
    oskDraw();
    overlayEnd();
    snprintf(path, sizeof(path), "%s/keyboard.ppm", out);
    save(path);
    tap(2);   // Square: delete
    tap(3);   // Triangle: space
    tap(4);   // touchpad: Tab
    tap(6);   // Options: Enter
    printf("typed: %s\n", typed);
    tap(1);   // Circle closes
    printf("keyboard visible after Circle: %d\n", oskVisible());
    GLint value = 0, enabled = 0;
    glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &value);
    glGetVertexAttribiv(3, GL_VERTEX_ATTRIB_ARRAY_ENABLED, &enabled);
    GLint row_length = 0, unpack = 0;
    glGetIntegerv(GL_UNPACK_ROW_LENGTH, &row_length);
    glGetIntegerv(GL_PIXEL_UNPACK_BUFFER_BINDING, &unpack);
    printf("game state kept: array_buffer=%d attrib3=%d row_length=%d unpack=%d scissor=%d\n", value == (GLint)game_buffer, enabled,
           row_length, unpack == (GLint)unpack_buffer, glIsEnabled(GL_SCISSOR_TEST));
    glDisable(GL_SCISSOR_TEST);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    printf("black picture has content: %d\n", overlayPictureHasContent(W, H));
    gameFrame();
    printf("game picture has content: %d\n", overlayPictureHasContent(W, H));

    // A new context in place of the old one (as the game's replaces the loading screen's): objects are made again.
    overlayContextLost();
    eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    eglDestroyContext(display, context);
    context = eglCreateContext(display, config, EGL_NO_CONTEXT, NULL);
    eglMakeCurrent(display, surface, surface, context);

    linkBoxShow("https://pokemmo.com/en/account/forgot_password/");
    gameFrame();
    overlayBegin(W, H);
    linkBoxDraw();
    overlayEnd();
    snprintf(path, sizeof(path), "%s/link.ppm", out);
    save(path);
    return 0;
}

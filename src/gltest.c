/*
 * Quartix - `quartix gltest`: a standalone graphics smoke test.
 * Copyright (C) 2026 Quartix contributors. GPLv3.
 *
 * This drives ANGLE directly on the real GPU, independently of the Roblox
 * engine: it opens a window, brings up an EGL display on ANGLE, picks an
 * ES2 config, creates a window surface and an ES2 context, makes it current,
 * prints GL_VENDOR/GL_RENDERER/GL_VERSION (which name the Direct3D backend),
 * clears a few frames and swaps them. If this works, ANGLE's GLES->Direct3D
 * path is good on this machine, which de-risks wiring the engine to it.
 *
 * Unlike src/gl.c, this file is ordinary native code: it fetches the ANGLE
 * entry points with qx_gl_proc() (which returns the DLL's Microsoft-ABI
 * pointers) and calls them directly. It is never called by the engine, so no
 * System V thunking is involved here.
 */
#include "quartix.h"
#include <stdio.h>

/* --- the slice of EGL/GLES constants this test needs --- */
#define EGL_DEFAULT_DISPLAY   ((void *)0)
#define EGL_NO_DISPLAY        ((void *)0)
#define EGL_NO_CONTEXT        ((void *)0)
#define EGL_NO_SURFACE        ((void *)0)
#define EGL_FALSE             0
#define EGL_NONE              0x3038
#define EGL_SURFACE_TYPE      0x3033
#define EGL_WINDOW_BIT        0x0004
#define EGL_RENDERABLE_TYPE   0x3040
#define EGL_OPENGL_ES2_BIT    0x0004
#define EGL_RED_SIZE          0x3024
#define EGL_GREEN_SIZE        0x3023
#define EGL_BLUE_SIZE         0x3022
#define EGL_ALPHA_SIZE        0x3021
#define EGL_DEPTH_SIZE        0x3025
#define EGL_CONTEXT_CLIENT_VERSION 0x3098

#define GL_VENDOR             0x1F00
#define GL_RENDERER           0x1F01
#define GL_VERSION            0x1F02
#define GL_COLOR_BUFFER_BIT   0x00004000

/* ANGLE entry points, Microsoft ABI (as the DLL exports them). */
typedef void *(*pfn_eglGetDisplay)(void *);
typedef unsigned (*pfn_eglInitialize)(void *, int *, int *);
typedef unsigned (*pfn_eglChooseConfig)(void *, const int *, void **, int, int *);
typedef void *(*pfn_eglCreateWindowSurface)(void *, void *, void *, const int *);
typedef void *(*pfn_eglCreateContext)(void *, void *, void *, const int *);
typedef unsigned (*pfn_eglMakeCurrent)(void *, void *, void *, void *);
typedef unsigned (*pfn_eglSwapBuffers)(void *, void *);
typedef int (*pfn_eglGetError)(void);
typedef const unsigned char *(*pfn_glGetString)(unsigned);
typedef void (*pfn_glClearColor)(float, float, float, float);
typedef void (*pfn_glClear)(unsigned);

static LRESULT CALLBACK gltest_wndproc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    if (m == WM_CLOSE) {
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

static HWND make_window(void)
{
    WNDCLASSW wc;
    HINSTANCE inst = GetModuleHandleW(NULL);
    memset(&wc, 0, sizeof(wc));
    wc.lpfnWndProc = gltest_wndproc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
    wc.lpszClassName = L"QuartixGLTest";
    RegisterClassW(&wc);
    return CreateWindowExW(0, L"QuartixGLTest", L"Quartix - ANGLE smoke test",
                           WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
                           640, 480, NULL, NULL, inst, NULL);
}

static void pump(int frames)
{
    MSG msg;
    int i;
    for (i = 0; i < frames; i++) {
        while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        Sleep(16);
    }
}

#define NEED(var, type, name)                                                  \
    type var = (type)qx_gl_proc(name);                                         \
    if (!var) {                                                                \
        qx_err("gltest: ANGLE is missing the entry point %s", name);           \
        return 1;                                                              \
    }

int qx_gl_smoketest(void)
{
    HWND win;
    void *dpy, *ctx, *surf;
    void *config = NULL;
    int ncfg = 0, major = 0, minor = 0;
    const int cfg_attrs[] = {
        EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
        EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
        EGL_DEPTH_SIZE, 16, EGL_NONE
    };
    const int ctx_attrs[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };

    if (!qx_gl_available()) {
        qx_err("gltest: the ANGLE DLLs (libEGL.dll, libGLESv2.dll) were not found");
        qx_err("  place them beside Quartix.exe or set QUARTIX_ANGLE_DIR");
        return 1;
    }

    {
        NEED(p_getDisplay, pfn_eglGetDisplay, "eglGetDisplay")
        NEED(p_initialize, pfn_eglInitialize, "eglInitialize")
        NEED(p_chooseConfig, pfn_eglChooseConfig, "eglChooseConfig")
        NEED(p_createWindowSurface, pfn_eglCreateWindowSurface, "eglCreateWindowSurface")
        NEED(p_createContext, pfn_eglCreateContext, "eglCreateContext")
        NEED(p_makeCurrent, pfn_eglMakeCurrent, "eglMakeCurrent")
        NEED(p_swapBuffers, pfn_eglSwapBuffers, "eglSwapBuffers")
        NEED(p_getError, pfn_eglGetError, "eglGetError")
        NEED(p_glGetString, pfn_glGetString, "glGetString")
        NEED(p_glClearColor, pfn_glClearColor, "glClearColor")
        NEED(p_glClear, pfn_glClear, "glClear")

        win = make_window();
        if (!win) {
            qx_err("gltest: could not create a window");
            return 1;
        }
        ShowWindow(win, SW_SHOW);

        dpy = p_getDisplay(EGL_DEFAULT_DISPLAY);
        if (dpy == EGL_NO_DISPLAY) {
            qx_err("gltest: eglGetDisplay failed");
            return 1;
        }
        if (!p_initialize(dpy, &major, &minor)) {
            qx_err("gltest: eglInitialize failed (egl error 0x%x)", p_getError());
            return 1;
        }
        qx_log("gltest: EGL %d.%d initialized", major, minor);

        if (!p_chooseConfig(dpy, cfg_attrs, &config, 1, &ncfg) || ncfg < 1) {
            qx_err("gltest: no matching EGL config (egl error 0x%x)", p_getError());
            return 1;
        }

        surf = p_createWindowSurface(dpy, config, (void *)win, NULL);
        if (surf == EGL_NO_SURFACE) {
            qx_err("gltest: eglCreateWindowSurface failed (egl error 0x%x)", p_getError());
            return 1;
        }
        ctx = p_createContext(dpy, config, EGL_NO_CONTEXT, ctx_attrs);
        if (ctx == EGL_NO_CONTEXT) {
            qx_err("gltest: eglCreateContext failed (egl error 0x%x)", p_getError());
            return 1;
        }
        if (!p_makeCurrent(dpy, surf, surf, ctx)) {
            qx_err("gltest: eglMakeCurrent failed (egl error 0x%x)", p_getError());
            return 1;
        }

        qx_log("gltest: GL_VENDOR   %s", (const char *)p_glGetString(GL_VENDOR));
        qx_log("gltest: GL_RENDERER %s", (const char *)p_glGetString(GL_RENDERER));
        qx_log("gltest: GL_VERSION  %s", (const char *)p_glGetString(GL_VERSION));

        /* Draw a few frames in Roblox grey so the window is visibly alive. */
        {
            int f;
            for (f = 0; f < 120; f++) {
                p_glClearColor(0.16f, 0.17f, 0.19f, 1.0f);
                p_glClear(GL_COLOR_BUFFER_BIT);
                p_swapBuffers(dpy, surf);
                pump(1);
            }
        }

        qx_log("gltest: ANGLE created a Direct3D context and drew - graphics path OK");
        DestroyWindow(win);
    }
    return 0;
}

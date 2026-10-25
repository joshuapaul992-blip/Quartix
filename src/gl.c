/*
 * Quartix - EGL (and later GLES) binding to ANGLE, on Win32.
 * Copyright (C) 2026 Quartix contributors. GPLv3.
 *
 * Phase 5, part 1: the engine draws with OpenGL ES 2 through EGL. Rather than
 * hand-write a GLES/EGL implementation, Quartix binds the engine's libEGL (and,
 * in the follow-up, libGLESv2) imports to ANGLE - Google's production GLES/EGL
 * implementation, the same one Chrome ships - loaded as ordinary Windows DLLs
 * placed beside Quartix.exe. ANGLE turns GLES2+EGL into Direct3D (D3D11 on
 * 7/8.1, D3D9 on the Vista floor), which is exactly this phase's job.
 *
 * The one subtlety is the ABI. The engine calls these functions System V style
 * (every Quartix shim function is sysv_abi); ANGLE's DLLs export the Microsoft
 * x64 convention like any Windows DLL. So each entry point here is a sysv_abi
 * thunk that forwards to the Microsoft-ABI function pointer resolved from the
 * DLL - GCC marshals the registers across the two conventions. eglGetProcAddress
 * is routed back through this resolver so that any extension pointer handed to
 * the engine is itself a sysv_abi thunk, never a raw Microsoft-ABI pointer.
 *
 * The DLLs are loaded lazily; until they are present beside the executable the
 * resolver returns NULL and these symbols stay pending (bound to the loader
 * trap), so a build without ANGLE still runs everything else. This layer is
 * validated structurally here (the loader and resolver behave with ANGLE
 * absent); its real exercise is on hardware with a GPU, where ANGLE actually
 * initialises a Direct3D device - this box has neither.
 */
#include "quartix.h"
#include <stdint.h>
#include <string.h>

#define SYSV __attribute__((sysv_abi))

/* ---- EGL types (the stable Khronos ABI) ----------------------------------- */
typedef void *EGLDisplay;
typedef void *EGLConfig;
typedef void *EGLContext;
typedef void *EGLSurface;
typedef void *EGLNativeDisplayType;
typedef void *EGLNativeWindowType;
typedef unsigned int EGLBoolean;
typedef int32_t EGLint;
typedef void (*EGLProc)(void);

/* ---- ANGLE DLL loading ---------------------------------------------------- */
static HMODULE g_libegl;
static HMODULE g_libgles;
static int g_gl_tried;

/* The runtime can point us at an explicit directory; by default LoadLibrary's
 * search finds the DLLs next to Quartix.exe. */
static wchar_t g_gl_dir[QX_PATH_MAX];
static int g_gl_dir_set;
void qx_gl_set_dir(const wchar_t *dir)
{
    if (dir && wcslen(dir) < QX_PATH_MAX) {
        wcscpy(g_gl_dir, dir);
        g_gl_dir_set = 1;
    }
}
static HMODULE load_one(const wchar_t *name)
{
    HMODULE h;
    if (g_gl_dir_set) {
        wchar_t path[QX_PATH_MAX];
        if (qx_path_join(path, QX_PATH_MAX, g_gl_dir, name) == 0) {
            h = LoadLibraryW(path);
            if (h)
                return h;
        }
    }
    return LoadLibraryW(name); /* searches the application directory first */
}
static void gl_ensure(void)
{
    if (g_gl_tried)
        return;
    g_gl_tried = 1;
    g_libegl = load_one(L"libEGL.dll");
    g_libgles = load_one(L"libGLESv2.dll");
}
/* Look a symbol up in the ANGLE DLLs (EGL first, then GLESv2). Exposed so the
 * GLES follow-up slice in this file can share it. */
void *qx_gl_proc(const char *name)
{
    void *p;
    gl_ensure();
    if (g_libegl && (p = (void *)GetProcAddress(g_libegl, name)))
        return p;
    if (g_libgles && (p = (void *)GetProcAddress(g_libgles, name)))
        return p;
    return NULL;
}
int qx_gl_available(void)
{
    gl_ensure();
    return g_libegl != NULL;
}

/* ---- the real (Microsoft-ABI) ANGLE entry points, resolved once ----------- */
/* Each slot holds the DLL's function pointer; a thunk forwards to it. A slot is
 * filled on first use so a call never dereferences NULL once resolve() vetted
 * it. */
#define EGL_PTR(ret, name, params) \
    typedef ret(*name##_t) params; \
    static name##_t p_##name;

EGL_PTR(EGLint, eglGetError, (void))
EGL_PTR(EGLDisplay, eglGetDisplay, (EGLNativeDisplayType))
EGL_PTR(EGLBoolean, eglInitialize, (EGLDisplay, EGLint *, EGLint *))
EGL_PTR(EGLBoolean, eglTerminate, (EGLDisplay))
EGL_PTR(EGLBoolean, eglChooseConfig, (EGLDisplay, const EGLint *, EGLConfig *, EGLint, EGLint *))
EGL_PTR(EGLBoolean, eglGetConfigAttrib, (EGLDisplay, EGLConfig, EGLint, EGLint *))
EGL_PTR(EGLSurface, eglCreateWindowSurface, (EGLDisplay, EGLConfig, EGLNativeWindowType, const EGLint *))
EGL_PTR(EGLSurface, eglCreatePbufferSurface, (EGLDisplay, EGLConfig, const EGLint *))
EGL_PTR(EGLBoolean, eglDestroySurface, (EGLDisplay, EGLSurface))
EGL_PTR(EGLContext, eglCreateContext, (EGLDisplay, EGLConfig, EGLContext, const EGLint *))
EGL_PTR(EGLBoolean, eglDestroyContext, (EGLDisplay, EGLContext))
EGL_PTR(EGLBoolean, eglMakeCurrent, (EGLDisplay, EGLSurface, EGLSurface, EGLContext))
EGL_PTR(EGLContext, eglGetCurrentContext, (void))
EGL_PTR(EGLBoolean, eglQuerySurface, (EGLDisplay, EGLSurface, EGLint, EGLint *))
EGL_PTR(EGLBoolean, eglSwapBuffers, (EGLDisplay, EGLSurface))
EGL_PTR(EGLBoolean, eglSwapInterval, (EGLDisplay, EGLint))

/* ---- sysv_abi thunks ------------------------------------------------------ */
static SYSV EGLint qx_eglGetError(void) { return p_eglGetError(); }
static SYSV EGLDisplay qx_eglGetDisplay(EGLNativeDisplayType d) { return p_eglGetDisplay(d); }
static SYSV EGLBoolean qx_eglInitialize(EGLDisplay d, EGLint *a, EGLint *b) { return p_eglInitialize(d, a, b); }
static SYSV EGLBoolean qx_eglTerminate(EGLDisplay d) { return p_eglTerminate(d); }
static SYSV EGLBoolean qx_eglChooseConfig(EGLDisplay d, const EGLint *a, EGLConfig *c, EGLint n, EGLint *m)
{ return p_eglChooseConfig(d, a, c, n, m); }
static SYSV EGLBoolean qx_eglGetConfigAttrib(EGLDisplay d, EGLConfig c, EGLint a, EGLint *v)
{ return p_eglGetConfigAttrib(d, c, a, v); }
static SYSV EGLSurface qx_eglCreateWindowSurface(EGLDisplay d, EGLConfig c, EGLNativeWindowType w, const EGLint *a)
{ return p_eglCreateWindowSurface(d, c, w, a); }
static SYSV EGLSurface qx_eglCreatePbufferSurface(EGLDisplay d, EGLConfig c, const EGLint *a)
{ return p_eglCreatePbufferSurface(d, c, a); }
static SYSV EGLBoolean qx_eglDestroySurface(EGLDisplay d, EGLSurface s) { return p_eglDestroySurface(d, s); }
static SYSV EGLContext qx_eglCreateContext(EGLDisplay d, EGLConfig c, EGLContext sh, const EGLint *a)
{ return p_eglCreateContext(d, c, sh, a); }
static SYSV EGLBoolean qx_eglDestroyContext(EGLDisplay d, EGLContext c) { return p_eglDestroyContext(d, c); }
static SYSV EGLBoolean qx_eglMakeCurrent(EGLDisplay d, EGLSurface dr, EGLSurface rd, EGLContext c)
{ return p_eglMakeCurrent(d, dr, rd, c); }
static SYSV EGLContext qx_eglGetCurrentContext(void) { return p_eglGetCurrentContext(); }
static SYSV EGLBoolean qx_eglQuerySurface(EGLDisplay d, EGLSurface s, EGLint a, EGLint *v)
{ return p_eglQuerySurface(d, s, a, v); }
static SYSV EGLBoolean qx_eglSwapBuffers(EGLDisplay d, EGLSurface s) { return p_eglSwapBuffers(d, s); }
static SYSV EGLBoolean qx_eglSwapInterval(EGLDisplay d, EGLint i) { return p_eglSwapInterval(d, i); }

/* eglGetProcAddress returns a function pointer the engine will call System V
 * style, so it must hand back one of our thunks, never ANGLE's raw pointer. */
static SYSV EGLProc qx_eglGetProcAddress(const char *name) { return (EGLProc)qx_gl_resolve(name); }

/* ---- resolver -------------------------------------------------------------- */
/* name -> {thunk, &real-pointer}. resolve() loads the DLL symbol into the real
 * pointer and returns the thunk only when both are available. */
struct egl_sym {
    const char *name;
    void *thunk;
    void **real;
};
static const struct egl_sym g_egl_table[] = {
    {"eglGetError", (void *)qx_eglGetError, (void **)&p_eglGetError},
    {"eglGetDisplay", (void *)qx_eglGetDisplay, (void **)&p_eglGetDisplay},
    {"eglInitialize", (void *)qx_eglInitialize, (void **)&p_eglInitialize},
    {"eglTerminate", (void *)qx_eglTerminate, (void **)&p_eglTerminate},
    {"eglChooseConfig", (void *)qx_eglChooseConfig, (void **)&p_eglChooseConfig},
    {"eglGetConfigAttrib", (void *)qx_eglGetConfigAttrib, (void **)&p_eglGetConfigAttrib},
    {"eglCreateWindowSurface", (void *)qx_eglCreateWindowSurface, (void **)&p_eglCreateWindowSurface},
    {"eglCreatePbufferSurface", (void *)qx_eglCreatePbufferSurface, (void **)&p_eglCreatePbufferSurface},
    {"eglDestroySurface", (void *)qx_eglDestroySurface, (void **)&p_eglDestroySurface},
    {"eglCreateContext", (void *)qx_eglCreateContext, (void **)&p_eglCreateContext},
    {"eglDestroyContext", (void *)qx_eglDestroyContext, (void **)&p_eglDestroyContext},
    {"eglMakeCurrent", (void *)qx_eglMakeCurrent, (void **)&p_eglMakeCurrent},
    {"eglGetCurrentContext", (void *)qx_eglGetCurrentContext, (void **)&p_eglGetCurrentContext},
    {"eglQuerySurface", (void *)qx_eglQuerySurface, (void **)&p_eglQuerySurface},
    {"eglSwapBuffers", (void *)qx_eglSwapBuffers, (void **)&p_eglSwapBuffers},
    {"eglSwapInterval", (void *)qx_eglSwapInterval, (void **)&p_eglSwapInterval},
};

void *qx_gl_resolve(const char *name)
{
    size_t i;
    if (!name)
        return NULL;
    if (strcmp(name, "eglGetProcAddress") == 0)
        return qx_gl_available() ? (void *)qx_eglGetProcAddress : NULL;
    for (i = 0; i < sizeof(g_egl_table) / sizeof(g_egl_table[0]); i++) {
        if (strcmp(name, g_egl_table[i].name) == 0) {
            if (!*g_egl_table[i].real) {
                void *p = qx_gl_proc(name);
                if (!p)
                    return NULL; /* ANGLE absent: stays pending */
                *g_egl_table[i].real = p;
            }
            return g_egl_table[i].thunk;
        }
    }
    return NULL;
}

/* ---- self-test ------------------------------------------------------------ */
/* Functional EGL needs a GPU and the ANGLE DLLs, which this box lacks, so the
 * check verifies the loader degrades cleanly: with ANGLE absent, resolve()
 * returns NULL for every EGL name rather than a dangling thunk, and the trap
 * keeps the image complete. When ANGLE is present (on real hardware) the same
 * names resolve to thunks. */
SYSV int qx_gl_selftest(int *total)
{
    int fails = 0, n = 0;

    n++;
    if (qx_gl_available()) {
        /* ANGLE present: a known EGL entry must resolve to a thunk */
        if (qx_gl_resolve("eglGetError") == NULL)
            fails++;
    } else {
        /* ANGLE absent: every EGL name resolves to NULL, no crash */
        if (qx_gl_resolve("eglGetError") != NULL ||
            qx_gl_resolve("eglInitialize") != NULL ||
            qx_gl_resolve("eglGetProcAddress") != NULL)
            fails++;
    }
    /* an unknown name is always NULL */
    n++;
    if (qx_gl_resolve("eglNoSuchEntryPoint") != NULL)
        fails++;

    if (total)
        *total = n;
    return fails;
}

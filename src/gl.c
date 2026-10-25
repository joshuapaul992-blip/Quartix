/*
 * Quartix - EGL (and later GLES) binding to ANGLE, on Win32.
 * Copyright (C) 2026 Quartix contributors. GPLv3.
 *
 * Phase 5: the engine draws with OpenGL ES 2 through EGL. Rather than
 * hand-write a GLES/EGL implementation, Quartix binds the engine's libEGL and
 * libGLESv2 imports to ANGLE - Google's production GLES/EGL implementation, the
 * same one Chrome ships - loaded as ordinary Windows DLLs placed beside
 * Quartix.exe. ANGLE turns GLES2+EGL into Direct3D (D3D11 on 7/8.1, D3D9 on the
 * Vista floor), which is exactly this phase's job. The engine imports exactly
 * the 17 EGL and 74 GLES entry points bound here; every one of the 74 GL calls
 * is GLES 2.0 core (no VAOs, instancing, map-buffer-range, sync objects or
 * multisample), so a GLES2-capable ANGLE build is sufficient.
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

/* ---- GLES2 types (the stable Khronos ABI) --------------------------------- */
typedef unsigned int GLenum;
typedef unsigned char GLboolean;
typedef unsigned int GLbitfield;
typedef int GLint;
typedef int GLsizei;
typedef unsigned int GLuint;
typedef float GLfloat;
typedef char GLchar;
typedef unsigned char GLubyte;
/* On the LP64 engine GLintptr/GLsizeiptr are 64-bit; on Win64 intptr_t is also
 * 64-bit, so the two agree. */
typedef intptr_t GLintptr;
typedef intptr_t GLsizeiptr;

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
    /* An explicit directory can be given with QUARTIX_ANGLE_DIR; otherwise the
     * DLLs are found by LoadLibrary's normal search, i.e. next to Quartix.exe. */
    if (!g_gl_dir_set) {
        DWORD n = GetEnvironmentVariableW(L"QUARTIX_ANGLE_DIR", g_gl_dir, QX_PATH_MAX);
        if (n > 0 && n < QX_PATH_MAX)
            g_gl_dir_set = 1;
    }
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

/* ---- GLES2 entry points (all 74 the engine imports are GLES 2.0 core) ------ */
/* EGL_PTR is a generic "typedef + Microsoft-ABI slot" declarator; reused here. */
EGL_PTR(void, glActiveTexture, (GLenum))
EGL_PTR(void, glAttachShader, (GLuint, GLuint))
EGL_PTR(void, glBindAttribLocation, (GLuint, GLuint, const GLchar *))
EGL_PTR(void, glBindBuffer, (GLenum, GLuint))
EGL_PTR(void, glBindFramebuffer, (GLenum, GLuint))
EGL_PTR(void, glBindRenderbuffer, (GLenum, GLuint))
EGL_PTR(void, glBindTexture, (GLenum, GLuint))
EGL_PTR(void, glBlendFunc, (GLenum, GLenum))
EGL_PTR(void, glBlendFuncSeparate, (GLenum, GLenum, GLenum, GLenum))
EGL_PTR(void, glBufferData, (GLenum, GLsizeiptr, const void *, GLenum))
EGL_PTR(void, glBufferSubData, (GLenum, GLintptr, GLsizeiptr, const void *))
EGL_PTR(GLenum, glCheckFramebufferStatus, (GLenum))
EGL_PTR(void, glClear, (GLbitfield))
EGL_PTR(void, glClearColor, (GLfloat, GLfloat, GLfloat, GLfloat))
EGL_PTR(void, glClearDepthf, (GLfloat))
EGL_PTR(void, glClearStencil, (GLint))
EGL_PTR(void, glColorMask, (GLboolean, GLboolean, GLboolean, GLboolean))
EGL_PTR(void, glCompileShader, (GLuint))
EGL_PTR(void, glCompressedTexImage2D, (GLenum, GLint, GLenum, GLsizei, GLsizei, GLint, GLsizei, const void *))
EGL_PTR(void, glCompressedTexSubImage2D, (GLenum, GLint, GLint, GLint, GLsizei, GLsizei, GLenum, GLsizei, const void *))
EGL_PTR(void, glCopyTexSubImage2D, (GLenum, GLint, GLint, GLint, GLint, GLint, GLsizei, GLsizei))
EGL_PTR(GLuint, glCreateProgram, (void))
EGL_PTR(GLuint, glCreateShader, (GLenum))
EGL_PTR(void, glCullFace, (GLenum))
EGL_PTR(void, glDeleteBuffers, (GLsizei, const GLuint *))
EGL_PTR(void, glDeleteFramebuffers, (GLsizei, const GLuint *))
EGL_PTR(void, glDeleteProgram, (GLuint))
EGL_PTR(void, glDeleteRenderbuffers, (GLsizei, const GLuint *))
EGL_PTR(void, glDeleteShader, (GLuint))
EGL_PTR(void, glDeleteTextures, (GLsizei, const GLuint *))
EGL_PTR(void, glDepthFunc, (GLenum))
EGL_PTR(void, glDepthMask, (GLboolean))
EGL_PTR(void, glDisable, (GLenum))
EGL_PTR(void, glDisableVertexAttribArray, (GLuint))
EGL_PTR(void, glDrawArrays, (GLenum, GLint, GLsizei))
EGL_PTR(void, glDrawElements, (GLenum, GLsizei, GLenum, const void *))
EGL_PTR(void, glEnable, (GLenum))
EGL_PTR(void, glEnableVertexAttribArray, (GLuint))
EGL_PTR(void, glFramebufferRenderbuffer, (GLenum, GLenum, GLenum, GLuint))
EGL_PTR(void, glFramebufferTexture2D, (GLenum, GLenum, GLenum, GLuint, GLint))
EGL_PTR(void, glGenBuffers, (GLsizei, GLuint *))
EGL_PTR(void, glGenFramebuffers, (GLsizei, GLuint *))
EGL_PTR(void, glGenRenderbuffers, (GLsizei, GLuint *))
EGL_PTR(void, glGenTextures, (GLsizei, GLuint *))
EGL_PTR(void, glGenerateMipmap, (GLenum))
EGL_PTR(void, glGetActiveUniform, (GLuint, GLuint, GLsizei, GLsizei *, GLint *, GLenum *, GLchar *))
EGL_PTR(GLenum, glGetError, (void))
EGL_PTR(void, glGetIntegerv, (GLenum, GLint *))
EGL_PTR(void, glGetProgramInfoLog, (GLuint, GLsizei, GLsizei *, GLchar *))
EGL_PTR(void, glGetProgramiv, (GLuint, GLenum, GLint *))
EGL_PTR(void, glGetShaderInfoLog, (GLuint, GLsizei, GLsizei *, GLchar *))
EGL_PTR(void, glGetShaderiv, (GLuint, GLenum, GLint *))
EGL_PTR(const GLubyte *, glGetString, (GLenum))
EGL_PTR(GLint, glGetUniformLocation, (GLuint, const GLchar *))
EGL_PTR(void, glLinkProgram, (GLuint))
EGL_PTR(void, glPixelStorei, (GLenum, GLint))
EGL_PTR(void, glPolygonOffset, (GLfloat, GLfloat))
EGL_PTR(void, glReadPixels, (GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, void *))
EGL_PTR(void, glReleaseShaderCompiler, (void))
EGL_PTR(void, glRenderbufferStorage, (GLenum, GLenum, GLsizei, GLsizei))
EGL_PTR(void, glScissor, (GLint, GLint, GLsizei, GLsizei))
EGL_PTR(void, glShaderSource, (GLuint, GLsizei, const GLchar *const *, const GLint *))
EGL_PTR(void, glStencilFunc, (GLenum, GLint, GLuint))
EGL_PTR(void, glStencilMask, (GLuint))
EGL_PTR(void, glStencilOp, (GLenum, GLenum, GLenum))
EGL_PTR(void, glTexImage2D, (GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum, const void *))
EGL_PTR(void, glTexParameterf, (GLenum, GLenum, GLfloat))
EGL_PTR(void, glTexParameterfv, (GLenum, GLenum, const GLfloat *))
EGL_PTR(void, glTexParameteri, (GLenum, GLenum, GLint))
EGL_PTR(void, glTexSubImage2D, (GLenum, GLint, GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, const void *))
EGL_PTR(void, glUniform1i, (GLint, GLint))
EGL_PTR(void, glUseProgram, (GLuint))
EGL_PTR(void, glVertexAttribPointer, (GLuint, GLint, GLenum, GLboolean, GLsizei, const void *))
EGL_PTR(void, glViewport, (GLint, GLint, GLsizei, GLsizei))

static SYSV void qx_glActiveTexture(GLenum a){p_glActiveTexture(a);}
static SYSV void qx_glAttachShader(GLuint a,GLuint b){p_glAttachShader(a,b);}
static SYSV void qx_glBindAttribLocation(GLuint a,GLuint b,const GLchar*c){p_glBindAttribLocation(a,b,c);}
static SYSV void qx_glBindBuffer(GLenum a,GLuint b){p_glBindBuffer(a,b);}
static SYSV void qx_glBindFramebuffer(GLenum a,GLuint b){p_glBindFramebuffer(a,b);}
static SYSV void qx_glBindRenderbuffer(GLenum a,GLuint b){p_glBindRenderbuffer(a,b);}
static SYSV void qx_glBindTexture(GLenum a,GLuint b){p_glBindTexture(a,b);}
static SYSV void qx_glBlendFunc(GLenum a,GLenum b){p_glBlendFunc(a,b);}
static SYSV void qx_glBlendFuncSeparate(GLenum a,GLenum b,GLenum c,GLenum d){p_glBlendFuncSeparate(a,b,c,d);}
static SYSV void qx_glBufferData(GLenum a,GLsizeiptr b,const void*c,GLenum d){p_glBufferData(a,b,c,d);}
static SYSV void qx_glBufferSubData(GLenum a,GLintptr b,GLsizeiptr c,const void*d){p_glBufferSubData(a,b,c,d);}
static SYSV GLenum qx_glCheckFramebufferStatus(GLenum a){return p_glCheckFramebufferStatus(a);}
static SYSV void qx_glClear(GLbitfield a){p_glClear(a);}
static SYSV void qx_glClearColor(GLfloat a,GLfloat b,GLfloat c,GLfloat d){p_glClearColor(a,b,c,d);}
static SYSV void qx_glClearDepthf(GLfloat a){p_glClearDepthf(a);}
static SYSV void qx_glClearStencil(GLint a){p_glClearStencil(a);}
static SYSV void qx_glColorMask(GLboolean a,GLboolean b,GLboolean c,GLboolean d){p_glColorMask(a,b,c,d);}
static SYSV void qx_glCompileShader(GLuint a){p_glCompileShader(a);}
static SYSV void qx_glCompressedTexImage2D(GLenum a,GLint b,GLenum c,GLsizei d,GLsizei e,GLint f,GLsizei g,const void*h){p_glCompressedTexImage2D(a,b,c,d,e,f,g,h);}
static SYSV void qx_glCompressedTexSubImage2D(GLenum a,GLint b,GLint c,GLint d,GLsizei e,GLsizei f,GLenum g,GLsizei h,const void*i){p_glCompressedTexSubImage2D(a,b,c,d,e,f,g,h,i);}
static SYSV void qx_glCopyTexSubImage2D(GLenum a,GLint b,GLint c,GLint d,GLint e,GLint f,GLsizei g,GLsizei h){p_glCopyTexSubImage2D(a,b,c,d,e,f,g,h);}
static SYSV GLuint qx_glCreateProgram(void){return p_glCreateProgram();}
static SYSV GLuint qx_glCreateShader(GLenum a){return p_glCreateShader(a);}
static SYSV void qx_glCullFace(GLenum a){p_glCullFace(a);}
static SYSV void qx_glDeleteBuffers(GLsizei a,const GLuint*b){p_glDeleteBuffers(a,b);}
static SYSV void qx_glDeleteFramebuffers(GLsizei a,const GLuint*b){p_glDeleteFramebuffers(a,b);}
static SYSV void qx_glDeleteProgram(GLuint a){p_glDeleteProgram(a);}
static SYSV void qx_glDeleteRenderbuffers(GLsizei a,const GLuint*b){p_glDeleteRenderbuffers(a,b);}
static SYSV void qx_glDeleteShader(GLuint a){p_glDeleteShader(a);}
static SYSV void qx_glDeleteTextures(GLsizei a,const GLuint*b){p_glDeleteTextures(a,b);}
static SYSV void qx_glDepthFunc(GLenum a){p_glDepthFunc(a);}
static SYSV void qx_glDepthMask(GLboolean a){p_glDepthMask(a);}
static SYSV void qx_glDisable(GLenum a){p_glDisable(a);}
static SYSV void qx_glDisableVertexAttribArray(GLuint a){p_glDisableVertexAttribArray(a);}
static SYSV void qx_glDrawArrays(GLenum a,GLint b,GLsizei c){p_glDrawArrays(a,b,c);}
static SYSV void qx_glDrawElements(GLenum a,GLsizei b,GLenum c,const void*d){p_glDrawElements(a,b,c,d);}
static SYSV void qx_glEnable(GLenum a){p_glEnable(a);}
static SYSV void qx_glEnableVertexAttribArray(GLuint a){p_glEnableVertexAttribArray(a);}
static SYSV void qx_glFramebufferRenderbuffer(GLenum a,GLenum b,GLenum c,GLuint d){p_glFramebufferRenderbuffer(a,b,c,d);}
static SYSV void qx_glFramebufferTexture2D(GLenum a,GLenum b,GLenum c,GLuint d,GLint e){p_glFramebufferTexture2D(a,b,c,d,e);}
static SYSV void qx_glGenBuffers(GLsizei a,GLuint*b){p_glGenBuffers(a,b);}
static SYSV void qx_glGenFramebuffers(GLsizei a,GLuint*b){p_glGenFramebuffers(a,b);}
static SYSV void qx_glGenRenderbuffers(GLsizei a,GLuint*b){p_glGenRenderbuffers(a,b);}
static SYSV void qx_glGenTextures(GLsizei a,GLuint*b){p_glGenTextures(a,b);}
static SYSV void qx_glGenerateMipmap(GLenum a){p_glGenerateMipmap(a);}
static SYSV void qx_glGetActiveUniform(GLuint a,GLuint b,GLsizei c,GLsizei*d,GLint*e,GLenum*f,GLchar*g){p_glGetActiveUniform(a,b,c,d,e,f,g);}
static SYSV GLenum qx_glGetError(void){return p_glGetError();}
static SYSV void qx_glGetIntegerv(GLenum a,GLint*b){p_glGetIntegerv(a,b);}
static SYSV void qx_glGetProgramInfoLog(GLuint a,GLsizei b,GLsizei*c,GLchar*d){p_glGetProgramInfoLog(a,b,c,d);}
static SYSV void qx_glGetProgramiv(GLuint a,GLenum b,GLint*c){p_glGetProgramiv(a,b,c);}
static SYSV void qx_glGetShaderInfoLog(GLuint a,GLsizei b,GLsizei*c,GLchar*d){p_glGetShaderInfoLog(a,b,c,d);}
static SYSV void qx_glGetShaderiv(GLuint a,GLenum b,GLint*c){p_glGetShaderiv(a,b,c);}
static SYSV const GLubyte* qx_glGetString(GLenum a){return p_glGetString(a);}
static SYSV GLint qx_glGetUniformLocation(GLuint a,const GLchar*b){return p_glGetUniformLocation(a,b);}
static SYSV void qx_glLinkProgram(GLuint a){p_glLinkProgram(a);}
static SYSV void qx_glPixelStorei(GLenum a,GLint b){p_glPixelStorei(a,b);}
static SYSV void qx_glPolygonOffset(GLfloat a,GLfloat b){p_glPolygonOffset(a,b);}
static SYSV void qx_glReadPixels(GLint a,GLint b,GLsizei c,GLsizei d,GLenum e,GLenum f,void*g){p_glReadPixels(a,b,c,d,e,f,g);}
static SYSV void qx_glReleaseShaderCompiler(void){p_glReleaseShaderCompiler();}
static SYSV void qx_glRenderbufferStorage(GLenum a,GLenum b,GLsizei c,GLsizei d){p_glRenderbufferStorage(a,b,c,d);}
static SYSV void qx_glScissor(GLint a,GLint b,GLsizei c,GLsizei d){p_glScissor(a,b,c,d);}
static SYSV void qx_glShaderSource(GLuint a,GLsizei b,const GLchar*const*c,const GLint*d){p_glShaderSource(a,b,c,d);}
static SYSV void qx_glStencilFunc(GLenum a,GLint b,GLuint c){p_glStencilFunc(a,b,c);}
static SYSV void qx_glStencilMask(GLuint a){p_glStencilMask(a);}
static SYSV void qx_glStencilOp(GLenum a,GLenum b,GLenum c){p_glStencilOp(a,b,c);}
static SYSV void qx_glTexImage2D(GLenum a,GLint b,GLint c,GLsizei d,GLsizei e,GLint f,GLenum g,GLenum h,const void*i){p_glTexImage2D(a,b,c,d,e,f,g,h,i);}
static SYSV void qx_glTexParameterf(GLenum a,GLenum b,GLfloat c){p_glTexParameterf(a,b,c);}
static SYSV void qx_glTexParameterfv(GLenum a,GLenum b,const GLfloat*c){p_glTexParameterfv(a,b,c);}
static SYSV void qx_glTexParameteri(GLenum a,GLenum b,GLint c){p_glTexParameteri(a,b,c);}
static SYSV void qx_glTexSubImage2D(GLenum a,GLint b,GLint c,GLint d,GLsizei e,GLsizei f,GLenum g,GLenum h,const void*i){p_glTexSubImage2D(a,b,c,d,e,f,g,h,i);}
static SYSV void qx_glUniform1i(GLint a,GLint b){p_glUniform1i(a,b);}
static SYSV void qx_glUseProgram(GLuint a){p_glUseProgram(a);}
static SYSV void qx_glVertexAttribPointer(GLuint a,GLint b,GLenum c,GLboolean d,GLsizei e,const void*f){p_glVertexAttribPointer(a,b,c,d,e,f);}
static SYSV void qx_glViewport(GLint a,GLint b,GLsizei c,GLsizei d){p_glViewport(a,b,c,d);}

static const struct egl_sym g_gl_table[] = {
    {"glActiveTexture",(void*)qx_glActiveTexture,(void**)&p_glActiveTexture},
    {"glAttachShader",(void*)qx_glAttachShader,(void**)&p_glAttachShader},
    {"glBindAttribLocation",(void*)qx_glBindAttribLocation,(void**)&p_glBindAttribLocation},
    {"glBindBuffer",(void*)qx_glBindBuffer,(void**)&p_glBindBuffer},
    {"glBindFramebuffer",(void*)qx_glBindFramebuffer,(void**)&p_glBindFramebuffer},
    {"glBindRenderbuffer",(void*)qx_glBindRenderbuffer,(void**)&p_glBindRenderbuffer},
    {"glBindTexture",(void*)qx_glBindTexture,(void**)&p_glBindTexture},
    {"glBlendFunc",(void*)qx_glBlendFunc,(void**)&p_glBlendFunc},
    {"glBlendFuncSeparate",(void*)qx_glBlendFuncSeparate,(void**)&p_glBlendFuncSeparate},
    {"glBufferData",(void*)qx_glBufferData,(void**)&p_glBufferData},
    {"glBufferSubData",(void*)qx_glBufferSubData,(void**)&p_glBufferSubData},
    {"glCheckFramebufferStatus",(void*)qx_glCheckFramebufferStatus,(void**)&p_glCheckFramebufferStatus},
    {"glClear",(void*)qx_glClear,(void**)&p_glClear},
    {"glClearColor",(void*)qx_glClearColor,(void**)&p_glClearColor},
    {"glClearDepthf",(void*)qx_glClearDepthf,(void**)&p_glClearDepthf},
    {"glClearStencil",(void*)qx_glClearStencil,(void**)&p_glClearStencil},
    {"glColorMask",(void*)qx_glColorMask,(void**)&p_glColorMask},
    {"glCompileShader",(void*)qx_glCompileShader,(void**)&p_glCompileShader},
    {"glCompressedTexImage2D",(void*)qx_glCompressedTexImage2D,(void**)&p_glCompressedTexImage2D},
    {"glCompressedTexSubImage2D",(void*)qx_glCompressedTexSubImage2D,(void**)&p_glCompressedTexSubImage2D},
    {"glCopyTexSubImage2D",(void*)qx_glCopyTexSubImage2D,(void**)&p_glCopyTexSubImage2D},
    {"glCreateProgram",(void*)qx_glCreateProgram,(void**)&p_glCreateProgram},
    {"glCreateShader",(void*)qx_glCreateShader,(void**)&p_glCreateShader},
    {"glCullFace",(void*)qx_glCullFace,(void**)&p_glCullFace},
    {"glDeleteBuffers",(void*)qx_glDeleteBuffers,(void**)&p_glDeleteBuffers},
    {"glDeleteFramebuffers",(void*)qx_glDeleteFramebuffers,(void**)&p_glDeleteFramebuffers},
    {"glDeleteProgram",(void*)qx_glDeleteProgram,(void**)&p_glDeleteProgram},
    {"glDeleteRenderbuffers",(void*)qx_glDeleteRenderbuffers,(void**)&p_glDeleteRenderbuffers},
    {"glDeleteShader",(void*)qx_glDeleteShader,(void**)&p_glDeleteShader},
    {"glDeleteTextures",(void*)qx_glDeleteTextures,(void**)&p_glDeleteTextures},
    {"glDepthFunc",(void*)qx_glDepthFunc,(void**)&p_glDepthFunc},
    {"glDepthMask",(void*)qx_glDepthMask,(void**)&p_glDepthMask},
    {"glDisable",(void*)qx_glDisable,(void**)&p_glDisable},
    {"glDisableVertexAttribArray",(void*)qx_glDisableVertexAttribArray,(void**)&p_glDisableVertexAttribArray},
    {"glDrawArrays",(void*)qx_glDrawArrays,(void**)&p_glDrawArrays},
    {"glDrawElements",(void*)qx_glDrawElements,(void**)&p_glDrawElements},
    {"glEnable",(void*)qx_glEnable,(void**)&p_glEnable},
    {"glEnableVertexAttribArray",(void*)qx_glEnableVertexAttribArray,(void**)&p_glEnableVertexAttribArray},
    {"glFramebufferRenderbuffer",(void*)qx_glFramebufferRenderbuffer,(void**)&p_glFramebufferRenderbuffer},
    {"glFramebufferTexture2D",(void*)qx_glFramebufferTexture2D,(void**)&p_glFramebufferTexture2D},
    {"glGenBuffers",(void*)qx_glGenBuffers,(void**)&p_glGenBuffers},
    {"glGenFramebuffers",(void*)qx_glGenFramebuffers,(void**)&p_glGenFramebuffers},
    {"glGenRenderbuffers",(void*)qx_glGenRenderbuffers,(void**)&p_glGenRenderbuffers},
    {"glGenTextures",(void*)qx_glGenTextures,(void**)&p_glGenTextures},
    {"glGenerateMipmap",(void*)qx_glGenerateMipmap,(void**)&p_glGenerateMipmap},
    {"glGetActiveUniform",(void*)qx_glGetActiveUniform,(void**)&p_glGetActiveUniform},
    {"glGetError",(void*)qx_glGetError,(void**)&p_glGetError},
    {"glGetIntegerv",(void*)qx_glGetIntegerv,(void**)&p_glGetIntegerv},
    {"glGetProgramInfoLog",(void*)qx_glGetProgramInfoLog,(void**)&p_glGetProgramInfoLog},
    {"glGetProgramiv",(void*)qx_glGetProgramiv,(void**)&p_glGetProgramiv},
    {"glGetShaderInfoLog",(void*)qx_glGetShaderInfoLog,(void**)&p_glGetShaderInfoLog},
    {"glGetShaderiv",(void*)qx_glGetShaderiv,(void**)&p_glGetShaderiv},
    {"glGetString",(void*)qx_glGetString,(void**)&p_glGetString},
    {"glGetUniformLocation",(void*)qx_glGetUniformLocation,(void**)&p_glGetUniformLocation},
    {"glLinkProgram",(void*)qx_glLinkProgram,(void**)&p_glLinkProgram},
    {"glPixelStorei",(void*)qx_glPixelStorei,(void**)&p_glPixelStorei},
    {"glPolygonOffset",(void*)qx_glPolygonOffset,(void**)&p_glPolygonOffset},
    {"glReadPixels",(void*)qx_glReadPixels,(void**)&p_glReadPixels},
    {"glReleaseShaderCompiler",(void*)qx_glReleaseShaderCompiler,(void**)&p_glReleaseShaderCompiler},
    {"glRenderbufferStorage",(void*)qx_glRenderbufferStorage,(void**)&p_glRenderbufferStorage},
    {"glScissor",(void*)qx_glScissor,(void**)&p_glScissor},
    {"glShaderSource",(void*)qx_glShaderSource,(void**)&p_glShaderSource},
    {"glStencilFunc",(void*)qx_glStencilFunc,(void**)&p_glStencilFunc},
    {"glStencilMask",(void*)qx_glStencilMask,(void**)&p_glStencilMask},
    {"glStencilOp",(void*)qx_glStencilOp,(void**)&p_glStencilOp},
    {"glTexImage2D",(void*)qx_glTexImage2D,(void**)&p_glTexImage2D},
    {"glTexParameterf",(void*)qx_glTexParameterf,(void**)&p_glTexParameterf},
    {"glTexParameterfv",(void*)qx_glTexParameterfv,(void**)&p_glTexParameterfv},
    {"glTexParameteri",(void*)qx_glTexParameteri,(void**)&p_glTexParameteri},
    {"glTexSubImage2D",(void*)qx_glTexSubImage2D,(void**)&p_glTexSubImage2D},
    {"glUniform1i",(void*)qx_glUniform1i,(void**)&p_glUniform1i},
    {"glUseProgram",(void*)qx_glUseProgram,(void**)&p_glUseProgram},
    {"glVertexAttribPointer",(void*)qx_glVertexAttribPointer,(void**)&p_glVertexAttribPointer},
    {"glViewport",(void*)qx_glViewport,(void**)&p_glViewport},
};

/* ---- resolver -------------------------------------------------------------- */
/* Look a name up in one binding table, filling its Microsoft-ABI slot from the
 * DLL on first use; returns the sysv_abi thunk, or NULL when ANGLE is absent so
 * the symbol stays pending (bound to the loader trap). */
static void *resolve_in(const struct egl_sym *tab, size_t n, const char *name)
{
    size_t i;
    for (i = 0; i < n; i++) {
        if (strcmp(name, tab[i].name) == 0) {
            if (!*tab[i].real) {
                void *p = qx_gl_proc(name);
                if (!p)
                    return NULL;
                *tab[i].real = p;
            }
            return tab[i].thunk;
        }
    }
    return NULL;
}

void *qx_gl_resolve(const char *name)
{
    void *r;
    if (!name)
        return NULL;
    if (strcmp(name, "eglGetProcAddress") == 0)
        return qx_gl_available() ? (void *)qx_eglGetProcAddress : NULL;
    r = resolve_in(g_egl_table, sizeof(g_egl_table) / sizeof(g_egl_table[0]), name);
    if (r)
        return r;
    return resolve_in(g_gl_table, sizeof(g_gl_table) / sizeof(g_gl_table[0]), name);
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
        /* ANGLE present: known EGL and GLES entries must resolve to thunks */
        if (qx_gl_resolve("eglGetError") == NULL ||
            qx_gl_resolve("glClear") == NULL)
            fails++;
    } else {
        /* ANGLE absent: every EGL and GLES name resolves to NULL, no crash */
        if (qx_gl_resolve("eglGetError") != NULL ||
            qx_gl_resolve("eglInitialize") != NULL ||
            qx_gl_resolve("eglGetProcAddress") != NULL ||
            qx_gl_resolve("glClear") != NULL ||
            qx_gl_resolve("glDrawElements") != NULL)
            fails++;
    }
    /* an unknown name is always NULL, in either table */
    n++;
    if (qx_gl_resolve("eglNoSuchEntryPoint") != NULL ||
        qx_gl_resolve("glNoSuchEntryPoint") != NULL)
        fails++;

    if (total)
        *total = n;
    return fails;
}

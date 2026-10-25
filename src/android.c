/*
 * Quartix - the libandroid NativeActivity surface, on Win32.
 * Copyright (C) 2026 Quartix contributors. GPLv3.
 *
 * Phase 4: the NDK C entry points the engine reaches through libandroid, as
 * opposed to the libc/libm shim below it. The engine is an NDK NativeActivity
 * app, so it drives its lifecycle and I/O through these:
 *
 *   - AAssetManager / AAsset  - open a packaged asset and read its bytes. Real:
 *     backed by a filesystem asset root (which the runtime points at the APK's
 *     extracted assets). open -> read whole file into a buffer; getBuffer /
 *     getLength / close work over that buffer.
 *   - AConfiguration          - device configuration (locale, screen size,
 *     density). Real: a small struct with sensible host-derived values.
 *   - ALooper                 - the per-thread event loop the engine pumps.
 *     Real but minimal: a per-thread looper whose pollOnce waits up to the
 *     caller's timeout and reports "timed out" (no registered fd becomes ready
 *     in this headless build), so the engine's loop advances without spinning.
 *   - ANativeWindow           - the drawing surface. A refcounted handle that
 *     reports its dimensions; binding it to a real HWND/EGL surface is phase 5.
 *   - AHardwareBuffer / AImageReader / AImage - graphics-interop objects reached
 *     only deep in rendering. Safe refcounted placeholders here; real behavior
 *     lands with the graphics phase.
 *
 * Every engine-callable function is sysv_abi. These are framework entry points:
 * they are validated structurally here (they resolve, and the self-test
 * exercises the asset/config/looper/window mechanics without crashing). Their
 * full behavior is exercised once the window + ANGLE land and the client runs
 * on real hardware - this box has no GPU and no Android lifecycle to drive them.
 */
#include "quartix.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SYSV __attribute__((sysv_abi))

/* ---- AConfiguration ------------------------------------------------------- */
/* ACONFIGURATION_* screen-size buckets and the nav-hidden values. */
#define ACONFIGURATION_SCREENSIZE_NORMAL 0x02
#define ACONFIGURATION_KEYSHIDDEN_NO 0x01

struct qx_aconfig {
    char language[2];
    char country[2];
    int32_t screen_width_dp;
    int32_t screen_height_dp;
    int32_t screen_size;
    int32_t nav_hidden;
};

static SYSV struct qx_aconfig *qx_AConfiguration_new(void)
{
    struct qx_aconfig *c = (struct qx_aconfig *)calloc(1, sizeof(*c));
    if (c) {
        c->language[0] = 'e'; c->language[1] = 'n';
        c->country[0] = 'U'; c->country[1] = 'S';
        c->screen_width_dp = 1280;
        c->screen_height_dp = 720;
        c->screen_size = ACONFIGURATION_SCREENSIZE_NORMAL;
        c->nav_hidden = ACONFIGURATION_KEYSHIDDEN_NO;
    }
    return c;
}
static SYSV void qx_AConfiguration_delete(struct qx_aconfig *c) { free(c); }
static SYSV void qx_AConfiguration_fromAssetManager(struct qx_aconfig *c, void *mgr)
{
    (void)mgr;
    if (c) {
        struct qx_aconfig *d = qx_AConfiguration_new();
        if (d) { *c = *d; free(d); }
    }
}
static SYSV void qx_AConfiguration_getLanguage(const struct qx_aconfig *c, char *out)
{
    out[0] = c ? c->language[0] : 'e';
    out[1] = c ? c->language[1] : 'n';
}
static SYSV void qx_AConfiguration_getCountry(const struct qx_aconfig *c, char *out)
{
    out[0] = c ? c->country[0] : 'U';
    out[1] = c ? c->country[1] : 'S';
}
static SYSV int32_t qx_AConfiguration_getScreenWidthDp(const struct qx_aconfig *c) { return c ? c->screen_width_dp : 1280; }
static SYSV int32_t qx_AConfiguration_getScreenHeightDp(const struct qx_aconfig *c) { return c ? c->screen_height_dp : 720; }
static SYSV int32_t qx_AConfiguration_getScreenSize(const struct qx_aconfig *c) { return c ? c->screen_size : ACONFIGURATION_SCREENSIZE_NORMAL; }
static SYSV int32_t qx_AConfiguration_getNavHidden(const struct qx_aconfig *c) { return c ? c->nav_hidden : ACONFIGURATION_KEYSHIDDEN_NO; }

/* ---- AAssetManager / AAsset ----------------------------------------------- */
/* The asset root is a directory the runtime sets (the APK's extracted assets).
 * A single global manager stands in for the one the NativeActivity would hold. */
static wchar_t g_asset_root[QX_PATH_MAX];
static int g_asset_root_set;
static struct qx_assetmgr { int tag; } g_asset_mgr = {0xA55E7};

void qx_android_set_asset_dir(const wchar_t *dir)
{
    if (dir && wcslen(dir) < QX_PATH_MAX) {
        wcscpy(g_asset_root, dir);
        g_asset_root_set = 1;
    }
}
void *qx_android_asset_manager(void) { return &g_asset_mgr; }

struct qx_asset {
    unsigned char *data;
    int64_t len;
    int64_t pos;
};

static SYSV void *qx_AAssetManager_fromJava(void *env, void *obj)
{
    (void)env; (void)obj;
    return &g_asset_mgr; /* the engine has no real JVM here; hand it our manager */
}
static SYSV struct qx_asset *qx_AAssetManager_open(void *mgr, const char *filename, int mode)
{
    wchar_t path[QX_PATH_MAX], wname[QX_PATH_MAX];
    struct qx_asset *a;
    FILE *f;
    long sz;
    size_t i;
    (void)mgr; (void)mode;
    if (!g_asset_root_set || !filename)
        return NULL;
    /* asset names use '/'; build <root>\<name> with backslashes */
    for (i = 0; filename[i] && i < QX_PATH_MAX - 1; i++)
        wname[i] = (filename[i] == '/') ? L'\\' : (wchar_t)(unsigned char)filename[i];
    wname[i] = 0;
    if (qx_path_join(path, QX_PATH_MAX, g_asset_root, wname))
        return NULL;
    f = _wfopen(path, L"rb");
    if (!f)
        return NULL;
    fseek(f, 0, SEEK_END);
    sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz < 0) { fclose(f); return NULL; }
    a = (struct qx_asset *)calloc(1, sizeof(*a));
    if (!a) { fclose(f); return NULL; }
    a->data = (unsigned char *)malloc((size_t)sz ? (size_t)sz : 1);
    if (!a->data) { free(a); fclose(f); return NULL; }
    a->len = (int64_t)fread(a->data, 1, (size_t)sz, f);
    fclose(f);
    return a;
}
static SYSV const void *qx_AAsset_getBuffer(struct qx_asset *a) { return a ? a->data : NULL; }
static SYSV int64_t qx_AAsset_getLength(struct qx_asset *a) { return a ? a->len : 0; }
static SYSV void qx_AAsset_close(struct qx_asset *a)
{
    if (a) { free(a->data); free(a); }
}
static SYSV int qx_AAsset_openFileDescriptor(struct qx_asset *a, int64_t *start, int64_t *len)
{
    (void)a; (void)start; (void)len;
    return -1; /* assets are served from memory, not a shareable fd */
}

/* ---- ALooper -------------------------------------------------------------- */
#define ALOOPER_POLL_WAKE (-1)
#define ALOOPER_POLL_CALLBACK (-2)
#define ALOOPER_POLL_TIMEOUT (-3)
#define ALOOPER_POLL_ERROR (-4)
#define ALOOPER_PREPARE_ALLOW_NON_CALLBACKS 1

struct qx_looper {
    DWORD tid;
    long refs;
    HANDLE wake;      /* auto-reset event; ALooper_wake would signal it */
    int in_use;
};
#define QX_MAX_LOOPERS 64
static struct qx_looper g_loopers[QX_MAX_LOOPERS];
static CRITICAL_SECTION g_looper_lock;
static int g_looper_init;
static void looper_lock(void)
{
    if (!g_looper_init) { InitializeCriticalSection(&g_looper_lock); g_looper_init = 1; }
    EnterCriticalSection(&g_looper_lock);
}
static struct qx_looper *looper_for(DWORD tid, int create)
{
    int i, free_slot = -1;
    for (i = 0; i < QX_MAX_LOOPERS; i++) {
        if (g_loopers[i].in_use && g_loopers[i].tid == tid)
            return &g_loopers[i];
        if (!g_loopers[i].in_use && free_slot < 0)
            free_slot = i;
    }
    if (create && free_slot >= 0) {
        struct qx_looper *l = &g_loopers[free_slot];
        l->tid = tid;
        l->refs = 1;
        l->wake = CreateEventW(NULL, FALSE, FALSE, NULL);
        l->in_use = 1;
        return l;
    }
    return NULL;
}
static SYSV struct qx_looper *qx_ALooper_prepare(int opts)
{
    struct qx_looper *l;
    (void)opts;
    looper_lock();
    l = looper_for(GetCurrentThreadId(), 1);
    LeaveCriticalSection(&g_looper_lock);
    return l;
}
static SYSV struct qx_looper *qx_ALooper_forThread(void)
{
    struct qx_looper *l;
    looper_lock();
    l = looper_for(GetCurrentThreadId(), 0);
    LeaveCriticalSection(&g_looper_lock);
    return l;
}
static SYSV void qx_ALooper_acquire(struct qx_looper *l) { if (l) InterlockedIncrement(&l->refs); }
static SYSV void qx_ALooper_release(struct qx_looper *l)
{
    if (!l)
        return;
    if (InterlockedDecrement(&l->refs) <= 0) {
        looper_lock();
        if (l->wake) CloseHandle(l->wake);
        l->in_use = 0;
        l->wake = NULL;
        LeaveCriticalSection(&g_looper_lock);
    }
}
/* No registered fd becomes ready in this headless build, so pollOnce waits up
 * to the caller's timeout and reports a timeout. A negative timeout would block
 * forever on a real looper; we cap it so the engine's loop never wedges here. */
static SYSV int qx_ALooper_pollOnce(int timeoutMillis, int *outFd, int *outEvents, void **outData)
{
    struct qx_looper *l = qx_ALooper_forThread();
    DWORD wait = (timeoutMillis < 0) ? 50 : (DWORD)timeoutMillis;
    if (outFd) *outFd = 0;
    if (outEvents) *outEvents = 0;
    if (outData) *outData = NULL;
    if (l && l->wake)
        WaitForSingleObject(l->wake, wait);
    else
        Sleep(wait);
    return ALOOPER_POLL_TIMEOUT;
}
static SYSV int qx_ALooper_addFd(struct qx_looper *l, int fd, int ident, int events, void *cb, void *data)
{
    (void)l; (void)fd; (void)ident; (void)events; (void)cb; (void)data;
    return 1; /* accepted; readiness is not tracked in this build */
}
static SYSV int qx_ALooper_removeFd(struct qx_looper *l, int fd)
{
    (void)l; (void)fd;
    return 1;
}

/* ---- ANativeWindow -------------------------------------------------------- */
struct qx_native_window {
    long refs;
    int32_t width;
    int32_t height;
};
static struct qx_native_window g_window = {1, 1280, 720};

void qx_android_set_window_size(int w, int h)
{
    if (w > 0) g_window.width = w;
    if (h > 0) g_window.height = h;
}
void *qx_android_native_window(void) { return &g_window; }

static SYSV void qx_ANativeWindow_acquire(struct qx_native_window *w) { if (w) InterlockedIncrement(&w->refs); }
static SYSV void qx_ANativeWindow_release(struct qx_native_window *w) { if (w) InterlockedDecrement(&w->refs); }
static SYSV int32_t qx_ANativeWindow_getWidth(struct qx_native_window *w) { return w ? w->width : g_window.width; }
static SYSV int32_t qx_ANativeWindow_getHeight(struct qx_native_window *w) { return w ? w->height : g_window.height; }
static SYSV struct qx_native_window *qx_ANativeWindow_fromSurface(void *env, void *surface)
{
    (void)env; (void)surface;
    return &g_window; /* the one window; real surface binding is phase 5 */
}

/* ---- AHardwareBuffer / AImageReader / AImage (graphics-interop) ------------ */
/* Reached only deep in rendering; safe refcounted placeholders until phase 5. */
#define AMEDIA_ERROR_UNKNOWN (-10000)
struct qx_refcounted { long refs; };
static SYSV void qx_AHardwareBuffer_acquire(struct qx_refcounted *b) { if (b) InterlockedIncrement(&b->refs); }
static SYSV void qx_AHardwareBuffer_release(struct qx_refcounted *b) { if (b) InterlockedDecrement(&b->refs); }
static SYSV int qx_AImageReader_newWithUsage(int w, int h, int fmt, uint64_t usage, int maxImages, void **out)
{
    (void)w; (void)h; (void)fmt; (void)usage; (void)maxImages;
    if (out) *out = NULL;
    return AMEDIA_ERROR_UNKNOWN; /* no image reader without a GPU surface yet */
}
static SYSV void qx_AImageReader_delete(void *r) { (void)r; }
static SYSV int qx_AImageReader_getWindow(void *r, void **out)
{
    (void)r;
    if (out) *out = &g_window;
    return 0;
}
static SYSV int qx_AImageReader_acquireNextImage(void *r, void **out)
{
    (void)r;
    if (out) *out = NULL;
    return AMEDIA_ERROR_UNKNOWN; /* no frames are produced in this build */
}
static SYSV void qx_AImage_delete(void *im) { (void)im; }
static SYSV int qx_AImage_getHardwareBuffer(void *im, void **out)
{
    (void)im;
    if (out) *out = NULL;
    return AMEDIA_ERROR_UNKNOWN;
}
static SYSV int qx_AImage_getTimestamp(void *im, int64_t *ts)
{
    (void)im;
    if (ts) *ts = 0;
    return AMEDIA_ERROR_UNKNOWN;
}

/* ---- resolver -------------------------------------------------------------- */
struct android_sym {
    const char *name;
    void *fn;
};
static const struct android_sym g_android_table[] = {
    {"AConfiguration_new", (void *)qx_AConfiguration_new},
    {"AConfiguration_delete", (void *)qx_AConfiguration_delete},
    {"AConfiguration_fromAssetManager", (void *)qx_AConfiguration_fromAssetManager},
    {"AConfiguration_getLanguage", (void *)qx_AConfiguration_getLanguage},
    {"AConfiguration_getCountry", (void *)qx_AConfiguration_getCountry},
    {"AConfiguration_getScreenWidthDp", (void *)qx_AConfiguration_getScreenWidthDp},
    {"AConfiguration_getScreenHeightDp", (void *)qx_AConfiguration_getScreenHeightDp},
    {"AConfiguration_getScreenSize", (void *)qx_AConfiguration_getScreenSize},
    {"AConfiguration_getNavHidden", (void *)qx_AConfiguration_getNavHidden},
    {"AAssetManager_fromJava", (void *)qx_AAssetManager_fromJava},
    {"AAssetManager_open", (void *)qx_AAssetManager_open},
    {"AAsset_getBuffer", (void *)qx_AAsset_getBuffer},
    {"AAsset_getLength", (void *)qx_AAsset_getLength},
    {"AAsset_close", (void *)qx_AAsset_close},
    {"AAsset_openFileDescriptor", (void *)qx_AAsset_openFileDescriptor},
    {"ALooper_prepare", (void *)qx_ALooper_prepare},
    {"ALooper_forThread", (void *)qx_ALooper_forThread},
    {"ALooper_acquire", (void *)qx_ALooper_acquire},
    {"ALooper_release", (void *)qx_ALooper_release},
    {"ALooper_pollOnce", (void *)qx_ALooper_pollOnce},
    {"ALooper_addFd", (void *)qx_ALooper_addFd},
    {"ALooper_removeFd", (void *)qx_ALooper_removeFd},
    {"ANativeWindow_acquire", (void *)qx_ANativeWindow_acquire},
    {"ANativeWindow_release", (void *)qx_ANativeWindow_release},
    {"ANativeWindow_getWidth", (void *)qx_ANativeWindow_getWidth},
    {"ANativeWindow_getHeight", (void *)qx_ANativeWindow_getHeight},
    {"ANativeWindow_fromSurface", (void *)qx_ANativeWindow_fromSurface},
    {"AHardwareBuffer_acquire", (void *)qx_AHardwareBuffer_acquire},
    {"AHardwareBuffer_release", (void *)qx_AHardwareBuffer_release},
    {"AImageReader_newWithUsage", (void *)qx_AImageReader_newWithUsage},
    {"AImageReader_delete", (void *)qx_AImageReader_delete},
    {"AImageReader_getWindow", (void *)qx_AImageReader_getWindow},
    {"AImageReader_acquireNextImage", (void *)qx_AImageReader_acquireNextImage},
    {"AImage_delete", (void *)qx_AImage_delete},
    {"AImage_getHardwareBuffer", (void *)qx_AImage_getHardwareBuffer},
    {"AImage_getTimestamp", (void *)qx_AImage_getTimestamp},
};

void *qx_android_resolve(const char *name)
{
    size_t i;
    if (!name)
        return NULL;
    for (i = 0; i < sizeof(g_android_table) / sizeof(g_android_table[0]); i++)
        if (strcmp(name, g_android_table[i].name) == 0)
            return g_android_table[i].fn;
    return NULL;
}

/* ---- self-test ------------------------------------------------------------ */
SYSV int qx_android_selftest(int *total)
{
    int fails = 0, n = 0;

    /* AConfiguration carries the default locale and screen metrics */
    n++;
    {
        struct qx_aconfig *c = qx_AConfiguration_new();
        char lang[2], ctry[2];
        if (!c) {
            fails++;
        } else {
            qx_AConfiguration_getLanguage(c, lang);
            qx_AConfiguration_getCountry(c, ctry);
            if (lang[0] != 'e' || lang[1] != 'n' || ctry[0] != 'U' || ctry[1] != 'S' ||
                qx_AConfiguration_getScreenWidthDp(c) != 1280 ||
                qx_AConfiguration_getScreenHeightDp(c) != 720)
                fails++;
            qx_AConfiguration_delete(c);
        }
    }
    /* AAsset reads a file through the asset root as a byte buffer */
    n++;
    {
        wchar_t dir[QX_PATH_MAX], file[QX_PATH_MAX];
        DWORD tmplen = GetTempPathW(QX_PATH_MAX, dir);
        if (tmplen == 0 || tmplen >= QX_PATH_MAX) {
            fails++;
        } else {
            FILE *f;
            struct qx_asset *a;
            qx_path_join(file, QX_PATH_MAX, dir, L"quartix_asset_test.bin");
            f = _wfopen(file, L"wb");
            if (!f) {
                fails++;
            } else {
                fwrite("ROBLOX-ASSET", 1, 12, f);
                fclose(f);
                qx_android_set_asset_dir(dir);
                a = qx_AAssetManager_open(qx_android_asset_manager(), "quartix_asset_test.bin", 0);
                if (!a || qx_AAsset_getLength(a) != 12 ||
                    memcmp(qx_AAsset_getBuffer(a), "ROBLOX-ASSET", 12) != 0)
                    fails++;
                qx_AAsset_close(a);
                _wremove(file);
            }
        }
    }
    /* a missing asset returns NULL, not a crash */
    n++;
    if (qx_AAssetManager_open(qx_android_asset_manager(), "does/not/exist.bin", 0) != NULL)
        fails++;
    /* ALooper: prepare is per-thread and pollOnce returns promptly with a timeout */
    n++;
    {
        struct qx_looper *l = qx_ALooper_prepare(ALOOPER_PREPARE_ALLOW_NON_CALLBACKS);
        DWORD t0 = GetTickCount();
        int r = qx_ALooper_pollOnce(10, NULL, NULL, NULL);
        DWORD dt = GetTickCount() - t0;
        if (!l || l != qx_ALooper_forThread() || r != ALOOPER_POLL_TIMEOUT || dt > 2000)
            fails++;
        qx_ALooper_release(l);
    }
    /* ANativeWindow reports its dimensions and refcounts without crashing */
    n++;
    {
        struct qx_native_window *w = (struct qx_native_window *)qx_android_native_window();
        qx_android_set_window_size(800, 600);
        qx_ANativeWindow_acquire(w);
        if (qx_ANativeWindow_getWidth(w) != 800 || qx_ANativeWindow_getHeight(w) != 600)
            fails++;
        qx_ANativeWindow_release(w);
    }

    if (total)
        *total = n;
    return fails;
}

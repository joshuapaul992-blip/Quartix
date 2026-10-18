/*
 * Quartix - the window shown when Quartix.exe is started without arguments.
 * Shows the imported APK, imports a new one, and checks for and installs signed
 * updates on start. Slow work runs on a worker thread so the window stays live.
 * Copyright (C) 2026 Quartix contributors. Licensed under GPLv3 (see LICENSE).
 */
#include "quartix.h"
#include <commctrl.h>
#include <commdlg.h>
#include <shellapi.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <wchar.h>

#define WM_QX_UPDATE_DONE (WM_APP + 1)
#define WM_QX_IMPORT_DONE (WM_APP + 2)

enum { ID_IMPORT = 100, ID_CHECK, ID_DATA, ID_APK_TEXT, ID_UPDATE_TEXT };

static qx_paths *g_paths;
static HWND g_wnd, g_apk_text, g_update_text, g_import_btn, g_check_btn;
static HFONT g_font, g_bold;
static int g_busy;

struct update_result {
    int code;
    char version[32];
    char error[512];
};

struct import_job {
    wchar_t apk[QX_PATH_MAX];
    int ok;
    char error[512];
};

static void set_textf(HWND w, const wchar_t *fmt, ...)
{
    wchar_t buf[600];
    va_list ap;
    va_start(ap, fmt);
    _vsnwprintf(buf, sizeof(buf) / sizeof(buf[0]) - 1, fmt, ap);
    va_end(ap);
    buf[sizeof(buf) / sizeof(buf[0]) - 1] = 0;
    SetWindowTextW(w, buf);
}

static void set_busy(int busy)
{
    g_busy = busy;
    EnableWindow(g_import_btn, !busy);
    EnableWindow(g_check_btn, !busy);
}

/* Describes the active APK from apk\current and the size of its base.apk. */
static void refresh_apk_status(void)
{
    wchar_t path[QX_PATH_MAX], dir[QX_PATH_MAX], id[32];
    char raw[32];
    FILE *f;
    size_t n, i;
    WIN32_FILE_ATTRIBUTE_DATA fa;

    if (qx_path_join(path, QX_PATH_MAX, g_paths->apk_dir, L"current") || !(f = _wfopen(path, L"rb"))) {
        SetWindowTextW(g_apk_text, L"No APK imported yet. Import the x86_64 Roblox APK or APK bundle to get started.");
        return;
    }
    n = fread(raw, 1, sizeof(raw) - 1, f);
    fclose(f);
    for (i = 0; i < n && raw[i] != '\n' && raw[i] != '\r'; i++)
        id[i] = (wchar_t)raw[i];
    id[i] = 0;
    if (qx_path_join(dir, QX_PATH_MAX, g_paths->apk_dir, id) || qx_path_join(path, QX_PATH_MAX, dir, L"base.apk") ||
        !GetFileAttributesExW(path, GetFileExInfoStandard, &fa)) {
        SetWindowTextW(g_apk_text, L"The imported APK is missing. Import it again.");
        return;
    }
    set_textf(g_apk_text, L"Imported APK %ls (%.1f MB). Native library extracted.", id,
              ((double)fa.nFileSizeHigh * 4294967296.0 + fa.nFileSizeLow) / (1024.0 * 1024.0));
}

static DWORD WINAPI update_thread(void *arg)
{
    struct update_result *r = calloc(1, sizeof(*r));
    int check_only = (int)(INT_PTR)arg;
    if (r) {
        r->code = qx_update_online(g_paths, check_only, r->version);
        if (r->code < 0)
            snprintf(r->error, sizeof(r->error), "%s", qx_last_error());
    }
    PostMessageW(g_wnd, WM_QX_UPDATE_DONE, 0, (LPARAM)r);
    return 0;
}

static DWORD WINAPI import_thread(void *arg)
{
    struct import_job *j = arg;
    j->ok = qx_apk_import(g_paths, j->apk, NULL, 0) == 0;
    if (!j->ok)
        snprintf(j->error, sizeof(j->error), "%s", qx_last_error());
    PostMessageW(g_wnd, WM_QX_IMPORT_DONE, 0, (LPARAM)j);
    return 0;
}

static void start_thread(LPTHREAD_START_ROUTINE fn, void *arg)
{
    HANDLE t;
    set_busy(1);
    t = CreateThread(NULL, 0, fn, arg, 0, NULL);
    if (t)
        CloseHandle(t);
    else
        set_busy(0);
}

static void start_update(int check_only)
{
    if (!qx_update_has_key()) {
        SetWindowTextW(g_update_text, L"Updates are turned off in this build.");
        return;
    }
    SetWindowTextW(g_update_text, L"Checking for updates...");
    start_thread(update_thread, (void *)(INT_PTR)check_only);
}

static void relaunch(void)
{
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    memset(&si, 0, sizeof(si));
    si.cb = sizeof(si);
    /* Tells the new instance not to install again right away, so a release whose
     * signed version and built-in version disagree cannot cause a restart loop. */
    SetEnvironmentVariableW(L"QUARTIX_JUST_UPDATED", L"1");
    if (CreateProcessW(g_paths->exe_path, NULL, NULL, NULL, FALSE, 0, NULL, g_paths->install_dir, &si, &pi)) {
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        DestroyWindow(g_wnd);
    }
}

static void on_update_done(struct update_result *r)
{
    set_busy(0);
    if (!r) {
        SetWindowTextW(g_update_text, L"Update check failed: out of memory.");
        return;
    }
    switch (r->code) {
    case QX_UPDATE_NONE:
        set_textf(g_update_text, L"Version %ls. No release has been published yet.", QX_VERSION);
        break;
    case QX_UPDATE_CURRENT:
        set_textf(g_update_text, L"Version %ls. Quartix is up to date.", QX_VERSION);
        break;
    case QX_UPDATE_AVAILABLE:
        set_textf(g_update_text, L"Version %hs is available.", r->version);
        break;
    case QX_UPDATE_INSTALLED:
        set_textf(g_update_text, L"Updated to %hs. Restarting...", r->version);
        UpdateWindow(g_update_text);
        Sleep(800);
        relaunch();
        break;
    default:
        set_textf(g_update_text, L"Could not check for updates: %hs", r->error[0] ? r->error : "unknown error");
        break;
    }
    free(r);
}

static void on_import_done(struct import_job *j)
{
    set_busy(0);
    if (j->ok)
        refresh_apk_status();
    else
        set_textf(g_apk_text, L"Import failed: %hs", j->error[0] ? j->error : "unknown error");
    free(j);
}

static void choose_apk(void)
{
    OPENFILENAMEW ofn;
    struct import_job *j = calloc(1, sizeof(*j));
    if (!j)
        return;
    memset(&ofn, 0, sizeof(ofn));
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = g_wnd;
    ofn.lpstrFilter = L"APKs and APK bundles (*.apk, *.apks, *.xapk, *.apkm)\0*.apk;*.apks;*.xapk;*.apkm\0All files\0*.*\0";
    ofn.lpstrFile = j->apk;
    ofn.nMaxFile = QX_PATH_MAX;
    ofn.lpstrTitle = L"Import the x86_64 Roblox APK or APK bundle";
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_HIDEREADONLY;
    if (!GetOpenFileNameW(&ofn)) {
        free(j);
        return;
    }
    SetWindowTextW(g_apk_text, L"Importing... this copies the APK into the data folder.");
    start_thread(import_thread, j);
}

static HWND add(const wchar_t *cls, const wchar_t *text, DWORD style, int x, int y, int w, int h, int id, HFONT font)
{
    HWND c = CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | style, x, y, w, h, g_wnd,
                             (HMENU)(INT_PTR)id, GetModuleHandleW(NULL), NULL);
    SendMessageW(c, WM_SETFONT, (WPARAM)font, TRUE);
    return c;
}

static void create_controls(void)
{
    add(L"BUTTON", L"Roblox APK", BS_GROUPBOX, 10, 8, 440, 92, 0, g_bold);
    g_apk_text = add(L"STATIC", L"", SS_LEFT, 22, 30, 416, 34, ID_APK_TEXT, g_font);
    g_import_btn = add(L"BUTTON", L"Import APK...", BS_PUSHBUTTON | WS_TABSTOP, 22, 66, 110, 24, ID_IMPORT, g_font);

    add(L"BUTTON", L"Updates", BS_GROUPBOX, 10, 108, 440, 92, 0, g_bold);
    g_update_text = add(L"STATIC", L"", SS_LEFT, 22, 130, 416, 34, ID_UPDATE_TEXT, g_font);
    g_check_btn = add(L"BUTTON", L"Check now", BS_PUSHBUTTON | WS_TABSTOP, 22, 166, 110, 24, ID_CHECK, g_font);

    add(L"BUTTON", L"Open data folder", BS_PUSHBUTTON | WS_TABSTOP, 10, 212, 120, 24, ID_DATA, g_font);
    add(L"STATIC", L"Login data lives in the data folder and is kept across updates.", SS_LEFT | SS_CENTERIMAGE,
        140, 212, 310, 24, 0, g_font);
}

static LRESULT CALLBACK wnd_proc(HWND w, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case ID_IMPORT:
            if (!g_busy)
                choose_apk();
            return 0;
        case ID_CHECK:
            if (!g_busy)
                start_update(0);
            return 0;
        case ID_DATA:
            ShellExecuteW(w, L"open", g_paths->data_dir, NULL, NULL, SW_SHOWNORMAL);
            return 0;
        }
        break;
    case WM_QX_UPDATE_DONE:
        on_update_done((struct update_result *)lp);
        return 0;
    case WM_QX_IMPORT_DONE:
        on_import_done((struct import_job *)lp);
        return 0;
    case WM_CLOSE:
        if (g_busy && MessageBoxW(w, L"Quartix is still working. Close anyway?", L"Quartix",
                                  MB_YESNO | MB_ICONQUESTION) != IDYES)
            return 0;
        DestroyWindow(w);
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(w, msg, wp, lp);
}

/* When started from Explorer, Quartix owns its console window; hide it. When
 * started from a command prompt, the console is shared, so leave it alone. */
static void hide_own_console(void)
{
    DWORD procs[2];
    HWND con = GetConsoleWindow();
    if (con && GetConsoleProcessList(procs, 2) == 1)
        ShowWindow(con, SW_HIDE);
}

static void make_fonts(void)
{
    NONCLIENTMETRICSW ncm;
    LOGFONTW bold;
    memset(&ncm, 0, sizeof(ncm));
    ncm.cbSize = sizeof(ncm);
    if (SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0)) {
        g_font = CreateFontIndirectW(&ncm.lfMessageFont);
        bold = ncm.lfMessageFont;
        bold.lfWeight = FW_BOLD;
        g_bold = CreateFontIndirectW(&bold);
    }
    if (!g_font)
        g_font = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
    if (!g_bold)
        g_bold = g_font;
}

int qx_gui_run(qx_paths *p)
{
    INITCOMMONCONTROLSEX icc;
    WNDCLASSEXW wc;
    RECT rc = {0, 0, 460, 246};
    DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    wchar_t title[64];
    MSG msg;

    g_paths = p;
    hide_own_console();
    icc.dwSize = sizeof(icc);
    icc.dwICC = ICC_STANDARD_CLASSES;
    InitCommonControlsEx(&icc);
    make_fonts();

    memset(&wc, 0, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = wnd_proc;
    wc.hInstance = GetModuleHandleW(NULL);
    wc.hIcon = LoadIconW(wc.hInstance, MAKEINTRESOURCEW(1));
    if (!wc.hIcon)
        wc.hIcon = LoadIconW(NULL, (LPCWSTR)IDI_APPLICATION);
    wc.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = L"QuartixWindow";
    if (!RegisterClassExW(&wc))
        return 1;

    AdjustWindowRect(&rc, style, FALSE);
    _snwprintf(title, sizeof(title) / sizeof(title[0]), L"Quartix %ls", QX_VERSION);
    g_wnd = CreateWindowExW(0, wc.lpszClassName, title, style, CW_USEDEFAULT, CW_USEDEFAULT,
                            rc.right - rc.left, rc.bottom - rc.top, NULL, NULL, wc.hInstance, NULL);
    if (!g_wnd)
        return 1;
    create_controls();
    refresh_apk_status();
    ShowWindow(g_wnd, SW_SHOWNORMAL);
    UpdateWindow(g_wnd);

    /* Updates are automatic: check and install on every start, except right
     * after an update, when we only check. */
    if (GetEnvironmentVariableW(L"QUARTIX_JUST_UPDATED", NULL, 0)) {
        SetEnvironmentVariableW(L"QUARTIX_JUST_UPDATED", NULL);
        start_update(1);
    } else {
        start_update(0);
    }

    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        if (!IsDialogMessageW(g_wnd, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    return 0;
}

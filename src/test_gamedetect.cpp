/*
 * test_gamedetect.cpp - prints the foreground window plus the verdict that the
 * overlay's own game heuristic reaches for it. The heuristic is not copied: it
 * is called through ts3ovl::OverlayGameForegroundNow(), so this probe and the
 * overlay can never drift apart.
 *
 * usage: test_gamedetect.exe [seconds]   (default: print once)
 */
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <cwctype>
#include <cstdio>
#include <cstdlib>
#include <string>

#include "overlay.h"

static std::wstring ProcessExeName(DWORD pid)
{
    std::wstring result;
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!h) return result;

    wchar_t path[MAX_PATH * 2] = {0};
    DWORD len = (DWORD)(sizeof(path) / sizeof(path[0]));
    if (QueryFullProcessImageNameW(h, 0, path, &len) && len > 0) {
        const wchar_t* base = path;
        for (const wchar_t* p = path; *p; ++p) {
            if (*p == L'\\' || *p == L'/') base = p + 1;
        }
        for (const wchar_t* p = base; *p; ++p) result.push_back((wchar_t)towlower(*p));
    }
    CloseHandle(h);
    return result;
}

static void Report()
{
    HWND fg = GetForegroundWindow();
    if (!fg) { printf("foreground: (none)\n"); return; }

    wchar_t title[256] = {0};
    GetWindowTextW(fg, title, 256);
    wchar_t cls[64] = {0};
    GetClassNameW(fg, cls, 64);

    DWORD pid = 0;
    GetWindowThreadProcessId(fg, &pid);
    std::wstring exe = ProcessExeName(pid);

    RECT wr;
    ZeroMemory(&wr, sizeof(wr));
    GetWindowRect(fg, &wr);
    HMONITOR mon = MonitorFromWindow(fg, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi;
    ZeroMemory(&mi, sizeof(mi));
    mi.cbSize = sizeof(mi);
    GetMonitorInfoW(mon, &mi);

    LONG_PTR style   = GetWindowLongPtrW(fg, GWL_STYLE);
    LONG_PTR exStyle = GetWindowLongPtrW(fg, GWL_EXSTYLE);

    printf("title     : %ls\n", title);
    printf("class     : %ls\n", cls);
    printf("exe       : %ls\n", exe.c_str());
    printf("rect      : %ld,%ld %ldx%ld\n", wr.left, wr.top,
           wr.right - wr.left, wr.bottom - wr.top);
    printf("monitor   : %ldx%ld\n",
           mi.rcMonitor.right - mi.rcMonitor.left,
           mi.rcMonitor.bottom - mi.rcMonitor.top);
    printf("style     : caption=%d thickframe=%d toolwindow=%d topmost=%d\n",
           (style & WS_CAPTION) == WS_CAPTION ? 1 : 0,
           (style & WS_THICKFRAME) ? 1 : 0,
           (exStyle & WS_EX_TOOLWINDOW) ? 1 : 0,
           (exStyle & WS_EX_TOPMOST) ? 1 : 0);
    printf("VERDICT   : %s\n", ts3ovl::OverlayGameForegroundNow() ? "GAME (overlay shows)"
                                                                 : "not a game (overlay hides)");
    printf("\n");
}

static BOOL CALLBACK AuditProc(HWND hwnd, LPARAM lParam)
{
    int* count = (int*)lParam;
    if (!IsWindowVisible(hwnd)) return TRUE;

    wchar_t title[256] = {0};
    GetWindowTextW(hwnd, title, 256);
    if (!title[0]) return TRUE;

    if (!ts3ovl::OverlayClassifyWindow((unsigned long long)(uintptr_t)hwnd)) return TRUE;

    wchar_t cls[64] = {0};
    GetClassNameW(hwnd, cls, 64);
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    std::wstring exe = ProcessExeName(pid);

    RECT wr;
    ZeroMemory(&wr, sizeof(wr));
    GetWindowRect(hwnd, &wr);

    printf("  GAME?  %-22ls %4ldx%-4ld  %-28ls %ls\n",
           exe.c_str(), wr.right - wr.left, wr.bottom - wr.top, cls, title);
    ++(*count);
    return TRUE;
}

int main(int argc, char** argv)
{
    if (argc > 1 && !_stricmp(argv[1], "-all")) {
        printf("visible windows the heuristic classifies as GAME (false positives show up here):\n");
        int count = 0;
        EnumWindows(AuditProc, (LPARAM)&count);
        printf("total: %d\n", count);
        return 0;
    }

    int times = 1;
    if (argc > 1) times = atoi(argv[1]);
    if (times < 1) times = 1;
    for (int i = 0; i < times; ++i) {
        if (times > 1) printf("--- sample %d ---\n", i + 1);
        Report();
        if (i + 1 < times) Sleep(1000);
    }
    return 0;
}

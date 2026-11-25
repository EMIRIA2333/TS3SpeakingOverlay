// zorder.cpp - walk the real desktop z-order from top to bottom and report
// class / pid / exe / rect / exstyle / visible / cloaked for interesting windows.
// usage: zorder.exe [max]
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <psapi.h>
#include <dwmapi.h>
#include <string>
#include <cstdio>

static std::wstring ExeOf(DWORD pid)
{
    std::wstring out;
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!h) return out;
    wchar_t buf[MAX_PATH * 2] = {0};
    DWORD n = (DWORD)(sizeof(buf) / sizeof(buf[0]));
    if (QueryFullProcessImageNameW(h, 0, buf, &n)) {
        const wchar_t* slash = wcsrchr(buf, L'\\');
        out = slash ? (slash + 1) : buf;
    }
    CloseHandle(h);
    return out;
}

int wmain(int argc, wchar_t** argv)
{
    int max = (argc > 1) ? _wtoi(argv[1]) : 80;
    if (max <= 0) max = 80;

    HWND h = GetTopWindow(NULL);
    // GetTopWindow(NULL) returns the first child of the desktop... use the
    // documented walk: GetWindow(hwnd, GW_HWNDFIRST) on the desktop window.
    HWND desktop = GetDesktopWindow();
    h = GetWindow(desktop, GW_CHILD);
    h = GetWindow(h, GW_HWNDLAST);   // start from the topmost window
    int i = 0;
    int shown = 0;
    while (h && shown < max) {
        ++i;
        wchar_t cls[256] = {0}, title[256] = {0};
        GetClassNameW(h, cls, 256);
        GetWindowTextW(h, title, 256);
        RECT r; GetWindowRect(h, &r);
        DWORD pid = 0;
        GetWindowThreadProcessId(h, &pid);
        LONG_PTR ex = GetWindowLongPtrW(h, GWL_EXSTYLE);
        BOOL vis = IsWindowVisible(h);
        int cloaked = -1;
        DwmGetWindowAttribute(h, 14 /*DWMWA_CLOAKED*/, &cloaked, sizeof(cloaked));
        bool topmost = (ex & WS_EX_TOPMOST) != 0;
        if (vis && !cloaked) {
            wprintf(L"z=%02d hwnd=%p top=%d lay=%d cls=%-32ls pid=%-6lu exe=%-24ls rect=%ld,%ld,%ld,%ld title=%ls\n",
                    i, (void*)h, topmost ? 1 : 0, (ex & WS_EX_LAYERED) ? 1 : 0,
                    cls, pid, ExeOf(pid).c_str(), r.left, r.top, r.right, r.bottom, title);
            ++shown;
        }
        h = GetWindow(h, GW_HWNDPREV);
    }
    return 0;
}

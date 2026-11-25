// zenum.cpp - list top-level windows in real Z order (top first) using EnumWindows,
// which enumerates in Z order, and report class / pid / exe / rect / exstyle / cloaked.
// usage: zenum.exe [filter]
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <dwmapi.h>
#include <string>
#include <cstdio>
#include <cwchar>

struct Row { HWND h; }; // not used

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

static const wchar_t* g_filter = NULL;

static BOOL CALLBACK Proc(HWND h, LPARAM lp)
{
    (void)lp;
    static int index = 0;
    ++index;
    wchar_t cls[256] = {0}, title[200] = {0};
    GetClassNameW(h, cls, 256);
    GetWindowTextW(h, title, 200);
    DWORD pid = 0;
    GetWindowThreadProcessId(h, &pid);
    std::wstring exe = ExeOf(pid);

    bool match = true;
    if (g_filter && *g_filter) {
        match = (wcsstr(cls, g_filter) != NULL) || (wcsstr(exe.c_str(), g_filter) != NULL);
    }
    LONG_PTR ex = GetWindowLongPtrW(h, GWL_EXSTYLE);
    BOOL vis = IsWindowVisible(h);
    int cloaked = 0;
    DwmGetWindowAttribute(h, 14, &cloaked, sizeof(cloaked));
    if (match) {
        RECT r; GetWindowRect(h, &r);
        wprintf(L"z=%03d hwnd=%p vis=%d cloak=%d top=%d lay=%d ex=0x%08llX cls=%-30ls exe=%-22ls rect=%ld,%ld,%ld,%ld | %ls\n",
                index, (void*)h, vis ? 1 : 0, cloaked, (ex & WS_EX_TOPMOST) ? 1 : 0,
                (ex & WS_EX_LAYERED) ? 1 : 0, (unsigned long long)ex, cls, exe.c_str(),
                r.left, r.top, r.right, r.bottom, title);
    }
    return TRUE;
}

int wmain(int argc, wchar_t** argv)
{
    g_filter = (argc > 1) ? argv[1] : NULL;
    EnumWindows(Proc, 0);
    return 0;
}

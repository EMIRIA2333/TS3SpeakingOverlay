// pixprobe.cpp - sample screen pixels and report the overlay window state.
// usage: pixprobe.exe <x> <y> [verbose]
// Prints the RGB at the given screen point, plus overlay window visible flag,
// its rect, exstyle, and whether DWM considers it cloaked.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <dwmapi.h>
#include <cstdio>

static HWND g_ovl = NULL;

static BOOL CALLBACK Proc(HWND h, LPARAM)
{
    wchar_t cls[256] = {0};
    GetClassNameW(h, cls, 256);
    if (wcscmp(cls, L"TS3SpeakingOverlayWnd") == 0) { g_ovl = h; }
    return TRUE;
}

int wmain(int argc, wchar_t** argv)
{
    if (argc < 3) { wprintf(L"usage: pixprobe.exe <x> <y>\n"); return 2; }
    int x = _wtoi(argv[1]), y = _wtoi(argv[2]);

    HDC dc = GetDC(NULL);
    COLORREF c = GetPixel(dc, x, y);
    ReleaseDC(NULL, dc);
    int r = GetRValue(c), g = GetGValue(c), b = GetBValue(c);

    EnumWindows(Proc, 0);
    if (!g_ovl) { wprintf(L"rgb=%d,%d,%d overlay=none\n", r, g, b); return 0; }

    RECT rc; GetWindowRect(g_ovl, &rc);
    LONG_PTR ex = GetWindowLongPtrW(g_ovl, GWL_EXSTYLE);
    int cloaked = 0;
    DwmGetWindowAttribute(g_ovl, 14, &cloaked, sizeof(cloaked));
    HWND fg = GetForegroundWindow();
    wchar_t fcls[256] = {0};
    if (fg) GetClassNameW(fg, fcls, 256);
    wprintf(L"rgb=%d,%d,%d vis=%d cloak=%d top=%d rect=%ld,%ld,%ld,%ld fg=%ls\n",
            r, g, b, IsWindowVisible(g_ovl) ? 1 : 0, cloaked,
            (ex & WS_EX_TOPMOST) ? 1 : 0, rc.left, rc.top, rc.right, rc.bottom, fcls);
    return 0;
}

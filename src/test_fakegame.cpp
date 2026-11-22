/*
 * test_fakegame.cpp - creates a borderless fullscreen window that looks like a
 * game to the overlay's heuristic, so the "show while in game" branch can be
 * verified without actually launching a game.
 *
 * The verdict is printed through the overlay's own classifier
 * (ts3ovl::OverlayClassifyWindow / OverlayGameForegroundNow), so this never
 * disagrees with what the overlay actually does.
 *
 * usage: test_fakegame.exe [seconds] [windowed]
 *   default      : borderless fullscreen over the primary monitor
 *   windowed     : 70% of the monitor, still captionless/borderless
 */
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "overlay.h"

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg) {
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    case WM_KEYDOWN:
        if (wParam == VK_ESCAPE) { PostQuitMessage(0); return 0; }
        break;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        RECT rc;
        GetClientRect(hwnd, &rc);
        HBRUSH bg = CreateSolidBrush(RGB(12, 20, 34));
        FillRect(dc, &rc, bg);
        DeleteObject(bg);
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, RGB(120, 220, 160));
        HFONT font = CreateFontW(-32, 0, 0, 0, FW_BOLD, 0, 0, 0, DEFAULT_CHARSET,
                                 OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                 CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
        HGDIOBJ old = SelectObject(dc, font);
        DrawTextW(dc, L"FAKE GAME WINDOW  (Esc to close)", -1, &rc,
                  DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        SelectObject(dc, old);
        DeleteObject(font);
        EndPaint(hwnd, &ps);
        return 0;
    }
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

int main(int argc, char** argv)
{
    int seconds = 20;
    bool windowed = false;
    if (argc > 1) seconds = atoi(argv[1]);
    if (argc > 2 && !_stricmp(argv[2], "windowed")) windowed = true;
    if (seconds <= 0) seconds = 20;

    HINSTANCE hInst = GetModuleHandleW(NULL);

    WNDCLASSEXW wc;
    ZeroMemory(&wc, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.lpszClassName = L"FakeGameWindow";
    if (!RegisterClassExW(&wc)) { printf("RegisterClass failed %lu\n", GetLastError()); return 1; }

    int x = 0, y = 0, w = 1280, h = 720;
    MONITORINFO mi;
    ZeroMemory(&mi, sizeof(mi));
    mi.cbSize = sizeof(mi);
    if (GetMonitorInfoW(MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY), &mi)) {
        const RECT& r = mi.rcMonitor;
        w = r.right - r.left;
        h = r.bottom - r.top;
        if (windowed) { w = w * 7 / 10; h = h * 7 / 10; }
        x = r.left + ((r.right - r.left) - w) / 2;
        y = r.top + ((r.bottom - r.top) - h) / 2;
    }

    HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"Fake Game",
                                WS_POPUP | WS_VISIBLE,
                                x, y, w, h, NULL, NULL, hInst, NULL);
    if (!hwnd) { printf("CreateWindow failed %lu\n", GetLastError()); return 1; }

    SetWindowPos(hwnd, HWND_TOP, x, y, w, h, SWP_SHOWWINDOW);
    SetForegroundWindow(hwnd);
    SetTimer(hwnd, 1, (UINT)seconds * 1000, NULL);

    Sleep(400);
    printf("fake game hwnd   : %p\n", (void*)hwnd);
    printf("windowed mode    : %s\n", windowed ? "yes" : "no");
    printf("size             : %dx%d\n", w, h);
    printf("VERDICT (direct) : %s\n",
           ts3ovl::OverlayClassifyWindow((unsigned long long)(uintptr_t)hwnd)
               ? "GAME (overlay shows)" : "not a game (overlay hides)");
    printf("foreground now   : %s\n",
           ts3ovl::OverlayGameForegroundNow() ? "GAME (overlay shows)"
                                              : "not a game (overlay hides)");
    fflush(stdout);

    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        if (msg.message == WM_TIMER && msg.hwnd == hwnd) { PostQuitMessage(0); break; }
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return 0;
}

/*
 * ts3-speaking-overlay - always-on-top "who is talking" overlay (KOOK style)
 * settings.cpp : native settings window
 *
 * Opened from the TS3 client through
 *   Options -> Addons -> "Speaking Overlay" -> Settings
 * which the client implements by calling ts3plugin_configure(handle, qParentWidget).
 *
 * Threading: ts3plugin_offersConfigure() returns
 * PLUGIN_OFFERS_CONFIGURE_NEW_THREAD, i.e. the client runs ts3plugin_configure
 * on a thread it creates for us and expects the call to block for as long as
 * the window is open. A window without a message pump would just die, so
 * SettingsShow() owns a modal message loop and returns when the window closes.
 *
 * The three switches are plain checkboxes whose state is read back from the
 * overlay module every time the window is shown, so the window can never show
 * a stale value. Toggling a checkbox only calls the existing public overlay
 * API; persistence already happens in there.
 */
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif

#include <windows.h>
#include <commctrl.h>

#include <string>

#include "overlay.h"
#include "settings.h"

namespace ts3ovl {

namespace {

const wchar_t* const kSettingsClass = L"TS3SpeakingOverlaySettingsWnd";
const wchar_t* const kSettingsTitle = L"Speaking Overlay 设置";

/* control ids */
enum {
    IDC_CHK_VISIBLE      = 2001,
    IDC_CHK_ONLY_IN_GAME = 2002,
    IDC_CHK_LOCKED       = 2003,
    IDC_BTN_RESET        = 2004,
    IDC_STATUS_TEXT      = 2005,
    IDC_HINT_TEXT        = 2006
};

/* timer id used to keep the status line live */
const UINT_PTR kTimerSync = 1;

HWND g_hwnd          = NULL;
HWND g_chkVisible    = NULL;
HWND g_chkOnlyInGame = NULL;
HWND g_chkLocked     = NULL;
HWND g_btnReset      = NULL;
HWND g_statusText    = NULL;
HWND g_hintText      = NULL;
HFONT g_font         = NULL;
DWORD g_threadId     = 0;      /* thread that owns g_hwnd */

/* Fill the checkboxes from the live overlay state. */
void SyncFromOverlay()
{
    if (g_chkVisible) {
        SendMessageW(g_chkVisible, BM_SETCHECK,
                     OverlayVisibleEnabled() ? BST_CHECKED : BST_UNCHECKED, 0);
    }
    if (g_chkOnlyInGame) {
        SendMessageW(g_chkOnlyInGame, BM_SETCHECK,
                     OverlayOnlyInGameEnabled() ? BST_CHECKED : BST_UNCHECKED, 0);
    }
    if (g_chkLocked) {
        SendMessageW(g_chkLocked, BM_SETCHECK,
                     OverlayIsLocked() ? BST_CHECKED : BST_UNCHECKED, 0);
    }

    /* One line of live status underneath the switches. */
    if (g_statusText) {
        wchar_t msg[256];
        const wchar_t* visible = OverlayIsShowing() ? L"是" : L"否";
        const wchar_t* hidden  = OverlayHiddenByForeground() ? L"（被『仅游戏内显示』隐藏）" : L"";
        wsprintfW(msg, L"当前是否可见：%s%s", visible, hidden);
        SetWindowTextW(g_statusText, msg);
    }
}

bool Checked(HWND chk)
{
    return chk && SendMessageW(chk, BM_GETCHECK, 0, 0) == BST_CHECKED;
}

/* The overlay API exposes toggles, not setters, so a checkbox only calls the
 * toggle when its value actually differs from the current state. */
void ApplyOne(bool want, bool have, void (*toggle)())
{
    if (want != have && toggle) toggle();
}

void ApplyAll()
{
    ApplyOne(Checked(g_chkVisible),    OverlayVisibleEnabled(),    &OverlayToggleVisible);
    ApplyOne(Checked(g_chkOnlyInGame), OverlayOnlyInGameEnabled(), &OverlayToggleOnlyInGame);
    ApplyOne(Checked(g_chkLocked),     OverlayIsLocked(),          &OverlayToggleLock);
    /* Persist immediately: overlay.ini is otherwise only written on drag. */
    OverlaySaveSettings();
}

void LayoutChildren(HWND hwnd)
{
    RECT rc;
    GetClientRect(hwnd, &rc);
    const int pad = 20;
    const int w   = rc.right - rc.left;
    int y = pad;

    const int chkW = w - pad * 2;
    const int chkH = 26;

    if (g_chkVisible)    { MoveWindow(g_chkVisible,    pad, y, chkW, chkH, TRUE); y += chkH + 6;  }
    if (g_chkOnlyInGame) { MoveWindow(g_chkOnlyInGame, pad, y, chkW, chkH, TRUE); y += chkH + 6;  }
    if (g_chkLocked)     { MoveWindow(g_chkLocked,     pad, y, chkW, chkH, TRUE); y += chkH + 16; }
    if (g_btnReset)      { MoveWindow(g_btnReset,      pad, y, 150, 30, TRUE);   y += 30 + 16;   }
    if (g_statusText)    { MoveWindow(g_statusText,    pad, y, chkW, 22, TRUE);  y += 22 + 8;    }
    if (g_hintText)      { MoveWindow(g_hintText,      pad, y, chkW, 40, TRUE);                  }
}

LRESULT CALLBACK SettingsProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg) {
    case WM_CREATE: {
        HINSTANCE inst = (HINSTANCE)GetWindowLongPtrW(hwnd, GWLP_HINSTANCE);

        g_chkVisible = CreateWindowExW(0, L"BUTTON", L"显示悬浮窗",
                                       WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
                                       0, 0, 0, 0, hwnd, (HMENU)(INT_PTR)IDC_CHK_VISIBLE, inst, NULL);
        g_chkOnlyInGame = CreateWindowExW(0, L"BUTTON", L"只在游戏内显示（前台不是游戏时隐藏）",
                                          WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
                                          0, 0, 0, 0, hwnd, (HMENU)(INT_PTR)IDC_CHK_ONLY_IN_GAME, inst, NULL);
        g_chkLocked = CreateWindowExW(0, L"BUTTON", L"锁定位置（锁定后鼠标可穿透）",
                                      WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
                                      0, 0, 0, 0, hwnd, (HMENU)(INT_PTR)IDC_CHK_LOCKED, inst, NULL);
        g_btnReset = CreateWindowExW(0, L"BUTTON", L"重置浮窗位置",
                                     WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                                     0, 0, 0, 0, hwnd, (HMENU)(INT_PTR)IDC_BTN_RESET, inst, NULL);
        g_statusText = CreateWindowExW(0, L"STATIC", L"",
                                       WS_CHILD | WS_VISIBLE | SS_LEFT,
                                       0, 0, 0, 0, hwnd, (HMENU)(INT_PTR)IDC_STATUS_TEXT, inst, NULL);
        g_hintText = CreateWindowExW(0, L"STATIC",
                                     L"提示：拖动浮窗可移动位置，松手后会自动吸附到屏幕边缘。",
                                     WS_CHILD | WS_VISIBLE | SS_LEFT,
                                     0, 0, 0, 0, hwnd, (HMENU)(INT_PTR)IDC_HINT_TEXT, inst, NULL);

        NONCLIENTMETRICSW ncm;
        ZeroMemory(&ncm, sizeof(ncm));
        ncm.cbSize = sizeof(ncm);
        if (SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0)) {
            g_font = CreateFontIndirectW(&ncm.lfMessageFont);
        }
        if (!g_font) {
            g_font = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
        }

        HWND kids[] = { g_chkVisible, g_chkOnlyInGame, g_chkLocked, g_btnReset, g_statusText, g_hintText };
        for (HWND k : kids) {
            if (k && g_font) SendMessageW(k, WM_SETFONT, (WPARAM)g_font, TRUE);
        }

        LayoutChildren(hwnd);
        SyncFromOverlay();
        return 0;
    }

    case WM_SIZE:
        LayoutChildren(hwnd);
        return 0;

    case WM_GETMINMAXINFO: {
        MINMAXINFO* mmi = (MINMAXINFO*)lParam;
        mmi->ptMinTrackSize.x = 400;
        mmi->ptMinTrackSize.y = 300;
        return 0;
    }

    case WM_COMMAND: {
        const int id   = LOWORD(wParam);
        const int code = HIWORD(wParam);

        if (id == IDC_BTN_RESET && code == BN_CLICKED) {
            OverlayResetPosition();
            SyncFromOverlay();
            return 0;
        }
        if ((id == IDC_CHK_VISIBLE || id == IDC_CHK_ONLY_IN_GAME || id == IDC_CHK_LOCKED)
            && code == BN_CLICKED) {
            ApplyAll();
            /* Read back from the overlay: a toggle may have been refused
             * (e.g. no window yet) and the status line must stay honest. */
            SyncFromOverlay();
            return 0;
        }
        return 0;
    }

    case WM_TIMER:
        if (wParam == kTimerSync) {
            SyncFromOverlay();
            return 0;
        }
        break;

    case WM_CLOSE:
        /* Destroy instead of hide: the modal loop in SettingsShow() ends with
         * the window. Hiding it would leave configure() blocked forever. */
        DestroyWindow(hwnd);
        return 0;

    case WM_DESTROY:
        KillTimer(hwnd, kTimerSync);
        g_chkVisible = g_chkOnlyInGame = g_chkLocked = NULL;
        g_btnReset = g_statusText = g_hintText = NULL;
        g_hwnd = NULL;
        return 0;

    default:
        break;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

bool EnsureClass(HINSTANCE inst)
{
    static bool registered = false;
    if (registered) return true;

    WNDCLASSEXW wc;
    ZeroMemory(&wc, sizeof(wc));
    wc.cbSize        = sizeof(wc);
    wc.style         = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc   = SettingsProc;
    wc.hInstance     = inst;
    wc.hCursor       = LoadCursorW(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = kSettingsClass;
    wc.hIcon         = LoadIconW(inst, L"IDI_APP");
    if (!wc.hIcon) wc.hIcon = LoadIconW(NULL, IDI_APPLICATION);

    if (!RegisterClassExW(&wc)) {
        if (GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return false;
    }
    registered = true;
    return true;
}

} // namespace

/* ------------------------------------------------------------------------- */

/* Thread entry used by SettingsShowAsync: SettingsShow runs a modal message
 * loop, which is fine on a private thread but would freeze TeamSpeak if it ran
 * on the client's Qt GUI thread. */
static DWORD WINAPI AsyncThread(LPVOID)
{
    SettingsShow(NULL);
    return 0;
}

void SettingsShow(void* owner)
{
    /* Re-entrancy: the client can call configure again while the window is
     * already open. Just raise it and return, do not start a second loop. */
    if (g_hwnd && IsWindow(g_hwnd)) {
        SyncFromOverlay();
        ShowWindow(g_hwnd, SW_SHOWNORMAL);
        SetForegroundWindow(g_hwnd);
        return;
    }

    HINSTANCE inst = GetModuleHandleW(NULL);
    if (!EnsureClass(inst)) return;

    /* IMPORTANT: `owner` is the Qt QWidget* the client passes to
     * ts3plugin_configure - it is NOT an HWND. Handing it to CreateWindowExW as
     * the parent makes the call fail, so the window is always created as a
     * top-level window and only positioned near the client afterwards. */
    (void)owner;
    g_hwnd = CreateWindowExW(WS_EX_TOOLWINDOW,
                             kSettingsClass, kSettingsTitle,
                             WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_THICKFRAME,
                             CW_USEDEFAULT, CW_USEDEFAULT, 480, 360,
                             NULL, NULL, inst, NULL);
    if (!g_hwnd) return;

    g_threadId = GetCurrentThreadId();

    /* Centre on the owner when we have one, otherwise on the work area. */
    RECT target;
    ZeroMemory(&target, sizeof(target));
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &target, 0);
    const bool haveTarget = (target.right > target.left);
    if (haveTarget) {
        RECT self;
        ZeroMemory(&self, sizeof(self));
        if (GetWindowRect(g_hwnd, &self)) {
            const int w = self.right - self.left;
            const int h = self.bottom - self.top;
            const int x = target.left + ((target.right - target.left) - w) / 2;
            const int y = target.top + ((target.bottom - target.top) - h) / 2;
            SetWindowPos(g_hwnd, HWND_TOP, x, y, 0, 0, SWP_NOSIZE | SWP_SHOWWINDOW);
        }
    }

    ShowWindow(g_hwnd, SW_SHOWNORMAL);
    SetForegroundWindow(g_hwnd);
    SetTimer(g_hwnd, kTimerSync, 500, NULL);

    /* Modal loop: ts3plugin_configure must block while the window is open. */
    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        if (IsDialogMessageW(g_hwnd, &msg)) continue;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    if (g_font) {
        DeleteObject(g_font);
        g_font = NULL;
    }
    g_threadId = 0;
}

void SettingsShowAsync()
{
    /* Raise an already open window instead of starting a second one. */
    if (g_hwnd && IsWindow(g_hwnd)) {
        SyncFromOverlay();
        ShowWindow(g_hwnd, SW_SHOWNORMAL);
        SetForegroundWindow(g_hwnd);
        return;
    }
    /* The caller may be the client's Qt GUI thread (plugin menu handler), so
     * the modal loop cannot run there without freezing TeamSpeak. Give the
     * window its own thread and let SettingsShow pump it. */
    HANDLE th = CreateThread(NULL, 0, AsyncThread, NULL, 0, NULL);
    if (th) CloseHandle(th);
}

void SettingsClose()
{
    if (g_hwnd && IsWindow(g_hwnd)) {
        if (g_threadId && g_threadId != GetCurrentThreadId()) {
            PostMessageW(g_hwnd, WM_CLOSE, 0, 0);
        } else {
            DestroyWindow(g_hwnd);
        }
    }
}

} // namespace ts3ovl

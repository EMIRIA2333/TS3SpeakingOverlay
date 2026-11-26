/*
 * ts3-speaking-overlay - always-on-top "who is talking" overlay (KOOK style)
 * settings.cpp : native settings window
 *
 * Opened from the TS3 client through
 *   Options -> Addons -> "Speaking Overlay" -> Settings
 * or from the plugin menu ("设置 / 游戏列表…", "检查更新…").
 *
 * Threading: ts3plugin_offersConfigure() returns
 * PLUGIN_OFFERS_CONFIGURE_NEW_THREAD, i.e. the client runs ts3plugin_configure
 * on a thread it creates for us and expects the call to block for as long as
 * the window is open. A window without a message pump would just die, so
 * SettingsShow() owns a modal message loop and returns when the window closes.
 *
 * Everything the UI shows is read back from the live modules (overlay.cpp,
 * games.cpp, update.cpp) so the window can never display a stale value.
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
#include <commdlg.h>
#include <shellapi.h>

#include <cstdarg>
#include <cstdio>
#include <string>
#include <vector>

#include "games.h"
#include "overlay.h"
#include "settings.h"
#include "update.h"
#include "version.h"

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
    IDC_HINT_TEXT        = 2006,

    IDC_GRP_GAMES        = 2010,
    IDC_CHK_AUTODETECT   = 2011,
    IDC_LIST_GAMES       = 2012,
    IDC_BTN_ADD_CURRENT  = 2013,
    IDC_BTN_ADD_FILE     = 2014,
    IDC_BTN_SCAN         = 2015,
    IDC_BTN_ENABLE       = 2016,
    IDC_BTN_DISABLE      = 2017,
    IDC_BTN_REMOVE       = 2018,

    IDC_GRP_UPDATE       = 2020,
    IDC_UPDATE_CURRENT   = 2021,
    IDC_BTN_CHECK        = 2022,
    IDC_UPDATE_STATE     = 2023,
    IDC_BTN_DOWNLOAD     = 2024,
    IDC_BTN_PAGE         = 2025,

    IDC_GAME_COUNT       = 2030,

    IDC_GRP_LOOK         = 2040,
    IDC_BTN_TEXT_COLOR   = 2041,
    IDC_SWATCH_TEXT      = 2042,
    IDC_BTN_BG_COLOR     = 2043,
    IDC_SWATCH_BG        = 2044,
    IDC_BTN_LOOK_RESET   = 2045,
    IDC_LBL_ALPHA        = 2046,
    IDC_SLD_ALPHA        = 2047,
    IDC_ALPHA_VALUE      = 2048,
    IDC_LBL_FONT         = 2049,
    IDC_CMB_FONT         = 2050
};

/* timer ids */
const UINT_PTR kTimerSync = 1;

/* private window messages used by the worker threads */
const UINT WM_UPDATE_CHECKED    = WM_APP + 1;
const UINT WM_UPDATE_DOWNLOADED = WM_APP + 2;

HWND g_hwnd          = NULL;
HWND g_chkVisible    = NULL;
HWND g_chkOnlyInGame = NULL;
HWND g_chkLocked     = NULL;
HWND g_btnReset      = NULL;
HWND g_statusText    = NULL;
HWND g_hintText      = NULL;
HWND g_chkAutoDetect = NULL;
HWND g_listGames     = NULL;
HWND g_btnAddCurrent = NULL;
HWND g_btnAddFile    = NULL;
HWND g_btnScan       = NULL;
HWND g_btnEnable     = NULL;
HWND g_btnDisable    = NULL;
HWND g_btnRemove     = NULL;
HWND g_updateCurrent = NULL;
HWND g_btnCheck      = NULL;
HWND g_updateState   = NULL;
HWND g_btnDownload   = NULL;
HWND g_btnPage       = NULL;
HWND g_gameCount     = NULL;
HWND g_grpGames      = NULL;   /* "游戏列表" group box */
HWND g_grpUpdate     = NULL;   /* "更新" group box */

HWND g_grpLook       = NULL;   /* "外观" group box */
HWND g_btnTextColor  = NULL;
HWND g_swatchText    = NULL;
HWND g_btnBgColor    = NULL;
HWND g_swatchBg      = NULL;
HWND g_btnLookReset  = NULL;
HWND g_lblAlpha      = NULL;
HWND g_sldAlpha      = NULL;
HWND g_alphaValue    = NULL;
HWND g_lblFont       = NULL;
HWND g_cmbFont       = NULL;

/* appearance, mirrored from the overlay for the swatch previews */
unsigned int g_bgColor   = 0x00000000;
unsigned int g_textColor = 0xFFFFFFFF;

HFONT g_font         = NULL;
DWORD g_threadId     = 0;      /* thread that owns g_hwnd */

/* Trace helper: the settings window runs on its own thread, so a hang inside
 * the update worker is invisible in the client log. Set TS3OVL_DEBUG=1 and read
 * %TEMP%\ts3ovl_dbg.log to see how far it got. */
void DbgSettings(const wchar_t* fmt, ...)
{
    wchar_t buf[1024];
    va_list ap;
    va_start(ap, fmt);
    _vsnwprintf(buf, 1024, fmt, ap);
    buf[1023] = L'\0';
    va_end(ap);

    OutputDebugStringW(L"[ts3ovl-settings] ");
    OutputDebugStringW(buf);
    OutputDebugStringW(L"\n");

    wchar_t flag[8] = {0};
    if (!GetEnvironmentVariableW(L"TS3OVL_DEBUG", flag, 7) || flag[0] != L'1') return;

    wchar_t path[MAX_PATH];
    if (!GetTempPathW(MAX_PATH, path)) return;
    std::wstring file = path;
    file += L"ts3ovl_dbg.log";

    FILE* f = NULL;
    if (_wfopen_s(&f, file.c_str(), L"a, ccs=UTF-8") != 0 || !f) return;
    fwprintf(f, L"[settings] %ls\n", buf);   /* %ls: MinGW treats %s as narrow here */
    fclose(f);
}

/* pending update information (kept between "check" and "download") */
UpdateResult g_update;
std::wstring g_downloadedSetup;
bool         g_busy = false;

/* set by SettingsShowUpdateAsync so the freshly opened window starts a check
 * on its own (the "检查更新…" menu item must not require a second click) */
bool g_pendingAutoCheck = false;

/* Re-entrancy guard for the list view.
 *
 * Rebuilding the list from inside its own LVN_ITEMCHANGED notification (which
 * is what a naive WM_NOTIFY handler does) tears the list down while comctl32 is
 * still iterating its items: ListView_DeleteAllItems runs, then the pending
 * notification is dispatched against freed item state and comctl32 dies in
 * _TrackMouseEvent / CallWindowProcW with a corrupted stack.  Two rules keep
 * us out of that: RefreshGameList() refuses to run while it is already running,
 * and notification handlers never rebuild synchronously - they post
 * WM_APP_REFRESH_LIST instead. */
bool g_refreshing = false;
#define WM_APP_REFRESH_LIST (WM_APP + 10)

/* Every rebuild goes through the message queue. Deleting the items from inside a
 * button handler or a notification still leaves comctl32 walking its own item
 * array (gdb showed the crash inside comctl32's _TrackMouseEvent right after a
 * Remove click), so the rebuild is always deferred to a clean dispatch. */
void RequestListRefresh()
{
    if (g_hwnd && IsWindow(g_hwnd)) PostMessageW(g_hwnd, WM_APP_REFRESH_LIST, 0, 0);
}

/* ------------------------------------------------------------------ */
/* small helpers                                                       */
/* ------------------------------------------------------------------ */

HWND MakeChild(HWND parent, const wchar_t* cls, const wchar_t* text, DWORD style,
               int id, HINSTANCE inst)
{
    return CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | style,
                           0, 0, 0, 0, parent, (HMENU)(INT_PTR)id, inst, NULL);
}

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
    if (g_chkAutoDetect) {
        SendMessageW(g_chkAutoDetect, BM_SETCHECK,
                     GamesAutoDetect() ? BST_CHECKED : BST_UNCHECKED, 0);
    }

    /* One line of live status underneath the switches. */
    if (g_statusText) {
        wchar_t msg[512];
        const wchar_t* visible = OverlayIsShowing() ? L"是" : L"否";
        std::wstring running = GamesRunningName();
        std::wstring gameMsg;
        if (running.empty()) {
            gameMsg = L"　未检测到列表中的游戏在运行";
        } else {
            gameMsg = L"　正在运行：";
            gameMsg += running;
        }
        const wchar_t* hidden = OverlayHiddenByForeground() ? L"（被『仅游戏内显示』隐藏）" : L"";
        wsprintfW(msg, L"当前是否可见：%s%s%s", visible, hidden, gameMsg.c_str());
        SetWindowTextW(g_statusText, msg);
    }
}

/* ------------------------------------------------------------------ */
/* appearance                                                          */
/* ------------------------------------------------------------------ */

/* Our colours are 0xAARRGGBB, CHOOSECOLORW speaks COLORREF (0x00BBGGRR). */
COLORREF ArgbToColorref(unsigned int argb)
{
    return RGB((argb >> 16) & 0xFF, (argb >> 8) & 0xFF, argb & 0xFF);
}

unsigned int ColorrefToArgb(COLORREF c, unsigned int alpha)
{
    return ((alpha & 0xFF) << 24) | (GetRValue(c) << 16) | (GetGValue(c) << 8) | GetBValue(c);
}

void SyncLookFromOverlay()
{
    Appearance a;
    OverlayGetAppearance(&a);
    g_bgColor   = a.bgColor;
    g_textColor = a.textColor;

    if (g_sldAlpha) {
        SendMessageW(g_sldAlpha, TBM_SETPOS, TRUE, (LPARAM)((a.bgColor >> 24) & 0xFF));
    }
    if (g_alphaValue) {
        wchar_t buf[32];
        wsprintfW(buf, L"%d%%", (int)(((a.bgColor >> 24) & 0xFF) * 100 / 255));
        SetWindowTextW(g_alphaValue, buf);
    }
    if (g_cmbFont) {
        wchar_t buf[16];
        wsprintfW(buf, L"%d", a.fontSize);
        SetWindowTextW(g_cmbFont, buf);
    }
    if (g_swatchText) InvalidateRect(g_swatchText, NULL, TRUE);
    if (g_swatchBg)   InvalidateRect(g_swatchBg, NULL, TRUE);
}

/* Push the controls' current values into the overlay and persist them. */
void ApplyAppearance()
{
    Appearance a;
    OverlayGetAppearance(&a);

    if (g_sldAlpha) {
        int pos = (int)SendMessageW(g_sldAlpha, TBM_GETPOS, 0, 0);
        if (pos < 0)   pos = 0;
        if (pos > 255) pos = 255;
        a.bgColor = ((unsigned int)pos << 24) | (g_bgColor & 0x00FFFFFFu);
    } else {
        a.bgColor = g_bgColor;
    }
    a.textColor = g_textColor;

    if (g_cmbFont) {
        wchar_t buf[16] = {0};
        GetWindowTextW(g_cmbFont, buf, 16);
        const int px = _wtoi(buf);
        if (px >= 8 && px <= 32) a.fontSize = px;
    }

    OverlaySetAppearance(a);
    OverlaySaveSettings();

    /* remember what actually took effect (the overlay clamps the values) */
    Appearance now;
    OverlayGetAppearance(&now);
    g_bgColor   = now.bgColor;
    g_textColor = now.textColor;
    if (g_swatchBg)   InvalidateRect(g_swatchBg, NULL, TRUE);
    if (g_swatchText) InvalidateRect(g_swatchText, NULL, TRUE);
    if (g_alphaValue) {
        wchar_t buf[32];
        wsprintfW(buf, L"%d%%", (int)(((now.bgColor >> 24) & 0xFF) * 100 / 255));
        SetWindowTextW(g_alphaValue, buf);
    }
}

void PickTextColor(HWND owner)
{
    static COLORREF custom[16];
    CHOOSECOLORW cc;
    ZeroMemory(&cc, sizeof(cc));
    cc.lStructSize  = sizeof(cc);
    cc.hwndOwner    = owner;
    cc.rgbResult    = ArgbToColorref(g_textColor);
    cc.lpCustColors = custom;
    cc.Flags        = CC_FULLOPEN | CC_RGBINIT;
    if (!ChooseColorW(&cc)) return;
    g_textColor = ColorrefToArgb(cc.rgbResult, 0xFF);
    ApplyAppearance();
}

void PickBgColor(HWND owner)
{
    static COLORREF customB[16];
    CHOOSECOLORW cc;
    ZeroMemory(&cc, sizeof(cc));
    cc.lStructSize  = sizeof(cc);
    cc.hwndOwner    = owner;
    cc.rgbResult    = ArgbToColorref(g_bgColor);
    cc.lpCustColors = customB;
    cc.Flags        = CC_FULLOPEN | CC_RGBINIT;
    if (!ChooseColorW(&cc)) return;

    /* Picking a background colour while the panel is fully transparent would
     * look like nothing happened, so give it a sane default opacity. */
    unsigned int alpha = (g_bgColor >> 24) & 0xFF;
    if (alpha == 0) alpha = 0xB4;
    g_bgColor = ColorrefToArgb(cc.rgbResult, alpha);
    if (g_sldAlpha) SendMessageW(g_sldAlpha, TBM_SETPOS, TRUE, (LPARAM)alpha);
    ApplyAppearance();
}

void ResetAppearance()
{
    Appearance a;
    a.bgColor   = 0x00000000u;   /* fully transparent again */
    a.textColor = 0xFFFFFFFFu;   /* white */
    a.fontSize  = 12;
    OverlaySetAppearance(a);
    OverlaySaveSettings();
    SyncLookFromOverlay();
}

void DrawSwatch(const DRAWITEMSTRUCT* dis)
{
    if (!dis) return;
    const unsigned int color = (dis->CtlID == IDC_SWATCH_TEXT) ? g_textColor : g_bgColor;
    HBRUSH br = CreateSolidBrush(ArgbToColorref(color));
    if (br) {
        FillRect(dis->hDC, &dis->rcItem, br);
        DeleteObject(br);
    }
    /* a thin frame so a white swatch is still visible on a white dialog */
    FrameRect(dis->hDC, &dis->rcItem, (HBRUSH)GetStockObject(GRAY_BRUSH));
    /* show the alpha of the background as text, otherwise it is a mystery */
    if (dis->CtlID == IDC_SWATCH_BG) {
        wchar_t buf[32];
        wsprintfW(buf, L"%d%%", (int)(((color >> 24) & 0xFF) * 100 / 255));
        SetBkMode(dis->hDC, TRANSPARENT);
        SetTextColor(dis->hDC, RGB(0, 0, 0));
        RECT r = dis->rcItem;
        r.left += 4;
        DrawTextW(dis->hDC, buf, -1, &r, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    }
}

/* Window under the settings dialog itself: the user has to click our button to
 * reach this code, so GetForegroundWindow() is always *our* window - that is
 * how "test_settings.exe" ended up in the game list during testing. Walk the
 * z-order instead and take the first visible top-level window that belongs to
 * another process and is not a shell/desktop window. */
struct FrontmostSearch
{
    DWORD        self;
    std::wstring exe;
};

BOOL CALLBACK FrontmostProc(HWND hwnd, LPARAM param)
{
    FrontmostSearch* s = (FrontmostSearch*)param;
    if (!IsWindowVisible(hwnd)) return TRUE;

    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid == 0 || pid == s->self) return TRUE;

    const LONG_PTR ex = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
    if (ex & WS_EX_TOOLWINDOW) return TRUE;
    if (GetWindowTextLengthW(hwnd) == 0) return TRUE;
    /* GetClassNameW(hwnd, NULL, 0) returns 0, so it cannot be used to probe the
     * class length - ask for the real name instead. */
    wchar_t cls[128] = {0};
    if (GetClassNameW(hwnd, cls, 128) <= 0) return TRUE;

    /* Resolve the image name here and keep walking when it is not readable
     * (elevated processes cannot be queried): stopping at the first window
     * whose owner is unknown used to make the button report "读不到". */
    const std::wstring exe = GamesExeOfWindow((unsigned long long)hwnd);
    if (exe.empty()) return TRUE;

    s->exe = exe;
    return FALSE; /* first resolvable hit wins - EnumWindows walks front to back */
}

std::wstring FrontmostForeignExe()
{
    FrontmostSearch s;
    s.self  = GetCurrentProcessId();
    EnumWindows(FrontmostProc, (LPARAM)&s);
    return s.exe;
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

/* ------------------------------------------------------------------ */
/* game list                                                           */
/* ------------------------------------------------------------------ */

/* Rebuild the list view from games.cpp. The check state means "this game shows
 * the overlay". */
void RefreshGameList()
{
    if (!g_listGames) return;
    /* A notification is being dispatched right now: rebuilding would free the
     * items comctl32 is still walking. Let the caller's PostMessage do it. */
    if (g_refreshing) return;
    g_refreshing = true;

    /* remember the selection so a refresh does not jump around */
    const int sel = (int)SendMessageW(g_listGames, LVM_GETNEXTITEM, (WPARAM)-1, LVNI_SELECTED);

    SendMessageW(g_listGames, WM_SETREDRAW, FALSE, 0);
    ListView_DeleteAllItems(g_listGames);

    const std::vector<GameEntry> games = GamesList();
    for (size_t i = 0; i < games.size(); ++i) {
        LVITEMW it;
        ZeroMemory(&it, sizeof(it));
        it.mask     = LVIF_TEXT;
        it.iItem    = (int)i;
        it.iSubItem = 0;
        it.pszText  = (LPWSTR)games[i].exe.c_str();
        const int row = (int)SendMessageW(g_listGames, LVM_INSERTITEMW, 0, (LPARAM)&it);
        if (row < 0) continue;

        std::wstring state = games[i].enabled ? L"开启" : L"关闭";
        if (games[i].autoAdded) state += L"（自动检测）";
        ListView_SetItemText(g_listGames, row, 1, (LPWSTR)state.c_str());
        /* Setting the check state through the control (rather than passing
         * LVIF_STATE to LVM_INSERTITEMW) is what actually paints the box.
         * This raises LVN_ITEMCHANGED, which is harmless here because
         * g_refreshing is set: that handler ignores our own rebuild instead of
         * posting yet another refresh (the old message flood). */
        ListView_SetCheckState(g_listGames, row, games[i].enabled ? TRUE : FALSE);
    }

    if (sel >= 0 && sel < (int)games.size()) {
        ListView_SetItemState(g_listGames, sel, LVIS_SELECTED | LVIS_FOCUSED,
                              LVIS_SELECTED | LVIS_FOCUSED);
    }

    SendMessageW(g_listGames, WM_SETREDRAW, TRUE, 0);

    if (g_gameCount) {
        wchar_t buf[128];
        wsprintfW(buf, L"已登记 %u 个游戏", (unsigned)games.size());
        SetWindowTextW(g_gameCount, buf);
    }

    g_refreshing = false;
}

/* Currently selected exe name, "" when nothing is selected. */
std::wstring SelectedGame()
{
    if (!g_listGames) return std::wstring();
    const int row = (int)SendMessageW(g_listGames, LVM_GETNEXTITEM, (WPARAM)-1, LVNI_SELECTED);
    if (row < 0) return std::wstring();

    wchar_t buf[260] = {0};
    LVITEMW it;
    ZeroMemory(&it, sizeof(it));
    it.mask       = LVIF_TEXT;
    it.iItem      = row;
    it.iSubItem   = 0;
    it.pszText    = buf;
    it.cchTextMax = 260;
    SendMessageW(g_listGames, LVM_GETITEMW, 0, (LPARAM)&it);
    return std::wstring(buf);
}

void BrowseForGameExe(HWND owner)
{
    wchar_t path[MAX_PATH] = {0};
    OPENFILENAMEW ofn;
    ZeroMemory(&ofn, sizeof(ofn));
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner   = owner;
    ofn.lpstrFilter = L"程序 (*.exe)\0*.exe\0所有文件 (*.*)\0*.*\0";
    ofn.lpstrFile   = path;
    ofn.nMaxFile    = MAX_PATH;
    ofn.lpstrTitle  = L"选择游戏的 exe（例如 cs2.exe）";
    ofn.Flags       = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;

    if (GetOpenFileNameW(&ofn)) {
        /* Adding by hand overrides an earlier "delete": the user explicitly
         * wants this exe back on the list. */
        GamesSetIgnored(path, false);
        if (GamesAdd(path)) RequestListRefresh();
        else MessageBoxW(owner, L"这个游戏已经在列表里了。", L"Speaking Overlay", MB_OK | MB_ICONINFORMATION);
    }
}

/* ------------------------------------------------------------------ */
/* update worker threads                                               */
/* ------------------------------------------------------------------ */

struct CheckCtx { HWND hwnd; };

DWORD WINAPI CheckThread(LPVOID param)
{
    CheckCtx* ctx = (CheckCtx*)param;
    DbgSettings(L"CheckThread: entered, hwnd=%p", (void*)ctx->hwnd);

    UpdateResult r = UpdateCheck();
    DbgSettings(L"CheckThread: UpdateCheck done, ok=%d hasUpdate=%d err=[%s]",
                r.ok ? 1 : 0, r.hasUpdate ? 1 : 0, r.error.c_str());

    UpdateResult* heap = new UpdateResult();
    *heap = r;
    const BOOL posted = PostMessageW(ctx->hwnd, WM_UPDATE_CHECKED, 0, (LPARAM)heap);
    DbgSettings(L"CheckThread: PostMessage posted=%d err=%lu", posted ? 1 : 0,
                (unsigned long)(posted ? 0 : GetLastError()));
    delete ctx;
    return 0;
}

struct DownloadCtx { HWND hwnd; std::wstring url; std::wstring path; };

DWORD WINAPI DownloadThread(LPVOID param)
{
    DownloadCtx* ctx = (DownloadCtx*)param;
    std::wstring err;
    const bool ok = UpdateDownload(ctx->url, ctx->path, &err);

    /* hand the result over to the UI thread */
    std::wstring* payload = new std::wstring(ok ? ctx->path : err);
    PostMessageW(ctx->hwnd, WM_UPDATE_DOWNLOADED, ok ? 1 : 0, (LPARAM)payload);
    return 0;
}

std::wstring TempSetupPath()
{
    wchar_t dir[MAX_PATH] = {0};
    if (GetTempPathW(MAX_PATH, dir) == 0) return L"TS3SpeakingOverlay-Setup.exe";
    std::wstring p = dir;
    if (!p.empty() && p[p.size() - 1] != L'\\') p += L'\\';
    p += L"TS3SpeakingOverlay-Setup.exe";
    return p;
}

/* ------------------------------------------------------------------ */
/* layout                                                              */
/* ------------------------------------------------------------------ */

void LayoutChildren(HWND hwnd)
{
    RECT rc;
    GetClientRect(hwnd, &rc);
    const int pad = 16;
    const int w   = rc.right - rc.left;
    const int h   = rc.bottom - rc.top;
    int y = pad;

    const int chkW = w - pad * 2;
    const int chkH = 26;
    const int btnH = 30;

    if (g_chkVisible)    { MoveWindow(g_chkVisible,    pad, y, chkW, chkH, TRUE); y += chkH; }
    if (g_chkOnlyInGame) { MoveWindow(g_chkOnlyInGame, pad, y, chkW, chkH, TRUE); y += chkH; }
    if (g_chkLocked)     { MoveWindow(g_chkLocked,     pad, y, chkW, chkH, TRUE); y += chkH + 4; }
    if (g_btnReset)      { MoveWindow(g_btnReset,      pad, y, 150, btnH, TRUE);  y += btnH + 12; }

    /* ---- appearance group (fixed height, right under the switches) ---- */
    const int lookH = 96;
    const int lookY = y;
    if (g_grpLook) MoveWindow(g_grpLook, pad, lookY, chkW, lookH, TRUE);
    {
        const int ix = pad + 10;
        const int iw = chkW - 20;
        int ly = lookY + 24;
        /* row 1: text colour + swatch | background colour + swatch | reset */
        if (g_btnTextColor) MoveWindow(g_btnTextColor, ix, ly, 110, btnH, TRUE);
        if (g_swatchText)   MoveWindow(g_swatchText, ix + 116, ly + 4, 46, btnH - 8, TRUE);
        if (g_btnBgColor)   MoveWindow(g_btnBgColor, ix + 176, ly, 110, btnH, TRUE);
        if (g_swatchBg)     MoveWindow(g_swatchBg, ix + 292, ly + 4, 46, btnH - 8, TRUE);
        if (g_btnLookReset) {
            int rx = ix + iw - 130;
            if (rx < ix + 350) rx = ix + 350;
            MoveWindow(g_btnLookReset, rx, ly, 130, btnH, TRUE);
        }
        /* row 2: opacity label + slider + value | font size label + combo */
        ly += btnH + 8;
        if (g_lblAlpha) MoveWindow(g_lblAlpha, ix, ly + 6, 130, 20, TRUE);
        if (g_sldAlpha) MoveWindow(g_sldAlpha, ix + 134, ly + 2, 220, 28, TRUE);
        if (g_alphaValue) MoveWindow(g_alphaValue, ix + 360, ly + 6, 48, 20, TRUE);
        if (g_lblFont)  MoveWindow(g_lblFont, ix + 420, ly + 6, 46, 20, TRUE);
        if (g_cmbFont)  MoveWindow(g_cmbFont, ix + 470, ly, 80, 200, TRUE);
    }
    y = lookY + lookH + 12;

    /* ---- bottom status (fixed to the bottom, laid out first so the groups
     *      above can be sized against it) ---- */
    const int statusH = 22;
    const int hintH   = 34;
    const int bottomY = h - pad - hintH - statusH - 6;

    /* ---- update group ---- */
    const int updH = 96;
    int updY = bottomY - 12 - updH;
    if (updY < y + 140) updY = y + 140;          /* never squeeze the list away */
    if (g_grpUpdate) MoveWindow(g_grpUpdate, pad, updY, chkW, updH, TRUE);
    {
        const int ix = pad + 10;
        const int iw = chkW - 20;
        int iy = updY + 22;
        if (g_updateCurrent) {
            MoveWindow(g_updateCurrent, ix, iy + 4, 200, 22, TRUE);
            if (g_btnCheck) MoveWindow(g_btnCheck, ix + 210, iy, 110, btnH, TRUE);
            if (g_btnPage)  MoveWindow(g_btnPage,  ix + 328, iy, 110, btnH, TRUE);
            iy += btnH + 6;
        }
        if (g_updateState) {
            MoveWindow(g_updateState, ix, iy, iw - 158, 40, TRUE);
        }
        if (g_btnDownload) {
            MoveWindow(g_btnDownload, ix + iw - 150, iy, 150, btnH, TRUE);
            iy += btnH + 4;
        }
        (void)iy;
    }

    /* ---- game group fills whatever is left between the switches and the
     *      update group ---- */
    const int grpY = y;
    const int grpH = updY - 10 - grpY;
    if (g_grpGames) MoveWindow(g_grpGames, pad, grpY, chkW, grpH, TRUE);
    {
        const int ix = pad + 10;
        const int iw = chkW - 20;
        int iy = grpY + 20;

        if (g_chkAutoDetect) {
            MoveWindow(g_chkAutoDetect, ix, iy, iw, chkH, TRUE);
            iy += chkH + 2;
        }
        if (g_gameCount) {
            MoveWindow(g_gameCount, ix, iy, iw, 18, TRUE);
            iy += 20;
        }

        /* Button row *below* the list: a vertical column next to a 150 px list
         * cannot hold five buttons, which is what used to overlap the update
         * area. */
        const int rowH   = btnH + 8;
        const int listH  = (grpY + grpH - 8 - rowH) - iy;
        if (g_listGames) {
            MoveWindow(g_listGames, ix, iy, iw, listH > 40 ? listH : 40, TRUE);
        }
        iy += (listH > 40 ? listH : 40) + 8;

        /* one flow row: add current / browse / scan | enable / disable | remove */
        int bx = ix;
        const int gap = 6;
        const int wAddCurrent = 130;
        const int wAddFile    = 118;
        const int wScan       = 150;
        const int wSmall      = 60;
        const int wRemove     = 100;

        if (g_btnAddCurrent) {
            MoveWindow(g_btnAddCurrent, bx, iy, wAddCurrent, btnH, TRUE);
            bx += wAddCurrent + gap;
        }
        if (g_btnAddFile) {
            MoveWindow(g_btnAddFile, bx, iy, wAddFile, btnH, TRUE);
            bx += wAddFile + gap;
        }
        if (g_btnScan) {
            MoveWindow(g_btnScan, bx, iy, wScan, btnH, TRUE);
            bx += wScan + gap;
        }
        if (g_btnEnable) {
            MoveWindow(g_btnEnable, bx, iy, wSmall, btnH, TRUE);
            bx += wSmall + gap;
        }
        if (g_btnDisable) {
            MoveWindow(g_btnDisable, bx, iy, wSmall, btnH, TRUE);
            bx += wSmall + gap;
        }
        if (g_btnRemove) {
            /* right aligned so it does not wander when the window gets wide */
            int rx = ix + iw - wRemove;
            if (rx < bx) rx = bx;
            MoveWindow(g_btnRemove, rx, iy, wRemove, btnH, TRUE);
        }
    }

    if (g_hintText) {
        MoveWindow(g_hintText, pad, h - pad - hintH, chkW, hintH, TRUE);
    }
    if (g_statusText) {
        MoveWindow(g_statusText, pad, h - pad - hintH - statusH - 4, chkW, statusH, TRUE);
    }
}

/* ------------------------------------------------------------------ */
/* window procedure                                                    */
/* ------------------------------------------------------------------ */

LRESULT CALLBACK SettingsProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    if (msg >= WM_APP) {
        DbgSettings(L"proc msg=0x%04X wParam=%p", (unsigned)msg, (void*)wParam);
    }
    switch (msg) {
    case WM_CREATE: {
        HINSTANCE inst = (HINSTANCE)GetWindowLongPtrW(hwnd, GWLP_HINSTANCE);

        g_chkVisible    = MakeChild(hwnd, L"BUTTON", L"显示悬浮窗",
                                    BS_AUTOCHECKBOX, IDC_CHK_VISIBLE, inst);
        g_chkOnlyInGame = MakeChild(hwnd, L"BUTTON", L"只在列表中的游戏运行时显示（推荐）",
                                    BS_AUTOCHECKBOX, IDC_CHK_ONLY_IN_GAME, inst);
        g_chkLocked     = MakeChild(hwnd, L"BUTTON", L"锁定位置（锁定后鼠标可穿透）",
                                    BS_AUTOCHECKBOX, IDC_CHK_LOCKED, inst);
        g_btnReset      = MakeChild(hwnd, L"BUTTON", L"重置浮窗位置",
                                    BS_PUSHBUTTON, IDC_BTN_RESET, inst);

        g_chkAutoDetect = MakeChild(hwnd, L"BUTTON", L"自动检测新下载的游戏（新游戏默认开启浮窗）",
                                    BS_AUTOCHECKBOX, IDC_CHK_AUTODETECT, inst);

        g_grpGames  = MakeChild(hwnd, L"BUTTON", L" 游戏列表 ",
                                BS_GROUPBOX, IDC_GRP_GAMES, inst);
        g_grpUpdate = MakeChild(hwnd, L"BUTTON", L" 检查更新 ",
                                BS_GROUPBOX, IDC_GRP_UPDATE, inst);

        g_listGames = CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"",
                                      WS_CHILD | WS_VISIBLE | LVS_REPORT | LVS_SINGLESEL |
                                      LVS_SHOWSELALWAYS,
                                      0, 0, 0, 0, hwnd, (HMENU)(INT_PTR)IDC_LIST_GAMES, inst, NULL);
        if (g_listGames) {
            ListView_SetExtendedListViewStyle(g_listGames,
                                              LVS_EX_FULLROWSELECT | LVS_EX_CHECKBOXES |
                                              LVS_EX_GRIDLINES | LVS_EX_DOUBLEBUFFER);
            LVCOLUMNW col;
            ZeroMemory(&col, sizeof(col));
            col.mask     = LVCF_TEXT | LVCF_WIDTH;
            col.pszText  = (LPWSTR)L"游戏程序";
            col.cx       = 260;
            SendMessageW(g_listGames, LVM_INSERTCOLUMNW, 0, (LPARAM)&col);
            col.pszText = (LPWSTR)L"状态";
            col.cx      = 150;
            SendMessageW(g_listGames, LVM_INSERTCOLUMNW, 1, (LPARAM)&col);
        }

        g_gameCount     = MakeChild(hwnd, L"STATIC", L"", SS_LEFT, IDC_GAME_COUNT, inst);
        g_btnAddCurrent = MakeChild(hwnd, L"BUTTON", L"添加当前前台程序",
                                    BS_PUSHBUTTON, IDC_BTN_ADD_CURRENT, inst);
        g_btnAddFile    = MakeChild(hwnd, L"BUTTON", L"浏览选择 exe…",
                                    BS_PUSHBUTTON, IDC_BTN_ADD_FILE, inst);
        g_btnScan       = MakeChild(hwnd, L"BUTTON", L"扫描已安装的游戏",
                                    BS_PUSHBUTTON, IDC_BTN_SCAN, inst);
        g_btnEnable     = MakeChild(hwnd, L"BUTTON", L"启用", BS_PUSHBUTTON, IDC_BTN_ENABLE, inst);
        g_btnDisable    = MakeChild(hwnd, L"BUTTON", L"关闭", BS_PUSHBUTTON, IDC_BTN_DISABLE, inst);
        g_btnRemove     = MakeChild(hwnd, L"BUTTON", L"从列表删除", BS_PUSHBUTTON, IDC_BTN_REMOVE, inst);

        wchar_t cur[128];
        wsprintfW(cur, L"当前版本：%s", TS3OVL_VERSION_W);
        g_updateCurrent = MakeChild(hwnd, L"STATIC", cur, SS_LEFT, IDC_UPDATE_CURRENT, inst);
        g_btnCheck      = MakeChild(hwnd, L"BUTTON", L"检查更新",
                                    BS_PUSHBUTTON, IDC_BTN_CHECK, inst);
        g_btnPage       = MakeChild(hwnd, L"BUTTON", L"打开发布页",
                                    BS_PUSHBUTTON, IDC_BTN_PAGE, inst);
        g_updateState   = MakeChild(hwnd, L"STATIC", L"点『检查更新』查询 GitHub 上的最新版本。",
                                    SS_LEFT, IDC_UPDATE_STATE, inst);
        g_btnDownload   = MakeChild(hwnd, L"BUTTON", L"下载并安装",
                                    BS_PUSHBUTTON, IDC_BTN_DOWNLOAD, inst);
        EnableWindow(g_btnDownload, FALSE);

        g_statusText = MakeChild(hwnd, L"STATIC", L"", SS_LEFT, IDC_STATUS_TEXT, inst);
        g_hintText   = MakeChild(hwnd, L"STATIC",
                                 L"提示：拖动浮窗可移动位置，松手后会自动吸附到屏幕边缘；"
                                 L"勾选列表里的游戏决定什么时候显示浮窗。",
                                 SS_LEFT, IDC_HINT_TEXT, inst);

        /* ---- 外观 ---- */
        g_grpLook = MakeChild(hwnd, L"BUTTON", L" 外观 ", BS_GROUPBOX, IDC_GRP_LOOK, inst);
        g_btnTextColor = MakeChild(hwnd, L"BUTTON", L"文字颜色…",
                                   BS_PUSHBUTTON, IDC_BTN_TEXT_COLOR, inst);
        g_swatchText   = MakeChild(hwnd, L"STATIC", L"", SS_OWNERDRAW, IDC_SWATCH_TEXT, inst);
        g_btnBgColor   = MakeChild(hwnd, L"BUTTON", L"背景颜色…",
                                   BS_PUSHBUTTON, IDC_BTN_BG_COLOR, inst);
        g_swatchBg     = MakeChild(hwnd, L"STATIC", L"", SS_OWNERDRAW, IDC_SWATCH_BG, inst);
        g_btnLookReset = MakeChild(hwnd, L"BUTTON", L"恢复默认外观",
                                   BS_PUSHBUTTON, IDC_BTN_LOOK_RESET, inst);
        g_lblAlpha     = MakeChild(hwnd, L"STATIC", L"背景不透明度：", SS_LEFT, IDC_LBL_ALPHA, inst);
        g_sldAlpha     = CreateWindowExW(0, TRACKBAR_CLASSW, L"",
                                         WS_CHILD | WS_VISIBLE | TBS_HORZ | TBS_NOTICKS,
                                         0, 0, 0, 0, hwnd,
                                         (HMENU)(INT_PTR)IDC_SLD_ALPHA, inst, NULL);
        if (g_sldAlpha) {
            SendMessageW(g_sldAlpha, TBM_SETRANGE, TRUE, MAKELPARAM(0, 255));
            SendMessageW(g_sldAlpha, TBM_SETPAGESIZE, 0, 16);
        }
        g_alphaValue   = MakeChild(hwnd, L"STATIC", L"0%", SS_LEFT, IDC_ALPHA_VALUE, inst);
        g_lblFont      = MakeChild(hwnd, L"STATIC", L"字号：", SS_LEFT, IDC_LBL_FONT, inst);
        g_cmbFont      = MakeChild(hwnd, L"COMBOBOX", L"",
                                   CBS_DROPDOWN | WS_VSCROLL | CBS_HASSTRINGS,
                                   IDC_CMB_FONT, inst);
        if (g_cmbFont) {
            for (int px = 8; px <= 32; px += 2) {
                wchar_t buf[8];
                wsprintfW(buf, L"%d", px);
                SendMessageW(g_cmbFont, CB_ADDSTRING, 0, (LPARAM)buf);
            }
        }

        NONCLIENTMETRICSW ncm;
        ZeroMemory(&ncm, sizeof(ncm));
        ncm.cbSize = sizeof(ncm);
        if (SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0)) {
            g_font = CreateFontIndirectW(&ncm.lfMessageFont);
        }
        if (!g_font) g_font = (HFONT)GetStockObject(DEFAULT_GUI_FONT);

        HWND kids[] = { g_chkVisible, g_chkOnlyInGame, g_chkLocked, g_btnReset,
                        g_grpGames, g_grpUpdate, g_grpLook,
                        g_chkAutoDetect, g_listGames, g_gameCount, g_btnAddCurrent,
                        g_btnAddFile, g_btnScan, g_btnEnable, g_btnDisable, g_btnRemove,
                        g_updateCurrent, g_btnCheck, g_btnPage, g_updateState,
                        g_btnDownload, g_statusText, g_hintText,
                        g_btnTextColor, g_btnBgColor, g_btnLookReset,
                        g_lblAlpha, g_alphaValue, g_lblFont, g_cmbFont };
        for (HWND k : kids) {
            if (k && g_font) SendMessageW(k, WM_SETFONT, (WPARAM)g_font, TRUE);
        }

        LayoutChildren(hwnd);
        SyncLookFromOverlay();
        /* g_hwnd is only assigned after CreateWindowExW returns, so post to the
         * handle we were given instead of going through RequestListRefresh(). */
        PostMessageW(hwnd, WM_APP_REFRESH_LIST, 0, 0);

        /* Opened through the "检查更新…" menu item: start the check on our own
         * so the user does not have to press the button a second time. */
        if (g_pendingAutoCheck) {
            g_pendingAutoCheck = false;
            PostMessageW(hwnd, WM_COMMAND, MAKEWPARAM(IDC_BTN_CHECK, BN_CLICKED), 0);
        }
        return 0;
    }

    case WM_SIZE:
        LayoutChildren(hwnd);
        return 0;

    case WM_GETMINMAXINFO: {
        MINMAXINFO* mmi = (LPMINMAXINFO)lParam;
        /* the button row is ~648 px wide and the appearance row ~530 px, so a
         * narrower window would make 从列表删除 run into 关闭; the height has to
         * fit switches + appearance + list + update + status. */
        mmi->ptMinTrackSize.x = 760;
        mmi->ptMinTrackSize.y = 700;
        return 0;
    }

    case WM_NOTIFY: {
        LPNMHDR nh = (LPNMHDR)lParam;
        if (nh && nh->idFrom == IDC_LIST_GAMES) {
            if (nh->code == LVN_ITEMCHANGED) {
                LPNMLISTVIEW nv = (LPNMLISTVIEW)lParam;
                /* Checkbox changed -> persist it. Never rebuild the list from
                 * in here: comctl32 is still inside its own item loop, so the
                 * rebuild must be posted, not called.
                 *
                 * g_refreshing means the change came from our own rebuild, not
                 * from the user - persisting it would be a no-op and posting a
                 * refresh from here would loop forever. */
                if (g_refreshing) return 0;
                if ((nv->uChanged & LVIF_STATE) &&
                    ((nv->uNewState ^ nv->uOldState) & LVIS_STATEIMAGEMASK)) {
                    const BOOL checked = ListView_GetCheckState(g_listGames, nv->iItem);
                    wchar_t buf[260] = {0};
                    /* ListView_GetItemText() instead of a hand-built
                     * LVM_GETITEMW: inside LVN_ITEMCHANGED the control is still
                     * walking its own item array and the raw message can come
                     * back empty, which used to make the change silently not be
                     * persisted. */
                    ListView_GetItemText(g_listGames, nv->iItem, 0, buf, 260);
                    DbgSettings(L"LVN_ITEMCHANGED row=%d text=[%ls] checked=%d",
                                nv->iItem, buf, (int)checked);
                    if (buf[0]) {
                        GamesSetEnabled(buf, checked ? true : false);
                        PostMessageW(hwnd, WM_APP_REFRESH_LIST, 0, 0);
                    }
                }
            } else if (nh->code == NM_DBLCLK) {
                /* double click toggles the entry */
                const std::wstring exe = SelectedGame();
                if (!exe.empty()) {
                    GamesSetEnabled(exe, !GamesEntryEnabled(exe));
                    PostMessageW(hwnd, WM_APP_REFRESH_LIST, 0, 0);
                }
            }
        }
        return 0;
    }

    /* Deferred list rebuild: runs after comctl32 finished dispatching, which is
     * the only safe moment to delete and recreate items. */
    case WM_APP_REFRESH_LIST:
        RefreshGameList();
        SyncFromOverlay();
        return 0;

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
            SyncFromOverlay();
            return 0;
        }
        if (id == IDC_CHK_AUTODETECT && code == BN_CLICKED) {
            GamesSetAutoDetect(Checked(g_chkAutoDetect));
            SyncFromOverlay();
            return 0;
        }
        if (id == IDC_BTN_ADD_CURRENT && code == BN_CLICKED) {
            /* Not GetForegroundWindow(): the click itself makes this window the
             * foreground one, which is how the harness ended up adding itself. */
            const std::wstring exe = FrontmostForeignExe();
            if (exe.empty()) {
                MessageBoxW(hwnd, L"读不到前台程序（可能只剩系统窗口）。"
                                  L"\n请先切到游戏窗口再点这个按钮，或用『浏览选择 exe…』。",
                            L"Speaking Overlay", MB_OK | MB_ICONINFORMATION);
            } else if (GamesAdd(exe)) {
                RequestListRefresh();
            } else {
                /* Either it is already listed or the user had removed it - in
                 * the latter case a click on "添加当前前台程序" is an explicit
                 * "bring it back", so clear the ignore flag and retry. */
                if (GamesIsIgnored(exe)) {
                    GamesSetIgnored(exe, false);
                    if (GamesAdd(exe)) { RequestListRefresh(); return 0; }
                }
                MessageBoxW(hwnd, L"这个程序已经在列表里了。", L"Speaking Overlay",
                            MB_OK | MB_ICONINFORMATION);
            }
            return 0;
        }
        if (id == IDC_BTN_ADD_FILE && code == BN_CLICKED) {
            BrowseForGameExe(hwnd);
            return 0;
        }
        if (id == IDC_BTN_SCAN && code == BN_CLICKED) {
            std::vector<std::wstring> added = GamesScanForNew();
            const std::vector<std::wstring> fromShortcuts = GamesScanShortcuts();
            for (size_t i = 0; i < fromShortcuts.size(); ++i) {
                bool dup = false;
                for (size_t a = 0; a < added.size(); ++a) {
                    if (added[a] == fromShortcuts[i]) { dup = true; break; }
                }
                if (!dup) added.push_back(fromShortcuts[i]);
            }
            RequestListRefresh();
            wchar_t msg[256];
            if (added.empty()) {
                wsprintfW(msg, L"没有发现新的游戏。\n\n"
                               L"（自动扫描覆盖 Steam / Epic / GOG / Battle.net / EA / "
                               L"Ubisoft / WeGame，以及桌面和开始菜单的快捷方式；"
                               L"其他情况请用『浏览选择 exe…』手动添加。）");
                MessageBoxW(hwnd, msg, L"Speaking Overlay", MB_OK | MB_ICONINFORMATION);
            } else {
                wsprintfW(msg, L"新检测到 %u 个游戏，已默认开启浮窗：\n\n", (unsigned)added.size());
                std::wstring list;
                for (size_t i = 0; i < added.size() && i < 12; ++i) {
                    list += added[i];
                    list += L'\n';
                }
                MessageBoxW(hwnd, (std::wstring(msg) + list).c_str(), L"Speaking Overlay",
                            MB_OK | MB_ICONINFORMATION);
            }
            return 0;
        }
        if ((id == IDC_BTN_ENABLE || id == IDC_BTN_DISABLE) && code == BN_CLICKED) {
            const std::wstring exe = SelectedGame();
            if (!exe.empty()) {
                GamesSetEnabled(exe, id == IDC_BTN_ENABLE);
                RequestListRefresh();
            }
            return 0;
        }
        if (id == IDC_BTN_REMOVE && code == BN_CLICKED) {
            const std::wstring exe = SelectedGame();
            if (!exe.empty()) {
                /* Remember the decision, otherwise the 60 s auto scan puts the
                 * entry straight back and the button appears to do nothing. */
                GamesSetIgnored(exe, true);
                GamesRemove(exe);
                RequestListRefresh();
            }
            return 0;
        }
        if (id == IDC_BTN_CHECK && code == BN_CLICKED) {
            if (g_busy) return 0;
            g_busy = true;
            EnableWindow(g_btnCheck, FALSE);
            EnableWindow(g_btnDownload, FALSE);
            SetWindowTextW(g_updateState, L"正在查询 GitHub…");
            DbgSettings(L"IDC_BTN_CHECK: starting CheckThread");
            CheckCtx* ctx = new CheckCtx();
            ctx->hwnd = hwnd;
            HANDLE th = CreateThread(NULL, 0, CheckThread, ctx, 0, NULL);
            DbgSettings(L"IDC_BTN_CHECK: thread=%p err=%lu", (void*)th,
                        (unsigned long)(th ? 0 : GetLastError()));
            if (th) CloseHandle(th);
            else { delete ctx; g_busy = false; EnableWindow(g_btnCheck, TRUE); }
            return 0;
        }
        if (id == IDC_BTN_DOWNLOAD && code == BN_CLICKED) {
            if (g_busy) return 0;
            if (g_update.downloadUrl.empty()) {
                MessageBoxW(hwnd, L"没有可用的下载地址，请点『打开发布页』手动下载。",
                            L"Speaking Overlay", MB_OK | MB_ICONINFORMATION);
                return 0;
            }
            g_busy = true;
            EnableWindow(g_btnDownload, FALSE);
            EnableWindow(g_btnCheck, FALSE);
            SetWindowTextW(g_updateState, L"正在下载安装包…（文件较大，请稍候）");
            DownloadCtx* ctx = new DownloadCtx();
            ctx->hwnd = hwnd;
            ctx->url  = g_update.downloadUrl;
            ctx->path = TempSetupPath();
            HANDLE th = CreateThread(NULL, 0, DownloadThread, ctx, 0, NULL);
            if (th) CloseHandle(th);
            else { delete ctx; g_busy = false; EnableWindow(g_btnCheck, TRUE); }
            return 0;
        }
        if (id == IDC_BTN_TEXT_COLOR && code == BN_CLICKED) {
            PickTextColor(hwnd);
            return 0;
        }
        if (id == IDC_BTN_BG_COLOR && code == BN_CLICKED) {
            PickBgColor(hwnd);
            return 0;
        }
        if (id == IDC_BTN_LOOK_RESET && code == BN_CLICKED) {
            ResetAppearance();
            return 0;
        }
        if (id == IDC_CMB_FONT && (code == CBN_SELCHANGE || code == CBN_EDITCHANGE)) {
            ApplyAppearance();
            return 0;
        }
        if (id == IDC_BTN_PAGE && code == BN_CLICKED) {
            std::wstring url = g_update.pageUrl;
            if (url.empty()) url = L"https://github.com/EMIRIA2333/TS3SpeakingOverlay/releases";
            ShellExecuteW(NULL, L"open", url.c_str(), NULL, NULL, SW_SHOWNORMAL);
            return 0;
        }
        return 0;
    }

    /* Opacity slider: live-preview while dragging, persist on release. */
    case WM_HSCROLL: {
        if ((HWND)lParam == g_sldAlpha) {
            ApplyAppearance();
        }
        return 0;
    }

    /* Colour swatches are owner-drawn STATES (SS_OWNERDRAW), so the colour is
     * drawn here instead of flipping a background brush. */
    case WM_DRAWITEM: {
        const DRAWITEMSTRUCT* dis = (const DRAWITEMSTRUCT*)lParam;
        if (dis && (dis->CtlID == IDC_SWATCH_TEXT || dis->CtlID == IDC_SWATCH_BG)) {
            DrawSwatch(dis);
            return TRUE;
        }
        break;
    }

    case WM_UPDATE_CHECKED: {
        DbgSettings(L"WM_UPDATE_CHECKED received, lParam=%p", (void*)lParam);
        UpdateResult* r = (UpdateResult*)lParam;
        g_busy = false;
        EnableWindow(g_btnCheck, TRUE);
        if (r) {
            g_update = *r;
            delete r;
        }
        if (!g_update.ok) {
            std::wstring msg = L"检查更新失败：";
            msg += g_update.error.empty() ? L"未知错误" : g_update.error;
            SetWindowTextW(g_updateState, msg.c_str());
            return 0;
        }
        if (!g_update.hasUpdate) {
            std::wstring msg = L"已经是最新版本（";
            msg += g_update.latestTag;
            msg += L"）。";
            SetWindowTextW(g_updateState, msg.c_str());
            return 0;
        }
        {
            std::wstring msg = L"发现新版本 ";
            msg += g_update.latestTag;
            msg += L"，点右侧『下载并安装』即可自动升级。";
            SetWindowTextW(g_updateState, msg.c_str());
        }
        EnableWindow(g_btnDownload, TRUE);
        return 0;
    }

    case WM_UPDATE_DOWNLOADED: {
        std::wstring* payload = (std::wstring*)lParam;
        const bool ok = (wParam != 0);
        g_busy = false;
        EnableWindow(g_btnCheck, TRUE);
        EnableWindow(g_btnDownload, TRUE);

        std::wstring text = payload ? *payload : std::wstring();
        delete payload;

        if (!ok) {
            std::wstring msg = L"下载失败：";
            msg += text.empty() ? L"未知错误" : text;
            msg += L"\n可以点『打开发布页』手动下载安装。";
            SetWindowTextW(g_updateState, msg.c_str());
            return 0;
        }

        g_downloadedSetup = text;
        std::wstring err;
        if (UpdateRunInstaller(g_downloadedSetup, &err)) {
            SetWindowTextW(g_updateState,
                           L"安装包已启动：它会自动关闭 TeamSpeak、替换插件并重新打开。"
                           L"\n安装完成后本窗口会随客户端一起关闭。");
        } else {
            std::wstring msg = L"无法启动安装包：";
            msg += err.empty() ? L"未知错误" : err;
            msg += L"\n安装包已保存在：";
            msg += g_downloadedSetup;
            SetWindowTextW(g_updateState, msg.c_str());
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
        g_chkAutoDetect = g_listGames = g_gameCount = NULL;
        g_btnAddCurrent = g_btnAddFile = g_btnScan = NULL;
        g_btnEnable = g_btnDisable = g_btnRemove = NULL;
        g_updateCurrent = g_btnCheck = g_updateState = g_btnDownload = g_btnPage = NULL;
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
        RefreshGameList();
        ShowWindow(g_hwnd, SW_SHOWNORMAL);
        SetForegroundWindow(g_hwnd);
        return;
    }

    HINSTANCE inst = GetModuleHandleW(NULL);
    if (!EnsureClass(inst)) return;

    INITCOMMONCONTROLSEX icc;
    icc.dwSize = sizeof(icc);
    icc.dwICC  = ICC_LISTVIEW_CLASSES | ICC_STANDARD_CLASSES | ICC_BAR_CLASSES;
    InitCommonControlsEx(&icc);

    /* IMPORTANT: `owner` is the Qt QWidget* the client passes to
     * ts3plugin_configure - it is NOT an HWND. Handing it to CreateWindowExW as
     * the parent makes the call fail, so the window is always created as a
     * top-level window. */
    (void)owner;
    g_hwnd = CreateWindowExW(WS_EX_TOOLWINDOW,
                             kSettingsClass, kSettingsTitle,
                             WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_THICKFRAME,
                             CW_USEDEFAULT, CW_USEDEFAULT, 820, 820,
                             NULL, NULL, inst, NULL);
    if (!g_hwnd) return;

    g_threadId = GetCurrentThreadId();

    RECT target;
    ZeroMemory(&target, sizeof(target));
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &target, 0);
    if (target.right > target.left) {
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

    /* Modal loop: ts3plugin_configure must block while the window is open.
     *
     * IsDialogMessageW() gives us Tab/Enter navigation, but it must never see
     * the private worker messages: it walks the dialog's children and drops
     * anything it cannot route, which is exactly how the update result used to
     * disappear. Everything >= WM_APP therefore goes straight to Dispatch. */
    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        if (msg.message < WM_APP && IsDialogMessageW(g_hwnd, &msg)) continue;
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
    if (g_hwnd && IsWindow(g_hwnd)) {
        SyncFromOverlay();
        RefreshGameList();
        ShowWindow(g_hwnd, SW_SHOWNORMAL);
        SetForegroundWindow(g_hwnd);
        return;
    }
    HANDLE th = CreateThread(NULL, 0, AsyncThread, NULL, 0, NULL);
    if (th) CloseHandle(th);
}

void SettingsShowUpdateAsync()
{
    g_pendingAutoCheck = true;
    SettingsShowAsync();
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

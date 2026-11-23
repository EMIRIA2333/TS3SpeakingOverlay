/*
 * ts3-speaking-overlay - always-on-top "who is talking" overlay (KOOK style)
 * overlay.cpp : layered topmost window rendered with GDI+ (per-pixel alpha)
 *
 * Design notes
 *  - WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, never in the taskbar.
 *  - UpdateLayeredWindow with a 32bpp PARGB DIB, so rounded corners and the
 *    translucent halo are anti-aliased and truly transparent (no color key).
 *  - Two rendering passes on the same DIB: GDI+ draws the panel, avatars and
 *    rings, then a Direct2D DC render target draws the names. D2D is used for
 *    text only because GDI+ has NO font fallback: a name containing an emoji
 *    renders as empty boxes with DrawString, while DirectWrite substitutes a
 *    colour-emoji font (D2D1_DRAW_TEXT_OPTIONS_ENABLE_COLOR_FONT). Verified
 *    that a D2D DC render target blends onto existing GDI+ content instead of
 *    wiping it, and that it writes correct premultiplied alpha for ULW_ALPHA.
 *  - Rendering happens on our own thread; state is handed over through a
 *    mutex-protected snapshot so the TS3 callback thread never blocks on GDI.
 *  - Position is persisted in overlay.ini next to the plugin; the window can be
 *    dragged with the left mouse button and locked through the plugin menu.
 *  - All GDI+ objects live in an inner scope: GdiplusShutdown() crashes if any
 *    Bitmap/Graphics/Font is still alive.
 */
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <objidl.h>
#include <propidl.h>   /* mingw: must come before gdiplus.h (PROPID errors) */
#include <gdiplus.h>
#include <d2d1.h>
#include <dwrite.h>
#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cwctype>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "overlay.h"

using namespace Gdiplus;

namespace ts3ovl {
namespace {

/* ------------------------------------------------------------------ */
/* tunables                                                            */
/* ------------------------------------------------------------------ */
const int   kAvatarSize      = 26;   /* avatar diameter in px              */
const int   kRowHeight       = 32;   /* vertical advance per speaker       */
const int   kPaddingX        = 8;    /* left/right padding                 */
const int   kPaddingY        = 6;    /* top/bottom padding                 */
const int   kGap             = 7;    /* space between avatar and text      */
const int   kCornerRadius    = 8;    /* panel corner radius                */
const int   kMaxNameWidth    = 130;  /* text column width                  */
const int   kFontSizePx      = 12;   /* name font size                     */
const int   kMaxRows         = 12;   /* hard cap on simultaneously shown   */
const int   kSnapDist        = 36;   /* edge snap distance on drag release */

/* colours (ARGB) */
/* The panel background is fully transparent by request: only the rounded
 * frame is drawn, so whatever is behind the overlay stays visible. The border
 * is therefore a bit stronger than it used to be - against a bright game it
 * still has to read as a frame. */
const ARGB  kBorder          = 0x88FFFFFF;
/* Dark halo drawn under the border so the frame stays visible on light
 * backgrounds (the panel background itself is fully transparent). */
const ARGB  kPanelHalo       = 0x70000000;
const ARGB  kRingActive      = 0xFF37C26A; /* KOOK-ish speaking green       */
const ARGB  kRingIdle        = 0x00FFFFFF;
const ARGB  kText            = 0xFFFFFFFF;
const ARGB  kAvatarFallback  = 0xFF3A3D4A;

/* Members that are not talking stay in the list but are drawn at this opacity
 * (out of 255) instead of disappearing - the panel keeps its size and only the
 * highlight moves, which is how KOOK behaves. */
const int   kDimAlpha        = 96;

const UINT_PTR kTimerRedraw   = 1;
const int      kRedrawMs      = 40;   /* gentle pulse while someone talks   */
const int      kIdleMs        = 250;  /* idle repaint: only the game gate   */

/* ------------------------------------------------------------------ */
/* shared state                                                        */
/* ------------------------------------------------------------------ */
struct Shared {
    std::mutex                  lock;
    std::vector<Entry>          entries;
    std::map<unsigned int, std::wstring> avatars;
    bool                        visible   = true;
    bool                        locked    = false;
    bool                        onlyInGame = true;  /* KOOK-like default: games only */
    float                       pulse     = 0.0f;
} g_shared;

HWND          g_hwnd       = NULL;
HANDLE        g_thread     = NULL;
DWORD         g_threadId   = 0;
HINSTANCE     g_inst       = NULL;
std::wstring  g_iniPath;
ULONG_PTR     g_gdiplusTok = 0;
bool          g_gdiPlusUp  = false;

int           g_width      = 260;   /* current client size, recomputed on layout */
int           g_height     = 0;
int           g_x          = -10000;
int           g_y          = -10000;

bool          g_dragging   = false;
POINT         g_dragOrigin = {0, 0};
int           g_dragStartX = 0;
int           g_dragStartY = 0;

/* cached fonts / brushes, rebuilt on DPI change */
Font*         g_fontName    = NULL;
Font*         g_fontStatus  = NULL;

std::mutex    g_gdiLock;    /* protects the cached GDI+ objects */
int           g_currentDpi  = 96;

/* Direct2D/DirectWrite objects: used for the name text only, because GDI+ has
 * no font fallback and therefore cannot draw emoji. All of them are created and
 * destroyed on the overlay thread, so they need no lock. */
ID2D1Factory*          g_d2d      = NULL;
ID2D1DCRenderTarget*   g_d2dRt    = NULL;
ID2D1SolidColorBrush*  g_d2dText  = NULL;
ID2D1SolidColorBrush*  g_d2dDim   = NULL;   /* names of members not talking */
ID2D1SolidColorBrush*  g_d2dShadow    = NULL;  /* 1px outline behind names   */
ID2D1SolidColorBrush*  g_d2dShadowDim = NULL;
IDWriteFactory*        g_dw       = NULL;
IDWriteTextFormat*     g_dwFormat = NULL;
int                    g_d2dDpi   = 0;   /* dpi the text format was built for */

/* ------------------------------------------------------------------ */
/* helpers                                                             */
/* ------------------------------------------------------------------ */
inline int Dip(int px, int dpi) { return MulDiv(px, dpi, 96); }

/* Optional tracing: set TS3OVL_DEBUG=1 to log into %TEMP%\ts3ovl_dbg.log. */
void Dbg(const wchar_t* fmt, ...)
{
    static int enabled = -1;
    if (enabled < 0) {
        wchar_t v[8] = {0};
        enabled = (GetEnvironmentVariableW(L"TS3OVL_DEBUG", v, 8) > 0 && v[0] == L'1') ? 1 : 0;
    }
    if (!enabled) return;

    wchar_t path[MAX_PATH];
    DWORD n = GetEnvironmentVariableW(L"TEMP", path, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return;
    wcscat_s(path, MAX_PATH, L"\\ts3ovl_dbg.log");

    FILE* f = NULL;
    if (_wfopen_s(&f, path, L"a, ccs=UTF-8") != 0 || !f) return;
    va_list ap;
    va_start(ap, fmt);
    vfwprintf(f, fmt, ap);
    va_end(ap);
    fwprintf(f, L"\n");
    fclose(f);
}


std::wstring IniGet(const wchar_t* key, const wchar_t* def)
{
    wchar_t buf[128] = {0};
    if (g_iniPath.empty()) return def;
    GetPrivateProfileStringW(L"overlay", key, def, buf, 128, g_iniPath.c_str());
    return buf;
}

void IniSet(const wchar_t* key, int value)
{
    if (g_iniPath.empty()) return;
    wchar_t buf[32];
    wsprintfW(buf, L"%d", value);
    WritePrivateProfileStringW(L"overlay", key, buf, g_iniPath.c_str());
}

void SavePosition()
{
    IniSet(L"x", g_x);
    IniSet(L"y", g_y);
    IniSet(L"locked", g_shared.locked ? 1 : 0);
    IniSet(L"onlyInGame", g_shared.onlyInGame ? 1 : 0);
}

void LoadPosition()
{
    g_x = _wtoi(IniGet(L"x", L"-10000").c_str());
    g_y = _wtoi(IniGet(L"y", L"-10000").c_str());
    g_shared.locked     = _wtoi(IniGet(L"locked", L"0").c_str()) != 0;
    g_shared.onlyInGame = _wtoi(IniGet(L"onlyInGame", L"1").c_str()) != 0;
}

/* Default position: right edge of the primary work area, vertically centred. */
void DefaultPosition(int w, int h)
{
    RECT wa = {0, 0, 1920, 1080};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
    g_x = wa.right - w - 28;
    g_y = wa.top + (wa.bottom - wa.top - h) / 2;
    if (g_x < wa.left) g_x = wa.left;
    if (g_y < wa.top)  g_y = wa.top;
}

/* ------------------------------------------------------------------ */
/* game detection                                                      */
/* ------------------------------------------------------------------ */
/* Exe names that are definitely not games; a foreground window belonging to
 * one of these never counts as "in game". Lower-case, no extension path. */
const wchar_t* const kNonGameExes[] = {
    L"explorer.exe", L"ts3client_win64.exe", L"ts3client_win32.exe",
    L"chrome.exe", L"msedge.exe", L"firefox.exe", L"brave.exe", L"opera.exe",
    L"vivaldi.exe", L"360se.exe", L"qqbrowser.exe", L"sogouexplorer.exe",
    L"discord.exe", L"kook.exe", L"qq.exe", L"wechat.exe", L"weixin.exe",
    L"tim.exe", L"dingtalk.exe", L"wxwork.exe", L"telegram.exe",
    L"code.exe", L"devenv.exe", L"notepad.exe", L"notepad++.exe",
    L"notepad3.exe", L"wps.exe", L"winword.exe", L"excel.exe", L"powerpnt.exe",
    L"acrobat.exe", L"acrord32.exe", L"sumatrapdf.exe",
    L"cmd.exe", L"powershell.exe", L"pwsh.exe", L"windowsterminal.exe",
    L"conhost.exe", L"wt.exe", L"wt3.exe", L"bash.exe", L"wsl.exe",
    L"taskmgr.exe", L"regedit.exe", L"control.exe", L"mmc.exe",
    L"searchapp.exe", L"startmenuexperiencehost.exe", L"shellexperiencehost.exe",
    L"applicationframehost.exe", L"lockapp.exe", L"textinputhost.exe",
    L"msiafterburner.exe", L"rtss.exe", L"rivatuner.exe", L"fraps.exe",
    L"obs64.exe", L"obs32.exe", L"streamlabs obs.exe",
    L"potplayermini64.exe", L"vlc.exe", L"mpc-hc64.exe", L"mpc-be64.exe",
    L"cloudmusic.exe", L"qqmusic.exe", L"spotify.exe",
    L"photoshop.exe", L"illustrator.exe", L"lightroom.exe", L"figma.exe",
    L"idea64.exe", L"pycharm64.exe", L"clion64.exe", L"goland64.exe",
    L"webstorm64.exe", L"rider64.exe", L"sublime_text.exe", L"atom.exe",
    L"everything.exe", L"7zfm.exe", L"winrar.exe", L"bandizip.exe",
    /* screen-capture / vendor overlay tools sit fullscreen on top of games and
     * would otherwise be mistaken for one (e.g. CEF-OSC-WIDGET, 2047x1152) */
    L"nvidia overlay.exe", L"nvidia share.exe", L"nvsphelper64.exe",
    L"nvcontainer.exe", L"radeonsoftware.exe", L"amdow.exe", L"amdfendrsr.exe",
    L"gamebar.exe", L"gamebarftserver.exe", L"xboxgamebarwidgets.exe",
    L"xboxapp.exe", L"igfxem.exe", L"igfxtray.exe", L"displaylink.exe",
    NULL
};

/* Lower-case file name (no directory) of the given process id. */
std::wstring ProcessExeName(DWORD pid)
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

bool IsExeBlacklisted(const std::wstring& exe)
{
    for (int i = 0; kNonGameExes[i]; ++i) {
        if (exe == kNonGameExes[i]) return true;
    }
    return false;
}

/* True when the given window belongs to a fullscreen/background-less app that
 * is not the shell and not one of the known desktop apps. That is the
 * practical definition of "the user is in a game". Split out from
 * IsGameForeground() so it can be exercised with an explicit window. */
bool IsGameWindow(HWND fg)
{
    if (!fg || fg == g_hwnd) return false;

    /* desktop / shell windows are never games */
    wchar_t cls[64] = {0};
    GetClassNameW(fg, cls, 64);
    if (!wcscmp(cls, L"Progman") || !wcscmp(cls, L"WorkerW") ||
        !wcscmp(cls, L"Shell_TrayWnd") || !wcscmp(cls, L"Shell_SecondaryTrayWnd") ||
        !wcscmp(cls, L"NotifyIconOverflowWindow") || !wcscmp(cls, L"Windows.UI.Core.CoreWindow") ||
        !wcscmp(cls, L"ApplicationFrameWindow") || !wcscmp(cls, L"TaskListThumbnailWnd") ||
        /* vendor overlay widgets drawn by a helper process, not by the game */
        !wcscmp(cls, L"CEF-OSC-WIDGET") || !wcscmp(cls, L"Chrome_WidgetWin_1") ||
        !wcscmp(cls, L"EdgeUiInputTopWndClass") || !wcscmp(cls, L"Windows.UI.Composition.DesktopWindowContentBridge")) {
        return false;
    }

    if (IsIconic(fg) || !IsWindowVisible(fg)) return false;

    DWORD pid = 0;
    GetWindowThreadProcessId(fg, &pid);
    if (pid == 0) return false;
    std::wstring exe = ProcessExeName(pid);
    if (exe.empty() || IsExeBlacklisted(exe)) return false;

    /* does the window cover its monitor? (exclusive or borderless fullscreen) */
    RECT wr;
    if (!GetWindowRect(fg, &wr)) return false;
    HMONITOR mon = MonitorFromWindow(fg, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi;
    ZeroMemory(&mi, sizeof(mi));
    mi.cbSize = sizeof(mi);
    if (!GetMonitorInfoW(mon, &mi)) return false;

    const int tol = 2;
    int w = wr.right - wr.left;
    int h = wr.bottom - wr.top;
    int mw = mi.rcMonitor.right - mi.rcMonitor.left;
    int mh = mi.rcMonitor.bottom - mi.rcMonitor.top;
    if (w >= mw - tol && h >= mh - tol &&
        wr.left <= mi.rcMonitor.left + tol && wr.top <= mi.rcMonitor.top + tol) {
        return true;
    }

    /* not fullscreen: fall back to "no caption, no border, larger than half
     * the screen" which catches most windowed-borderless games */
    LONG_PTR style   = GetWindowLongPtrW(fg, GWL_STYLE);
    LONG_PTR exStyle = GetWindowLongPtrW(fg, GWL_EXSTYLE);
    bool noCaption = (style & WS_CAPTION) != WS_CAPTION;
    bool noBorder  = (style & WS_THICKFRAME) == 0;
    bool notTool   = (exStyle & WS_EX_TOOLWINDOW) == 0;
    bool topMost   = (exStyle & WS_EX_TOPMOST) == 0;   /* always-on-top apps are overlays/launchers */
    if (noCaption && noBorder && notTool && topMost &&
        w >= mw / 2 && h >= mh / 2) {
        return true;
    }
    return false;
}

/* Foreground-window wrapper used by the per-frame gate. */
bool IsGameForeground()
{
    return IsGameWindow(GetForegroundWindow());
}

/* Snap the given position to the nearest monitor edge when it is close. */
void SnapToEdges(int& x, int& y, int w, int h)
{
    HMONITOR mon = MonitorFromPoint(POINT{x + w / 2, y + h / 2}, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi;
    ZeroMemory(&mi, sizeof(mi));
    mi.cbSize = sizeof(mi);
    if (!GetMonitorInfoW(mon, &mi)) return;

    const RECT& wa = mi.rcWork;
    const int snap = kSnapDist;

    if (abs(x - wa.left) <= snap)                 x = wa.left;
    else if (abs((x + w) - wa.right) <= snap)     x = wa.right - w;

    if (abs(y - wa.top) <= snap)                  y = wa.top;
    else if (abs((y + h) - wa.bottom) <= snap)    y = wa.bottom - h;

    /* never leave the work area */
    if (x < wa.left)              x = wa.left;
    if (x + w > wa.right)         x = wa.right - w;
    if (y < wa.top)               y = wa.top;
    if (y + h > wa.bottom)        y = wa.bottom - h;
}

/* ------------------------------------------------------------------ */
/* layout                                                              */
/* ------------------------------------------------------------------ */
struct Layout {
    int width;
    int height;
    int avatarSize;
    int rowHeight;
    int padX;
    int padY;
    int gap;
    int radius;
    int nameWidth;
    int fontSize;
};

/* ------------------------------------------------------------------ */
/* Direct2D / DirectWrite text engine                                  */
/*                                                                     */
/* GDI+ has no font fallback at all: a nickname containing an emoji     */
/* renders as empty boxes because Microsoft YaHei UI has no glyph for   */
/* it. DirectWrite falls back to Segoe UI Emoji, and with               */
/* D2D1_DRAW_TEXT_OPTIONS_ENABLE_COLOR_FONT the glyphs even come out in */
/* colour. Text therefore goes through D2D, while the panel, avatars    */
/* and rings stay with GDI+ (verified: a D2D DC render target blends    */
/* onto existing GDI+ pixels instead of wiping them, and writes correct */
/* premultiplied alpha for UpdateLayeredWindow).                        */
/* ------------------------------------------------------------------ */
inline float Px2Dip(int px, int dpi)
{
    return (float)px * 96.0f / (float)(dpi > 0 ? dpi : 96);
}

void TeardownTextEngine()
{
    if (g_dwFormat) { g_dwFormat->Release(); g_dwFormat = NULL; }
    if (g_d2dText)  { g_d2dText->Release();  g_d2dText  = NULL; }
    if (g_d2dDim)   { g_d2dDim->Release();   g_d2dDim   = NULL; }
    if (g_d2dShadow)    { g_d2dShadow->Release();    g_d2dShadow    = NULL; }
    if (g_d2dShadowDim) { g_d2dShadowDim->Release(); g_d2dShadowDim = NULL; }
    if (g_dw)       { g_dw->Release();       g_dw       = NULL; }
    if (g_d2dRt)    { g_d2dRt->Release();    g_d2dRt    = NULL; }
    if (g_d2d)      { g_d2d->Release();      g_d2d      = NULL; }
    g_d2dDpi = 0;
}

/* Build (or rebuild for a new DPI) the text engine.
 * Returns false when Direct2D is unavailable; callers then fall back to GDI+. */
bool InitTextEngine(int dpi)
{
    if (g_dwFormat && g_d2dDpi == dpi) return true;
    TeardownTextEngine();

    if (!g_d2d) {
        if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, &g_d2d)) || !g_d2d) {
            g_d2d = NULL;
            Dbg(L"[d2d] factory failed, falling back to GDI+ text");
            return false;
        }
    }
    if (!g_d2dRt) {
        D2D1_RENDER_TARGET_PROPERTIES rtp;
        ZeroMemory(&rtp, sizeof(rtp));
        rtp.type                  = D2D1_RENDER_TARGET_TYPE_DEFAULT;
        rtp.pixelFormat.format    = DXGI_FORMAT_B8G8R8A8_UNORM;
        rtp.pixelFormat.alphaMode = D2D1_ALPHA_MODE_PREMULTIPLIED;
        rtp.usage                 = D2D1_RENDER_TARGET_USAGE_NONE;
        rtp.minLevel              = D2D1_FEATURE_LEVEL_DEFAULT;
        if (FAILED(g_d2d->CreateDCRenderTarget(&rtp, &g_d2dRt)) || !g_d2dRt) {
            g_d2dRt = NULL;
            return false;
        }
        /* grayscale AA: ClearType would bleed colour fringes into the alpha edges */
        g_d2dRt->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
    }
    if (!g_dw) {
        if (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                       reinterpret_cast<IUnknown**>(&g_dw))) || !g_dw) {
            g_dw = NULL;
            return false;
        }
    }

    /* font size is kFontSizePx DIPs; SetDpi maps DIPs onto the pixel surface */
    g_d2dRt->SetDpi((float)dpi, (float)dpi);
    if (FAILED(g_dw->CreateTextFormat(L"Microsoft YaHei UI", NULL,
                                      DWRITE_FONT_WEIGHT_NORMAL,
                                      DWRITE_FONT_STYLE_NORMAL,
                                      DWRITE_FONT_STRETCH_NORMAL,
                                      (float)kFontSizePx, L"", &g_dwFormat)) || !g_dwFormat) {
        g_dwFormat = NULL;
        return false;
    }
    g_dwFormat->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    g_dwFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
    g_dwFormat->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    /* ellipsis, like GDI+'s StringTrimmingEllipsisCharacter */
    IDWriteInlineObject* sign = NULL;
    if (SUCCEEDED(g_dw->CreateEllipsisTrimmingSign(g_dwFormat, &sign)) && sign) {
        DWRITE_TRIMMING trim;
        trim.granularity    = DWRITE_TRIMMING_GRANULARITY_CHARACTER;
        trim.delimiter      = 0;
        trim.delimiterCount = 0;
        g_dwFormat->SetTrimming(&trim, sign);
        sign->Release();   /* the text format keeps its own reference */
    }
    g_d2dDpi = dpi;
    Dbg(L"[d2d] text engine ready dpi=%d", dpi);
    return true;
}

/* Pixel width of a name; -1 when the text engine is unavailable. */
int MeasureNameD2D(const std::wstring& name)
{
    if (!g_dw || !g_dwFormat) return -1;
    IDWriteTextLayout* layout = NULL;
    if (FAILED(g_dw->CreateTextLayout(name.c_str(), (UINT32)name.size(), g_dwFormat,
                                      100000.0f, 1000.0f, &layout)) || !layout) {
        return -1;
    }
    DWRITE_TEXT_METRICS m;
    int w = -1;
    if (SUCCEEDED(layout->GetMetrics(&m))) {
        w = (int)std::ceil(m.widthIncludingTrailingWhitespace * (float)g_d2dDpi / 96.0f);
    }
    layout->Release();
    return w;
}

/* Second render pass: the names, drawn over the finished GDI+ panel.
 * memDc must still have the target DIB selected. Non-talking members get a
 * dimmed brush instead of being skipped. */
void DrawNamesD2D(HDC memDc, int w, int h,
                  const std::vector<Entry>& entries, const Layout& L)
{
    if (!g_d2dRt || !g_dwFormat) return;
    RECT rc = { 0, 0, w, h };
    if (FAILED(g_d2dRt->BindDC(memDc, &rc))) return;
    if (!g_d2dText) {
        if (FAILED(g_d2dRt->CreateSolidColorBrush(D2D1::ColorF(1.0f, 1.0f, 1.0f, 1.0f),
                                                  &g_d2dText)) || !g_d2dText) {
            return;
        }
    }
    if (!g_d2dDim) {
        const float d = (float)kDimAlpha / 255.0f;
        if (FAILED(g_d2dRt->CreateSolidColorBrush(D2D1::ColorF(1.0f, 1.0f, 1.0f, d),
                                                  &g_d2dDim)) || !g_d2dDim) {
            return;
        }
    }
    /* With the panel background gone the white names sit directly on whatever
     * the game renders, so they need a dark 1px outline to stay readable on
     * bright scenes. It is the same glyph drawn once in near-black, offset. */
    if (!g_d2dShadow) {
        if (FAILED(g_d2dRt->CreateSolidColorBrush(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.75f),
                                                  &g_d2dShadow)) || !g_d2dShadow) {
            return;
        }
    }
    if (!g_d2dShadowDim) {
        const float d = (float)kDimAlpha / 255.0f * 0.75f;
        if (FAILED(g_d2dRt->CreateSolidColorBrush(D2D1::ColorF(0.0f, 0.0f, 0.0f, d),
                                                  &g_d2dShadowDim)) || !g_d2dShadowDim) {
            return;
        }
    }
    int dpi = g_d2dDpi > 0 ? g_d2dDpi : 96;
    g_d2dRt->BeginDraw();
    for (size_t i = 0; i < entries.size(); ++i) {
        float rowTop = Px2Dip(L.padY + (int)i * L.rowHeight, dpi);
        float rowH   = Px2Dip(L.rowHeight, dpi);
        float textX  = Px2Dip(L.padX + L.avatarSize + L.gap, dpi);
        /* +2px slack so the trailing glyph is not clipped by rounding */
        float textW  = Px2Dip(L.nameWidth + 2, dpi);
        const float off = Px2Dip(1, dpi);
        D2D1_RECT_F rs = D2D1::RectF(textX + off, rowTop + off,
                                     textX + textW + off, rowTop + rowH + off);
        g_d2dRt->DrawTextW(entries[i].name.c_str(), (UINT32)entries[i].name.size(),
                           g_dwFormat, &rs,
                           entries[i].talking ? g_d2dShadow : g_d2dShadowDim,
                           D2D1_DRAW_TEXT_OPTIONS_ENABLE_COLOR_FONT,
                           DWRITE_MEASURING_MODE_NATURAL);
        D2D1_RECT_F r = D2D1::RectF(textX, rowTop, textX + textW, rowTop + rowH);
        g_d2dRt->DrawTextW(entries[i].name.c_str(), (UINT32)entries[i].name.size(),
                           g_dwFormat, &r,
                           entries[i].talking ? g_d2dText : g_d2dDim,
                           D2D1_DRAW_TEXT_OPTIONS_ENABLE_COLOR_FONT,
                           DWRITE_MEASURING_MODE_NATURAL);
    }
    HRESULT hr = g_d2dRt->EndDraw();
    if (FAILED(hr)) Dbg(L"[d2d] EndDraw failed 0x%08lX", (unsigned long)hr);
}

/* Initial glyph for members without a cached avatar. Drawn with Direct2D too,
 * because nicknames frequently start with an emoji and GDI+ cannot draw one
 * (no font fallback -> empty box). The disc itself is drawn by DrawAvatar. */
void DrawInitialsD2D(HDC memDc, int w, int h,
                     const std::vector<Entry>& entries, const Layout& L)
{
    if (!g_d2dRt || !g_dwFormat || !g_dw) return;

    /* Dedicated centred format: the name format is left-aligned and would
     * push a single glyph to the left edge of the disc's box. */
    static IDWriteTextFormat* s_center = NULL;
    static int s_centerDpi = 0;
    if (!s_center || s_centerDpi != g_d2dDpi) {
        if (s_center) { s_center->Release(); s_center = NULL; }
        if (FAILED(g_dw->CreateTextFormat(L"Microsoft YaHei UI", NULL,
                                          DWRITE_FONT_WEIGHT_NORMAL,
                                          DWRITE_FONT_STYLE_NORMAL,
                                          DWRITE_FONT_STRETCH_NORMAL,
                                          (float)kFontSizePx, L"", &s_center)) || !s_center) {
            s_center = NULL;
            return;
        }
        s_center->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        s_center->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        s_center->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        s_centerDpi = g_d2dDpi;
    }

    RECT rc = { 0, 0, w, h };
    if (FAILED(g_d2dRt->BindDC(memDc, &rc))) return;

    int dpi = g_d2dDpi > 0 ? g_d2dDpi : 96;
    g_d2dRt->BeginDraw();
    bool drewAnything = false;
    for (size_t i = 0; i < entries.size(); ++i) {
        if (!entries[i].avatarPath.empty()) continue;   /* real image, no disc */
        if (entries[i].name.empty()) continue;

        /* brush: white for speakers, dimmed for the rest */
        ID2D1SolidColorBrush* br = NULL;
        const float a = entries[i].talking ? 1.0f : (float)kDimAlpha / 255.0f;
        if (FAILED(g_d2dRt->CreateSolidColorBrush(D2D1::ColorF(1.0f, 1.0f, 1.0f, a), &br)) || !br)
            continue;
        drewAnything = true;

        float rowTop = (float)(L.padY + (int)i * L.rowHeight);
        float avY    = rowTop + (L.rowHeight - L.avatarSize) / 2.0f;
        D2D1_RECT_F r = D2D1::RectF(Px2Dip(L.padX, dpi), Px2Dip((int)avY, dpi),
                                    Px2Dip(L.padX + L.avatarSize, dpi),
                                    Px2Dip((int)avY + L.avatarSize, dpi));
        /* one glyph only; a surrogate pair (emoji) is two UTF-16 units */
        UINT32 len = 1;
        if (entries[i].name[0] >= 0xD800 && entries[i].name[0] <= 0xDBFF &&
            entries[i].name.size() > 1) {
            len = 2;
        }
        g_d2dRt->DrawTextW(entries[i].name.c_str(), len, s_center, &r, br,
                           D2D1_DRAW_TEXT_OPTIONS_ENABLE_COLOR_FONT,
                           DWRITE_MEASURING_MODE_NATURAL);
        br->Release();
    }
    if (drewAnything) {
        HRESULT hr = g_d2dRt->EndDraw();
        if (FAILED(hr)) Dbg(L"[d2d] initial EndDraw failed 0x%08lX", (unsigned long)hr);
    } else {
        g_d2dRt->EndDraw();
    }
}

/* ------------------------------------------------------------------ */
Layout ComputeLayout(int count, int dpi, const std::vector<Entry>& entries)
{
    Layout L;
    L.avatarSize = Dip(kAvatarSize, dpi);
    L.rowHeight  = Dip(kRowHeight, dpi);
    L.padX       = Dip(kPaddingX, dpi);
    L.padY       = Dip(kPaddingY, dpi);
    L.gap        = Dip(kGap, dpi);
    L.radius     = Dip(kCornerRadius, dpi);
    L.fontSize   = Dip(kFontSizePx, dpi);

    /* width adapts to the longest name, clamped.
     * Measured with DirectWrite so the width matches what DrawNamesD2D will
     * actually render - GDI+ reports the box width for fallback glyphs and
     * would size the panel for the wrong text. */
    int widest = 0;
    bool measured = g_dwFormat != NULL;
    if (measured) {
        for (size_t i = 0; i < entries.size(); ++i) {
            int w = MeasureNameD2D(entries[i].name);
            if (w > 0) widest = std::max(widest, w);
        }
    }
    if (!measured) {
        /* Direct2D unavailable: fall back to GDI+ metrics */
        std::lock_guard<std::mutex> lk(g_gdiLock);
        if (g_fontName) {
            /* GDI+ has no Font::MeasureString in the mingw headers; a scratch
             * 1x1 bitmap Graphics (or the screen DC) does the measuring. */
            Bitmap scratch(1, 1, PixelFormat32bppPARGB);
            Graphics mg(&scratch);
            if (mg.GetLastStatus() == Ok) {
                for (size_t i = 0; i < entries.size(); ++i) {
                    RectF box;
                    mg.SetTextRenderingHint(TextRenderingHintAntiAliasGridFit);
                    if (mg.MeasureString(entries[i].name.c_str(), -1, g_fontName,
                                         PointF(0, 0), &box) == Ok) {
                        widest = std::max(widest, (int)std::ceil(box.Width));
                    }
                }
            }
        }
    }
    L.nameWidth  = std::min(std::max(widest, Dip(90, dpi)), Dip(kMaxNameWidth, dpi));

    L.width  = L.padX * 2 + L.avatarSize + L.gap + L.nameWidth;
    L.height = count > 0 ? L.padY * 2 + count * L.rowHeight : 0;
    return L;
}

/* ------------------------------------------------------------------ */
/* painting                                                            */
/* ------------------------------------------------------------------ */
void AddRoundedRect(GraphicsPath& p, const RectF& r, float radius)
{
    float d = radius * 2.0f;
    if (d <= 0.0f) {
        p.AddRectangle(r);
        return;
    }
    p.AddArc(r.X, r.Y, d, d, 180, 90);
    p.AddArc(r.X + r.Width - d, r.Y, d, d, 270, 90);
    p.AddArc(r.X + r.Width - d, r.Y + r.Height - d, d, d, 0, 90);
    p.AddArc(r.X, r.Y + r.Height - d, d, d, 90, 90);
    p.CloseFigure();
}

/* Draw one avatar: cached image if available, else a coloured initial disc.
 * `alpha` (0..255) dims the whole thing for members that are not talking. */
void DrawAvatar(Graphics& g, const Entry& e, const RectF& box, int alpha)
{
    GraphicsPath circle;
    circle.AddEllipse(box);
    g.SetClip(&circle);

    const int a = alpha < 0 ? 0 : (alpha > 255 ? 255 : alpha);
    bool drawn = false;
    if (!e.avatarPath.empty()) {
        Bitmap* bmp = new Bitmap(e.avatarPath.c_str());
        if (bmp && bmp->GetLastStatus() == Ok && bmp->GetWidth() > 0) {
            /* scale to cover the circle */
            float bw = (float)bmp->GetWidth();
            float bh = (float)bmp->GetHeight();
            float scale = std::max(box.Width / bw, box.Height / bh);
            float dw = bw * scale;
            float dh = bh * scale;
            float dx = box.X + (box.Width - dw) / 2.0f;
            float dy = box.Y + (box.Height - dh) / 2.0f;
            if (a >= 255) {
                g.DrawImage(bmp, RectF(dx, dy, dw, dh));
            } else {
                /* dim by scaling the image's own alpha channel. MinGW types
                 * ColorMatrix as a bare REAL[5][5], so fill it at runtime
                 * instead of using an aggregate initialiser. */
                ColorMatrix cm;
                ZeroMemory(&cm, sizeof(cm));
                cm.m[0][0] = cm.m[1][1] = cm.m[2][2] = 1.0f;
                cm.m[3][3] = (REAL)a / 255.0f;
                cm.m[4][4] = 1.0f;
                ImageAttributes ia;
                ia.SetColorMatrix(&cm, ColorMatrixFlagsDefault, ColorAdjustTypeBitmap);
                g.DrawImage(bmp, RectF(dx, dy, dw, dh), 0.0f, 0.0f, bw, bh,
                            UnitPixel, &ia);
            }
            drawn = true;
        }
        delete bmp;
    }

    if (!drawn) {
        SolidBrush fb{Color(a, 0x3A, 0x3D, 0x4A)};
        g.FillEllipse(&fb, box);
        /* The initial glyph is NOT drawn here when Direct2D is available: it
         * goes through DrawInitialsD2D instead, because a great many TS3
         * nicknames start with an emoji and GDI+ has no font fallback (it
         * draws an empty box). Only without Direct2D do we fall back. */
        if (!g_dwFormat && g_fontName) {
            /* first character of the name, centred */
            wchar_t initial[2] = { e.name.empty() ? L'?' : e.name[0], 0 };
            StringFormat sf;
            sf.SetAlignment(StringAlignmentCenter);
            sf.SetLineAlignment(StringAlignmentCenter);
            SolidBrush tb{Color(a, 0xFF, 0xFF, 0xFF)};
            g.DrawString(initial, -1, g_fontName, box, &sf, &tb);
        }
    }
    g.ResetClip();
}

/* Draw the ring around an avatar. `t` >= 0 drives the speaking pulse (0..1);
 * a negative `t` means "static outline" and uses `alphaOverride` instead. */
void DrawRing(Graphics& g, const RectF& box, float radius, float t,
              ARGB base, int thickness, int alphaOverride = -1)
{
    float grow = 0.0f;
    int alpha;
    if (t < 0.0f) {
        alpha = alphaOverride < 0 ? 60 : alphaOverride;
    } else {
        grow  = radius * (0.06f * t);
        alpha = (int)(150 + 105 * t);
    }
    if (alpha > 255) alpha = 255;
    if (alpha <= 0) return;
    RectF r(box.X - grow, box.Y - grow, box.Width + grow * 2, box.Height + grow * 2);
    Color c((BYTE)alpha, (BYTE)((base >> 16) & 0xFF), (BYTE)((base >> 8) & 0xFF), (BYTE)(base & 0xFF));
    Pen pen{c, (float)thickness};
    pen.SetAlignment(PenAlignmentCenter);
    g.DrawEllipse(&pen, r);
}

void Paint(HWND hwnd)
{
    std::vector<Entry> entries;
    float pulse;
    {
        std::lock_guard<std::mutex> lk(g_shared.lock);
        entries = g_shared.entries;
        pulse   = g_shared.pulse;
    }
    if (entries.size() > (size_t)kMaxRows) entries.resize(kMaxRows);

    /* resolve avatars */
    {
        std::lock_guard<std::mutex> lk(g_shared.lock);
        for (size_t i = 0; i < entries.size(); ++i) {
            std::map<unsigned int, std::wstring>::const_iterator it =
                g_shared.avatars.find(entries[i].id);
            if (it != g_shared.avatars.end()) entries[i].avatarPath = it->second;
        }
    }

    int dpi = g_currentDpi;
    Dbg(L"[paint] entries=%zu", entries.size());
    Layout L = ComputeLayout((int)entries.size(), dpi, entries);

    if (entries.empty()) {
        Dbg(L"[paint] empty -> hide");
        /* nothing to show -> hide the window entirely */
        if (IsWindowVisible(hwnd)) ShowWindow(hwnd, SW_HIDE);
        g_width = g_height = 0;
        return;
    }
    bool wantVisible;
    bool onlyInGame;
    {
        std::lock_guard<std::mutex> lk(g_shared.lock);
        wantVisible = g_shared.visible;
        onlyInGame  = g_shared.onlyInGame;
    }
    /* "only show while a game is the foreground app" - the foreground window
     * can change at any time, so this is re-evaluated on every paint. */
    if (wantVisible && onlyInGame && !IsGameForeground()) {
        Dbg(L"[paint] only-in-game set, foreground is not a game -> hide");
        if (IsWindowVisible(hwnd)) ShowWindow(hwnd, SW_HIDE);
        g_width = g_height = 0;
        return;
    }
    if (wantVisible) {
        if (!IsWindowVisible(hwnd)) ShowWindow(hwnd, SW_SHOWNOACTIVATE);
    } else {
        if (IsWindowVisible(hwnd)) ShowWindow(hwnd, SW_HIDE);
    }

    /* resize if the layout changed */
    int newW = L.width, newH = L.height;
    Dbg(L"[paint] layout w=%d h=%d pos=%d,%d", newW, newH, g_x, g_y);
    if (newW != g_width || newH != g_height) {
        if (g_x < -9000) DefaultPosition(newW, newH);
        g_width  = newW;
        g_height = newH;
    }

    /* offscreen ARGB surface */
    Bitmap bmp(g_width, g_height, PixelFormat32bppPARGB);
    if (bmp.GetLastStatus() != Ok) return;
    Graphics g(&bmp);
    if (g.GetLastStatus() != Ok) return;
    g.SetSmoothingMode(SmoothingModeAntiAlias);
    g.SetTextRenderingHint(TextRenderingHintAntiAliasGridFit);
    g.SetPixelOffsetMode(PixelOffsetModeHalf);

    /* Panel frame only - the background is transparent on purpose.
     *
     * A single light stroke vanishes on bright game scenes, so the frame is
     * drawn twice: first a 3px black halo (readable on white), then a 1px
     * white line on top (readable on black). The result works on any
     * background without tinting what is behind the panel. */
    {
        GraphicsPath panel;
        AddRoundedRect(panel, RectF(1.5f, 1.5f, (float)g_width - 3.0f, (float)g_height - 3.0f),
                       (float)L.radius);
        Pen halo{Color(kPanelHalo), 3.0f};
        g.DrawPath(&halo, &panel);

        GraphicsPath panel2;
        AddRoundedRect(panel2, RectF(0.5f, 0.5f, (float)g_width - 1.0f, (float)g_height - 1.0f),
                       (float)L.radius);
        Pen border{Color(kBorder), 1.0f};
        g.DrawPath(&border, &panel2);
    }

    /* rows */
    bool pulseActive = false;
    for (size_t i = 0; i < entries.size(); ++i) {
        float rowTop = (float)(L.padY + (int)i * L.rowHeight);
        float avX    = (float)L.padX;
        float avY    = rowTop + (L.rowHeight - L.avatarSize) / 2.0f;
        RectF avBox(avX, avY, (float)L.avatarSize, (float)L.avatarSize);
        float radius = L.avatarSize / 2.0f;

        /* Silent members keep their row but are drawn dimmed; only the speaker
         * gets full brightness and the pulsing ring (KOOK behaviour). */
        const int alpha = entries[i].talking ? 255 : kDimAlpha;
        DrawAvatar(g, entries[i], avBox, alpha);

        if (entries[i].talking) {
            pulseActive = true;
            DrawRing(g, avBox, radius, pulse, kRingActive, std::max(2, Dip(2, dpi)));
        } else {
            /* faint static outline so the avatar still reads as a circle */
            DrawRing(g, avBox, radius, -1.0f, kRingIdle, std::max(1, Dip(1, dpi)), 70);
        }

        /* The name is NOT drawn here: it goes through Direct2D after this
         * surface has been blitted into the window DIB (see DrawNamesD2D),
         * because GDI+ cannot render emoji. Only when Direct2D is missing do
         * we fall back to GDI+ text below. */
        if (!g_dwFormat && g_fontName) {
            StringFormat sf;
            sf.SetTrimming(StringTrimmingEllipsisCharacter);
            sf.SetFormatFlags(StringFormatFlagsNoWrap);
            sf.SetLineAlignment(StringAlignmentCenter);
            RectF textBox(avX + L.avatarSize + L.gap, rowTop,
                          (float)L.nameWidth, (float)L.rowHeight);
            SolidBrush tb{Color(alpha, 0xFF, 0xFF, 0xFF)};
            g.DrawString(entries[i].name.c_str(), -1, g_fontName, textBox, &sf, &tb);
        }
    }

    /* pulse animation step */
    if (pulseActive) {
        std::lock_guard<std::mutex> lk(g_shared.lock);
        g_shared.pulse += 0.06f;
        if (g_shared.pulse > 1.0f) g_shared.pulse = 0.0f;
    }

    /* present via UpdateLayeredWindow */
    HDC screenDc = GetDC(NULL);
    HDC memDc    = CreateCompatibleDC(screenDc);
    BITMAPINFO bi;
    ZeroMemory(&bi, sizeof(bi));
    bi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth       = g_width;
    bi.bmiHeader.biHeight      = -g_height;   /* top-down */
    bi.bmiHeader.biPlanes      = 1;
    bi.bmiHeader.biBitCount    = 32;
    bi.bmiHeader.biCompression = BI_RGB;

    void* bits = NULL;
    HBITMAP dib = CreateDIBSection(screenDc, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
    Dbg(L"[paint] memDc=%p dib=%p bits=%p", (void*)memDc, (void*)dib, bits);
    if (dib && bits) {
        HGDIOBJ old = SelectObject(memDc, dib);
        /* ask GDI+ to render straight into the DIB bits */
        {
            Bitmap target(g_width, g_height, g_width * 4, PixelFormat32bppPARGB, (BYTE*)bits);
            Graphics tg(&target);
            if (tg.GetLastStatus() == Ok) {
                tg.SetCompositingMode(CompositingModeSourceCopy);
                tg.DrawImage(&bmp, 0, 0);
            }
        }

        /* Second pass: the names, rendered by Direct2D on top of the GDI+
         * panel. This must happen while the DIB is still selected into memDc,
         * which is also what UpdateLayeredWindow needs. */
        if (g_dwFormat) {
            DrawNamesD2D(memDc, g_width, g_height, entries, L);
            DrawInitialsD2D(memDc, g_width, g_height, entries, L);
        }

        POINT dst = { g_x, g_y };
        SIZE  sz  = { g_width, g_height };
        POINT src = { 0, 0 };
        BLENDFUNCTION bf;
        bf.BlendOp             = AC_SRC_OVER;
        bf.BlendFlags          = 0;
        bf.SourceConstantAlpha = 255;
        bf.AlphaFormat         = AC_SRC_ALPHA;
        /* The DIB must still be selected into memDc here: UpdateLayeredWindow
         * reads the surface through that DC (a deselected DC falls back to its
         * 1x1 monochrome stock bitmap and the call fails with ERROR_GEN_FAILURE
         * 31). Deselect it only afterwards. */
        BOOL ulw = UpdateLayeredWindow(hwnd, screenDc, &dst, &sz, memDc, &src, 0, &bf, ULW_ALPHA);
        Dbg(L"[paint] ULW=%d err=%lu dst=%d,%d size=%dx%d", ulw ? 1 : 0, GetLastError(), g_x, g_y, g_width, g_height);
        SelectObject(memDc, old);
    }
    if (dib) DeleteObject(dib);
    if (memDc) DeleteDC(memDc);
    if (screenDc) ReleaseDC(NULL, screenDc);
}

/* ------------------------------------------------------------------ */
/* window procedure                                                    */
/* ------------------------------------------------------------------ */
LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg) {
    case WM_NCHITTEST: {
        /* Click-through is done with WS_EX_TRANSPARENT (see OverlayToggleLock),
         * not HTTRANSPARENT: the latter only forwards to windows of the *same*
         * thread, which would never reach the game underneath. While unlocked
         * the window is a normal client area so the manual drag below runs. */
        bool locked;
        {
            std::lock_guard<std::mutex> lk(g_shared.lock);
            locked = g_shared.locked;
        }
        return locked ? HTTRANSPARENT : HTCLIENT;
    }
    case WM_LBUTTONDOWN: {
        {
            std::lock_guard<std::mutex> lk(g_shared.lock);
            if (g_shared.locked) return 0;
        }
        POINT pt;
        GetCursorPos(&pt);
        g_dragging   = true;
        g_dragOrigin = pt;
        g_dragStartX = g_x;
        g_dragStartY = g_y;
        SetCapture(hwnd);
        return 0;
    }
    case WM_MOUSEMOVE: {
        if (g_dragging) {
            POINT pt;
            GetCursorPos(&pt);
            g_x = g_dragStartX + (pt.x - g_dragOrigin.x);
            g_y = g_dragStartY + (pt.y - g_dragOrigin.y);
            SetWindowPos(hwnd, HWND_TOPMOST, g_x, g_y, 0, 0,
                         SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
        }
        return 0;
    }
    case WM_LBUTTONUP: {
        if (g_dragging) {
            g_dragging = false;
            ReleaseCapture();
            /* magnetic snap: hugging a screen edge is what users expect from a
             * small always-on-top panel, and it keeps it out of the way. */
            int sx = g_x, sy = g_y;
            SnapToEdges(sx, sy, g_width > 0 ? g_width : 200, g_height > 0 ? g_height : 40);
            if (sx != g_x || sy != g_y) {
                g_x = sx;
                g_y = sy;
                SetWindowPos(hwnd, HWND_TOPMOST, g_x, g_y, 0, 0,
                             SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
                Paint(hwnd);
            }
            SavePosition();
        }
        return 0;
    }
    case WM_TIMER: {
        if (wParam == kTimerRedraw) {
            KillTimer(hwnd, kTimerRedraw);
            /* Always repaint: Paint() itself hides the window when the entry
             * list is empty, so a paint must happen for the hide to be seen.
             * While somebody talks the timer runs at animation speed; with
             * only idle members it slows down to kIdleMs, which still
             * re-evaluates the foreground gate often enough to notice an
             * alt-tab into a game without burning CPU. */
            bool anyTalking = false;
            bool anyEntries = false;
            {
                std::lock_guard<std::mutex> lk(g_shared.lock);
                anyEntries = !g_shared.entries.empty();
                for (size_t i = 0; i < g_shared.entries.size(); ++i) {
                    if (g_shared.entries[i].talking) { anyTalking = true; break; }
                }
            }
            Paint(hwnd);
            /* Keep ticking as long as the channel has members: the timer is
             * also what re-evaluates the "only in game" gate, so stopping it
             * while the overlay is hidden for that reason would leave it
             * hidden forever. Full speed while someone talks (pulse
             * animation), slow tick otherwise. */
            if (anyTalking)      SetTimer(hwnd, kTimerRedraw, kRedrawMs, NULL);
            else if (anyEntries) SetTimer(hwnd, kTimerRedraw, kIdleMs, NULL);
        }
        return 0;
    }
    case WM_DISPLAYCHANGE:
    case WM_SETTINGCHANGE:
        Paint(hwnd);
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

/* ------------------------------------------------------------------ */
/* overlay thread                                                      */
/* ------------------------------------------------------------------ */
DWORD WINAPI OverlayThread(LPVOID)
{
    g_threadId = GetCurrentThreadId();
    Dbg(L"[ovl] thread start");

    /* GDI+ */
    GdiplusStartupInput gsi;
    g_gdiPlusUp = GdiplusStartup(&g_gdiplusTok, &gsi, NULL) == Ok;
    Dbg(L"[ovl] gdiplus=%d", g_gdiPlusUp ? 1 : 0);

    /* DPI: per-monitor v2 where available so the overlay stays sharp */
    {
        typedef BOOL (WINAPI *SetCtxFn)(HANDLE);
        HMODULE user32 = GetModuleHandleW(L"user32.dll");
        if (user32) {
            /* FARPROC -> the real prototype needs a cast through void* */
            SetCtxFn setCtx = (SetCtxFn)(void*)GetProcAddress(user32, "SetProcessDpiAwarenessContext");
            if (setCtx) setCtx((HANDLE)-4); /* PER_MONITOR_AWARE_V2 */
        }
    }

    if (!g_gdiPlusUp) { Dbg(L"[ovl] no gdiplus, abort"); return 1; }

    /* fonts are created inside the GDI+ lifetime */
    {
        std::lock_guard<std::mutex> lk(g_gdiLock);
        g_fontName   = new Font(L"Microsoft YaHei UI", (REAL)Dip(kFontSizePx, g_currentDpi),
                                FontStyleRegular, UnitPixel);
        g_fontStatus = new Font(L"Microsoft YaHei UI", (REAL)Dip(12, g_currentDpi),
                                FontStyleRegular, UnitPixel);
    }

    /* Direct2D/DirectWrite for the names (emoji fallback). Failure is not
     * fatal: Paint() then falls back to GDI+ text, which cannot do emoji. */
    InitTextEngine(g_currentDpi);

    WNDCLASSEXW wc;
    ZeroMemory(&wc, sizeof(wc));
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = g_inst;
    wc.lpszClassName = L"TS3SpeakingOverlayWnd";
    if (!RegisterClassExW(&wc)) { Dbg(L"[ovl] RegisterClass failed err=%lu", GetLastError()); return 1; }
    Dbg(L"[ovl] class registered");

    g_hwnd = CreateWindowExW(
        WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
        L"TS3SpeakingOverlayWnd", L"TS3 Speaking Overlay",
        WS_POPUP,
        0, 0, 10, 10,
        NULL, NULL, g_inst, NULL);
    if (!g_hwnd) { Dbg(L"[ovl] CreateWindow failed err=%lu", GetLastError()); return 1; }
    Dbg(L"[ovl] window=%p", (void*)g_hwnd);

    LoadPosition();
    if (g_x < -9000) DefaultPosition(g_width, 120);

    /* a persisted locked state must also set the click-through ex-style */
    if (g_shared.locked) {
        LONG_PTR ex = GetWindowLongPtrW(g_hwnd, GWL_EXSTYLE);
        SetWindowLongPtrW(g_hwnd, GWL_EXSTYLE, ex | WS_EX_TRANSPARENT);
    }

    SetTimer(g_hwnd, kTimerRedraw, kRedrawMs, NULL);
    Paint(g_hwnd);

    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    /* teardown: GDI+ objects must die before GdiplusShutdown */
    if (g_hwnd) {
        KillTimer(g_hwnd, kTimerRedraw);
        DestroyWindow(g_hwnd);
        g_hwnd = NULL;
    }
    TeardownTextEngine();          /* COM objects first - nothing else uses them */
    {
        std::lock_guard<std::mutex> lk(g_gdiLock);
        delete g_fontName;   g_fontName   = NULL;
        delete g_fontStatus; g_fontStatus = NULL;
    }
    if (g_gdiPlusUp) {
        GdiplusShutdown(g_gdiplusTok);
        g_gdiPlusUp = false;
    }
    return 0;
}

} // anonymous namespace

/* ------------------------------------------------------------------ */
/* public API                                                          */
/* ------------------------------------------------------------------ */
bool OverlayStart(HINSTANCE hInst, const std::wstring& iniPath)
{
    if (g_thread) return true;
    g_inst    = hInst;
    g_iniPath = iniPath;
    g_thread  = CreateThread(NULL, 0, OverlayThread, NULL, 0, NULL);
    return g_thread != NULL;
}

void OverlayStop()
{
    if (!g_thread) return;
    if (g_threadId) PostThreadMessageW(g_threadId, WM_QUIT, 0, 0);
    WaitForSingleObject(g_thread, 3000);
    CloseHandle(g_thread);
    g_thread = NULL;
}

void OverlaySetEntries(const std::vector<Entry>& entries)
{
    {
        std::lock_guard<std::mutex> lk(g_shared.lock);
        g_shared.entries = entries;
    }
    if (g_hwnd) {
        /* wake the paint timer immediately */
        KillTimer(g_hwnd, kTimerRedraw);
        SetTimer(g_hwnd, kTimerRedraw, 1, NULL);
    }
}

void OverlaySetAvatar(unsigned int id, const std::wstring& path)
{
    std::lock_guard<std::mutex> lk(g_shared.lock);
    g_shared.avatars[id] = path;
}

void OverlayToggleVisible()
{
    std::lock_guard<std::mutex> lk(g_shared.lock);
    g_shared.visible = !g_shared.visible;
}

void OverlayToggleLock()
{
    bool locked;
    {
        std::lock_guard<std::mutex> lk(g_shared.lock);
        g_shared.locked = !g_shared.locked;
        locked = g_shared.locked;
    }
    /* WS_EX_TRANSPARENT makes the whole window click-through for every other
     * process. HTTRANSPARENT alone cannot do that across thread boundaries. */
    if (g_hwnd) {
        LONG_PTR ex = GetWindowLongPtrW(g_hwnd, GWL_EXSTYLE);
        if (locked) ex |= WS_EX_TRANSPARENT;
        else        ex &= ~WS_EX_TRANSPARENT;
        SetWindowLongPtrW(g_hwnd, GWL_EXSTYLE, ex);
    }
    IniSet(L"locked", locked ? 1 : 0);
}

void OverlayResetPosition()
{
    DefaultPosition(g_width > 0 ? g_width : 200, g_height > 0 ? g_height : 40);
    SavePosition();
    if (g_hwnd)
        SetWindowPos(g_hwnd, HWND_TOPMOST, g_x, g_y, 0, 0,
                     SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
}

void OverlayToggleOnlyInGame()
{
    bool onlyInGame;
    {
        std::lock_guard<std::mutex> lk(g_shared.lock);
        g_shared.onlyInGame = !g_shared.onlyInGame;
        onlyInGame = g_shared.onlyInGame;
    }
    IniSet(L"onlyInGame", onlyInGame ? 1 : 0);
    if (g_hwnd) {
        KillTimer(g_hwnd, kTimerRedraw);
        SetTimer(g_hwnd, kTimerRedraw, 1, NULL);
    }
}

void OverlaySaveSettings()
{
    /* Called from the settings window, i.e. NOT from the overlay thread, so the
     * switches have to be read under the lock instead of using SavePosition(). */
    bool locked, onlyInGame;
    {
        std::lock_guard<std::mutex> lk(g_shared.lock);
        locked     = g_shared.locked;
        onlyInGame = g_shared.onlyInGame;
    }
    IniSet(L"locked", locked ? 1 : 0);
    IniSet(L"onlyInGame", onlyInGame ? 1 : 0);
}

bool OverlayHiddenByForeground()
{
    bool onlyInGame;
    {
        std::lock_guard<std::mutex> lk(g_shared.lock);
        onlyInGame = g_shared.onlyInGame;
    }
    return onlyInGame && !IsGameForeground();
}

bool OverlayVisibleEnabled()
{
    std::lock_guard<std::mutex> lk(g_shared.lock);
    return g_shared.visible;
}

bool OverlayIsLocked()
{
    std::lock_guard<std::mutex> lk(g_shared.lock);
    return g_shared.locked;
}

bool OverlayOnlyInGameEnabled()
{
    std::lock_guard<std::mutex> lk(g_shared.lock);
    return g_shared.onlyInGame;
}

bool OverlayIsShowing()
{
    return g_hwnd != NULL && IsWindowVisible(g_hwnd);
}

bool OverlayGameForegroundNow()
{
    return IsGameForeground();
}

bool OverlayClassifyWindow(unsigned long long hwnd)
{
    return IsGameWindow((HWND)(uintptr_t)hwnd);
}

} // namespace ts3ovl





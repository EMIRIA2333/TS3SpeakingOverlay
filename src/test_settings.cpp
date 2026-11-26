/* Local harness for the settings window.
 *
 * It links the real sources (settings.cpp / overlay.cpp / games.cpp /
 * update.cpp) and opens the very same window TeamSpeak shows through
 * ts3plugin_configure, on a worker thread, because that is exactly how the
 * client calls it (ts3plugin_offersConfigure returns
 * PLUGIN_OFFERS_CONFIGURE_NEW_THREAD).
 *
 * usage: test_settings.exe [seconds] [iniPath]
 *        seconds  how long the window stays open (default 25)
 *        iniPath  the overlay.ini to use (default: test_settings.ini next to
 *                 the exe) - the game list lives in its [games] section
 *
 * The harness prints the window geometry and the live switch states so a
 * script can assert on them, then closes the window. */
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
#include <cstdio>
#include <string>

#include "games.h"
#include "overlay.h"
#include "settings.h"

static DWORD       g_lifeMs = 25000;
static std::wstring g_iniPath;

static DWORD WINAPI Worker(LPVOID)
{
    printf("worker: opening settings window\n");
    fflush(stdout);
    ts3ovl::SettingsShow(NULL);   /* modal: returns when the window closes */
    printf("worker: settings window closed\n");
    fflush(stdout);
    return 0;
}

/* FindWindowW() does not locate this class reliably (the window belongs to a
 * thread that never pumps DefWindowProc for WM_NULL), so enumerate instead. */
static HWND FindSettingsWindow()
{
    struct Ctx { HWND found; } ctx = { NULL };

    EnumWindows([](HWND h, LPARAM p) -> BOOL {
        wchar_t cls[128] = {0};
        if (GetClassNameW(h, cls, 128) == 0) return TRUE;
        if (wcscmp(cls, L"TS3SpeakingOverlaySettingsWnd") != 0) return TRUE;
        ((Ctx*)p)->found = h;
        return FALSE;
    }, (LPARAM)&ctx);

    return ctx.found;
}

int main(int argc, char** argv)
{
    if (argc > 1) {
        int secs = 0;
        if (sscanf(argv[1], "%d", &secs) == 1 && secs > 0) g_lifeMs = (DWORD)secs * 1000;
    }

    if (argc > 2) {
        const int n = MultiByteToWideChar(CP_UTF8, 0, argv[2], -1, NULL, 0);
        if (n > 1) {
            g_iniPath.assign((size_t)n - 1, L'\0');
            MultiByteToWideChar(CP_UTF8, 0, argv[2], -1, &g_iniPath[0], n);
        }
    }
    if (g_iniPath.empty()) {
        wchar_t exe[MAX_PATH] = {0};
        GetModuleFileNameW(NULL, exe, MAX_PATH);
        wchar_t* slash = wcsrchr(exe, L'\\');
        if (slash) *slash = L'\0';
        g_iniPath = std::wstring(exe) + L"\\test_settings.ini";
    }

    /* OverlayStart() does two things this harness needs: it stores the ini path
     * (without it OverlaySaveSettings() writes nothing, so every appearance and
     * switch change silently vanished) and it creates the real floating window,
     * which makes the live preview of a colour/font change observable. */
    if (!ts3ovl::OverlayStart(GetModuleHandleW(NULL), g_iniPath)) {
        printf("OverlayStart FAILED\n");
        fflush(stdout);
        return 1;
    }
    printf("ini     : %ls\n", g_iniPath.c_str());
    printf("games   : %u entries, autoDetect=%d, running=%ls\n",
           (unsigned)ts3ovl::GamesList().size(),
           (int)ts3ovl::GamesAutoDetect(),
           ts3ovl::GamesRunningName().c_str());
    fflush(stdout);

    HANDLE th = CreateThread(NULL, 0, Worker, NULL, 0, NULL);
    if (!th) { printf("CreateThread failed err=%lu\n", GetLastError()); return 1; }

    Sleep(1500);
    HWND w = FindSettingsWindow();
    if (!w) {
        printf("settings window NOT FOUND\n");
        fflush(stdout);
        return 2;
    }

    RECT rc;
    GetWindowRect(w, &rc);
    printf("window  : hwnd=%p vis=%d rect=%ld,%ld,%ld,%ld\n",
           (void*)w, (int)IsWindowVisible(w), rc.left, rc.top, rc.right, rc.bottom);
    printf("switches: visible=%d onlyInGame=%d locked=%d\n",
           (int)ts3ovl::OverlayVisibleEnabled(),
           (int)ts3ovl::OverlayOnlyInGameEnabled(),
           (int)ts3ovl::OverlayIsLocked());
    {
        ts3ovl::Appearance a;
        ts3ovl::OverlayGetAppearance(&a);
        printf("look    : bgColor=0x%08X textColor=0x%08X fontSize=%d\n",
               a.bgColor, a.textColor, a.fontSize);
    }
    fflush(stdout);

    Sleep(g_lifeMs);

    printf("closing window\n");
    fflush(stdout);
    PostMessageW(w, WM_CLOSE, 0, 0);
    WaitForSingleObject(th, 5000);
    CloseHandle(th);
    ts3ovl::OverlayStop();
    printf("done\n");
    return 0;
}

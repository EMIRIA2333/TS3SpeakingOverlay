/* Local harness for the settings window: links the real sources and opens the
 * window on a worker thread (exactly how TS3 calls ts3plugin_configure when
 * ts3plugin_offersConfigure returns PLUGIN_OFFERS_CONFIGURE_NEW_THREAD).
 *
 * usage: test_settings.exe [seconds]   (default 25)
 *
 * The window is modal, so the worker thread blocks inside SettingsShow() until
 * the window is closed; main() just reports the window geometry and closes it
 * after the given time so the caller can take a screenshot in between. */
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

#include "overlay.h"
#include "settings.h"

static DWORD g_lifeMs = 25000;

static DWORD WINAPI Worker(LPVOID)
{
    printf("worker: opening settings window\n");
    fflush(stdout);
    ts3ovl::SettingsShow(NULL);   /* modal: returns when the window is closed */
    printf("worker: settings window closed\n");
    fflush(stdout);
    return 0;
}

int main(int argc, char** argv)
{
    if (argc > 1) {
        int secs = 0;
        if (sscanf(argv[1], "%d", &secs) == 1 && secs > 0) g_lifeMs = (DWORD)secs * 1000;
    }

    HANDLE th = CreateThread(NULL, 0, Worker, NULL, 0, NULL);
    if (!th) { printf("CreateThread failed err=%lu\n", GetLastError()); return 1; }

    Sleep(1200);
    HWND w = FindWindowW(L"TS3SpeakingOverlaySettingsWnd", NULL);
    if (!w) {
        printf("settings window NOT FOUND\n");
        fflush(stdout);
        return 2;
    }
    RECT rc;
    GetWindowRect(w, &rc);
    printf("settings window hwnd=%p vis=%d rect=%ld,%ld,%ld,%ld\n",
           (void*)w, (int)IsWindowVisible(w), rc.left, rc.top, rc.right, rc.bottom);
    printf("switches: visible=%d onlyInGame=%d locked=%d\n",
           (int)ts3ovl::OverlayVisibleEnabled(),
           (int)ts3ovl::OverlayOnlyInGameEnabled(),
           (int)ts3ovl::OverlayIsLocked());
    fflush(stdout);

    Sleep(g_lifeMs);

    printf("closing window\n");
    fflush(stdout);
    PostMessageW(w, WM_CLOSE, 0, 0);
    WaitForSingleObject(th, 5000);
    CloseHandle(th);
    printf("done\n");
    return 0;
}

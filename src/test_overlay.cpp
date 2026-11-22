/*
 * test_overlay.cpp - standalone harness for the overlay renderer.
 * Builds an EXE that shows the overlay with fake speakers, so the visual
 * layer can be validated without a running TeamSpeak client.
 *
 * usage: test_overlay.exe [seconds]
 */
#include <windows.h>
#include <shellapi.h>
#include <string>
#include <vector>

#include "overlay.h"

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE hPrev, LPWSTR cmdLine, int nShow)
{
    (void)hPrev; (void)cmdLine; (void)nShow;

    int seconds = 20;
    {
        int argc = 0;
        LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
        if (argv) {
            if (argc > 1) seconds = _wtoi(argv[1]);
            LocalFree(argv);
        }
    }

    /* Keep the harness state next to the EXE so nothing is hard-coded and the
     * production overlay.ini in the TS3 folder is never touched. */
    wchar_t exePath[MAX_PATH] = {0};
    GetModuleFileNameW(NULL, exePath, MAX_PATH);
    std::wstring iniPath(exePath);
    size_t slash = iniPath.find_last_of(L"\\/");
    if (slash != std::wstring::npos) iniPath.erase(slash + 1);
    iniPath += L"test_overlay.ini";

    if (!ts3ovl::OverlayStart(hInst, iniPath)) {
        MessageBoxW(NULL, L"OverlayStart failed", L"test", MB_ICONERROR);
        return 1;
    }

    /* The production default is "only while a game is in the foreground",
     * which would hide this harness on a desktop. Turn it off so the renderer
     * can be validated standalone. Pass "ingame" as second argument to keep
     * the game gating enabled and check that it hides on the desktop. */
    bool keepGating = false;
    {
        int argc = 0;
        LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
        if (argv) {
            for (int i = 2; i < argc; ++i) {
                if (!_wcsicmp(argv[i], L"ingame")) keepGating = true;
            }
            LocalFree(argv);
        }
    }
    if (!keepGating) {
        if (ts3ovl::OverlayHiddenByForeground()) ts3ovl::OverlayToggleOnlyInGame();
    }

    std::vector<ts3ovl::Entry> entries;
    /* Names deliberately mix CJK, latin and emoji: emoji used to render as
     * empty boxes because GDI+ has no font fallback (now drawn via Direct2D).
     * Only entries 0 and 2 are "talking"; the others must show up dimmed -
     * that is the whole point of this harness variant. */
    const wchar_t* names[] = {
        L"\U0001F602 \U0001F3AE\u5C0F\u660E",
        L"Alice",
        L"队长老王 \U0001F3AE",
        L"David \u2764\uFE0F"
    };
    const bool talking[] = { true, false, true, false };
    for (int i = 0; i < 4; ++i) {
        ts3ovl::Entry e;
        e.id      = (unsigned int)(i + 1);
        e.name    = names[i];
        e.talking = talking[i];
        entries.push_back(e);
    }
    ts3ovl::OverlaySetEntries(entries);

    DWORD end = GetTickCount() + (DWORD)seconds * 1000;
    while (GetTickCount() < end) {
        Sleep(100);
    }

    ts3ovl::OverlayStop();
    return 0;
}

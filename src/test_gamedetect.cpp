/*
 * test_gamedetect.cpp - shows what the new game detection decides.
 *
 * Since 1.2.0 the overlay no longer looks at the foreground window at all: it
 * shows up while any *enabled* entry of the game list is running. This probe
 * therefore prints the list, which of those processes are alive right now, and
 * the verdict the overlay itself would reach (OverlayGameForegroundNow() is a
 * thin wrapper around GamesAnyRunning(), so probe and overlay cannot drift).
 *
 * usage: test_gamedetect.exe [seconds] [iniPath]
 *        seconds  keep polling (default: print once)
 *        iniPath  the overlay.ini holding [games] (default: alongside the exe)
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
#include <tlhelp32.h>
#include <cwctype>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "games.h"
#include "overlay.h"

static std::string Narrow(const std::wstring& w)
{
    if (w.empty()) return std::string();
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), NULL, 0, NULL, NULL);
    std::string s((size_t)n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &s[0], n, NULL, NULL);
    return s;
}

static std::wstring DefaultIni()
{
    wchar_t exe[MAX_PATH] = {0};
    GetModuleFileNameW(NULL, exe, MAX_PATH);
    std::wstring p(exe);
    const size_t slash = p.find_last_of(L'\\');
    if (slash != std::wstring::npos) p.resize(slash);
    return p + L"\\test_gamedetect.ini";
}

/* Full process snapshot (lower-case exe names), so the probe can say which list
 * entries are really alive instead of guessing from visible windows. */
static void SnapshotNames(std::vector<std::wstring>* out)
{
    out->clear();
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return;
    PROCESSENTRY32W pe;
    ZeroMemory(&pe, sizeof(pe));
    pe.dwSize = sizeof(pe);
    if (Process32FirstW(snap, &pe)) {
        do {
            if (!pe.szExeFile[0]) continue;
            std::wstring n(pe.szExeFile);
            for (size_t i = 0; i < n.size(); ++i) n[i] = (wchar_t)towlower(n[i]);
            out->push_back(n);
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
}

static void Dump()
{
    const std::vector<ts3ovl::GameEntry> games = ts3ovl::GamesList();
    std::vector<std::wstring> procs;
    SnapshotNames(&procs);

    printf("entries=%u  autoDetect=%d\n", (unsigned)games.size(),
           (int)ts3ovl::GamesAutoDetect());
    for (size_t i = 0; i < games.size(); ++i) {
        bool alive = false;
        for (size_t r = 0; r < procs.size(); ++r) {
            if (procs[r] == games[i].exe) { alive = true; break; }
        }
        printf("  %-28s enabled=%d auto=%d %s\n",
               Narrow(games[i].exe).c_str(), (int)games[i].enabled,
               (int)games[i].autoAdded, alive ? (games[i].enabled ? "<-- RUNNING"
                                                                  : "<-- running but disabled")
                                              : "");
    }
    printf("running game    : %s\n", Narrow(ts3ovl::GamesRunningName()).c_str());
    printf("VERDICT         : %s\n",
           ts3ovl::OverlayGameForegroundNow() ? "show overlay (a listed game is running)"
                                              : "hide overlay (no listed game is running)");
    printf("current visible : %d  hiddenByForeground=%d\n",
           (int)ts3ovl::OverlayIsShowing(), (int)ts3ovl::OverlayHiddenByForeground());
    fflush(stdout);
}

int main(int argc, char** argv)
{
    int seconds = 0;
    std::wstring ini = DefaultIni();

    if (argc > 1) seconds = atoi(argv[1]);
    if (argc > 2) {
        const int n = MultiByteToWideChar(CP_UTF8, 0, argv[2], -1, NULL, 0);
        if (n > 1) {
            ini.assign((size_t)n - 1, L'\0');
            MultiByteToWideChar(CP_UTF8, 0, argv[2], -1, &ini[0], n);
        }
    }

    ts3ovl::GamesInit(ini);
    printf("ini             : %s\n", Narrow(ini).c_str());
    fflush(stdout);

    if (seconds <= 0) { Dump(); return 0; }

    for (int t = 0; t < seconds; ++t) {
        printf("--- t=%d ---\n", t);
        Dump();
        Sleep(1000);
    }
    return 0;
}

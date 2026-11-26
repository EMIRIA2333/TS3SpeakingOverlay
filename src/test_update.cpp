/* Console harness for update.cpp and games.cpp - no UI, so every failure is a
 * plain printf instead of a message box. */
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
#include <vector>

#include "games.h"
#include "update.h"

static std::string Narrow(const std::wstring& w)
{
    if (w.empty()) return std::string();
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), NULL, 0, NULL, NULL);
    std::string s((size_t)n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &s[0], n, NULL, NULL);
    return s;
}

int main(int argc, char** argv)
{
    const std::string mode = (argc > 1) ? argv[1] : "all";

    printf("current version : %s\n", Narrow(ts3ovl::UpdateCurrentVersion()).c_str());
    fflush(stdout);

    if (mode == "ver" || mode == "all") {
        struct Pair { const wchar_t* a; const wchar_t* b; };
        const Pair cases[] = {
            { L"1.1.1",       L"1.2.0-beta1" },
            { L"1.2.0-beta1", L"1.2.0-beta1" },
            { L"1.2.0-beta1", L"1.2.0-beta2" },
            { L"1.2.0",       L"1.2.0-beta2" },
            { L"1.2.0-beta2", L"1.2.0"       },
            { L"1.10.0",      L"1.9.9"       },
            { L"v1.1.1",      L"1.1.10"      },
        };
        for (const Pair& p : cases) {
            printf("newer(%-12ls > %-12ls) = %d\n",
                   p.a, p.b, (int)ts3ovl::UpdateIsNewer(p.a, p.b));
        }
        fflush(stdout);
    }

    if (mode == "games" || mode == "all") {
        wchar_t exe[MAX_PATH] = {0};
        GetModuleFileNameW(NULL, exe, MAX_PATH);
        std::wstring ini(exe);
        const size_t slash = ini.find_last_of(L'\\');
        if (slash != std::wstring::npos) ini.resize(slash);
        ini += L"\\test_update.ini";

        ts3ovl::GamesInit(ini);
        printf("games ini       : %s\n", Narrow(ini).c_str());
        printf("entries         : %u\n", (unsigned)ts3ovl::GamesList().size());
        for (const ts3ovl::GameEntry& g : ts3ovl::GamesList()) {
            printf("  %-24s enabled=%d auto=%d\n",
                   Narrow(g.exe).c_str(), (int)g.enabled, (int)g.autoAdded);
        }
        printf("autoDetect      : %d\n", (int)ts3ovl::GamesAutoDetect());
        printf("running game    : %s\n", Narrow(ts3ovl::GamesRunningName()).c_str());
        printf("any running     : %d\n", (int)ts3ovl::GamesAnyRunning());

        const std::vector<std::wstring> vis = ts3ovl::GamesVisibleWindowExes();
        printf("visible exes    : %u\n", (unsigned)vis.size());
        for (size_t i = 0; i < vis.size() && i < 40; ++i) {
            printf("  %s%s\n", Narrow(vis[i]).c_str(),
                   ts3ovl::GamesContains(vis[i]) ? "   [in list]" : "");
        }
        fflush(stdout);
    }

    if (mode == "scan") {
        wchar_t exe[MAX_PATH] = {0};
        GetModuleFileNameW(NULL, exe, MAX_PATH);
        std::wstring ini(exe);
        const size_t slash = ini.find_last_of(L'\\');
        if (slash != std::wstring::npos) ini.resize(slash);
        ini += L"\\test_update.ini";
        if (argc > 2 && argv[2][0]) {
            const int n = MultiByteToWideChar(CP_UTF8, 0, argv[2], -1, NULL, 0);
            if (n > 1) {
                ini.assign((size_t)n - 1, L'\0');
                MultiByteToWideChar(CP_UTF8, 0, argv[2], -1, &ini[0], n);
            }
        }
        ts3ovl::GamesInit(ini);

        const ULONGLONG t0 = GetTickCount64();
        std::vector<std::wstring> added = ts3ovl::GamesScanForNew();
        const ULONGLONG t1 = GetTickCount64();
        const ULONGLONG t2 = GetTickCount64();
        const std::vector<std::wstring> sc = ts3ovl::GamesScanShortcuts();
        const ULONGLONG t3 = GetTickCount64();
        printf("scan took       : %llu ms\n", (unsigned long long)(t1 - t0));
        printf("shortcut scan   : %llu ms, %u new\n", (unsigned long long)(t3 - t2), (unsigned)sc.size());
        for (const std::wstring& a : sc) {
            bool dup = false;
            for (const std::wstring& b : added) if (a == b) { dup = true; break; }
            if (!dup) added.push_back(a);
        }
        printf("newly added     : %u\n", (unsigned)added.size());
        for (const std::wstring& a : added) printf("  + %s\n", Narrow(a).c_str());
        printf("total entries   : %u\n", (unsigned)ts3ovl::GamesList().size());
        for (const ts3ovl::GameEntry& g : ts3ovl::GamesList()) {
            printf("  %-28s enabled=%d auto=%d\n",
                   Narrow(g.exe).c_str(), (int)g.enabled, (int)g.autoAdded);
        }
        fflush(stdout);
    }

    if (mode == "running") {
        wchar_t exe[MAX_PATH] = {0};
        GetModuleFileNameW(NULL, exe, MAX_PATH);
        std::wstring ini(exe);
        const size_t slash = ini.find_last_of(L'\\');
        if (slash != std::wstring::npos) ini.resize(slash);
        ini += L"\\test_running.ini";
        DeleteFileW(ini.c_str());
        ts3ovl::GamesInit(ini);

        printf("empty list      : anyRunning=%d name=%s\n",
               (int)ts3ovl::GamesAnyRunning(), Narrow(ts3ovl::GamesRunningName()).c_str());
        fflush(stdout);

        /* a process that is certainly running: this harness itself */
        wchar_t self[MAX_PATH] = {0};
        GetModuleFileNameW(NULL, self, MAX_PATH);
        std::wstring selfName(self);
        const size_t s = selfName.find_last_of(L'\\');
        if (s != std::wstring::npos) selfName = selfName.substr(s + 1);

        ts3ovl::GamesAdd(selfName);
        printf("added           : %s\n", Narrow(selfName).c_str());
        printf("after add       : anyRunning=%d name=%s\n",
               (int)ts3ovl::GamesAnyRunning(), Narrow(ts3ovl::GamesRunningName()).c_str());
        fflush(stdout);

        ts3ovl::GamesSetEnabled(selfName, false);
        printf("disabled        : anyRunning=%d name=%s\n",
               (int)ts3ovl::GamesAnyRunning(), Narrow(ts3ovl::GamesRunningName()).c_str());
        fflush(stdout);

        ts3ovl::GamesSetEnabled(selfName, true);
        /* the cache is 400 ms; wait it out so the re-enable is visible */
        Sleep(500);
        printf("re-enabled      : anyRunning=%d name=%s\n",
               (int)ts3ovl::GamesAnyRunning(), Narrow(ts3ovl::GamesRunningName()).c_str());
        fflush(stdout);

        /* a program that is definitely not running */
        ts3ovl::GamesAdd(L"definitely-not-running.exe");
        ts3ovl::GamesSetEnabled(selfName, false);
        Sleep(500);
        printf("only dead entry : anyRunning=%d name=%s\n",
               (int)ts3ovl::GamesAnyRunning(), Narrow(ts3ovl::GamesRunningName()).c_str());
        printf("listing         : %u entries\n", (unsigned)ts3ovl::GamesList().size());
        for (const ts3ovl::GameEntry& g : ts3ovl::GamesList()) {
            printf("  %-28s enabled=%d auto=%d\n",
                   Narrow(g.exe).c_str(), (int)g.enabled, (int)g.autoAdded);
        }
        fflush(stdout);
    }

    if (mode == "download") {
        printf("checking github ...\n");
        fflush(stdout);
        ts3ovl::UpdateResult r = ts3ovl::UpdateCheck();
        printf("ok=%d hasUpdate=%d latest=%s\n", (int)r.ok, (int)r.hasUpdate,
               Narrow(r.latestTag).c_str());
        if (!r.ok) { printf("check failed: %s\n", Narrow(r.error).c_str()); return 1; }

        wchar_t tmp[MAX_PATH] = {0};
        GetTempPathW(MAX_PATH, tmp);
        const std::wstring dst = std::wstring(tmp) + L"ts3ovl_update_test.exe";
        DeleteFileW(dst.c_str());

        const ULONGLONG t0 = GetTickCount64();
        std::wstring err;
        const bool ok = ts3ovl::UpdateDownload(r.downloadUrl, dst, &err);
        const ULONGLONG t1 = GetTickCount64();
        printf("download ok=%d took=%llu ms err=%s\n", (int)ok,
               (unsigned long long)(t1 - t0), Narrow(err).c_str());

        WIN32_FILE_ATTRIBUTE_DATA fad;
        if (GetFileAttributesExW(dst.c_str(), GetFileExInfoStandard, &fad)) {
            const ULONGLONG size = ((ULONGLONG)fad.nFileSizeHigh << 32) | fad.nFileSizeLow;
            printf("file            : %s (%llu bytes)\n", Narrow(dst).c_str(), (unsigned long long)size);
            /* first two bytes must be "MZ" */
            HANDLE f = CreateFileW(dst.c_str(), GENERIC_READ, FILE_SHARE_READ, NULL,
                                   OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
            if (f != INVALID_HANDLE_VALUE) {
                unsigned char mz[2] = {0};
                DWORD got = 0;
                ReadFile(f, mz, 2, &got, NULL);
                CloseHandle(f);
                printf("magic           : %c%c\n", got == 2 ? (char)mz[0] : '?',
                       got == 2 ? (char)mz[1] : '?');
            }
        } else {
            printf("file missing after download\n");
        }
        fflush(stdout);
    }

    if (mode == "check" || mode == "all") {
        printf("checking github ...\n");
        fflush(stdout);
        ts3ovl::UpdateResult r = ts3ovl::UpdateCheck();
        printf("ok=%d hasUpdate=%d err=%s\n", (int)r.ok, (int)r.hasUpdate, Narrow(r.error).c_str());
        printf("latestTag       : %s\n", Narrow(r.latestTag).c_str());
        printf("pageUrl         : %s\n", Narrow(r.pageUrl).c_str());
        printf("downloadUrl     : %s\n", Narrow(r.downloadUrl).c_str());
        printf("notes (first 200): %.200s\n", Narrow(r.notes).c_str());
        fflush(stdout);
    }

    return 0;
}

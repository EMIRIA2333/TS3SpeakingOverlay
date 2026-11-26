/* Diagnostic probe for the shortcut scanner.
 *
 * It #includes games.cpp so it can call the file-local helpers
 * (LooksLikeGameFolder / HasEngineMarker / HasEngineLayoutAndBulk ...) and show
 * WHY a given shortcut target is or is not registered as a game.
 *
 * Build (probe_scan.cpp must sit next to the staged games.cpp):
 *   copy tools\probe_scan.cpp %TEMP%\ts3ovl_build\src\
 *   g++ -std=c++17 -O2 -municode -static -o probe_scan.exe probe_scan.cpp \
 *       -lgdiplus -lgdi32 -luser32 -lshell32 -lole32 -luuid -lwinhttp -lcomctl32
 */
#include "games.cpp"

#include <cstdio>

static void DumpFolder(const wchar_t* label, const std::wstring& dir)
{
    ULONGLONG total = 0;
    const bool bulk = FolderAtLeast(dir, 2, &total);
    wprintf(L"    %-22s %s\n", label, dir.c_str());
    wprintf(L"      system folder  : %s\n", IsSystemFolder(Lower(dir)) ? L"yes" : L"no");
    wprintf(L"      engine marker  : %s\n", HasEngineMarker(dir) ? L"yes" : L"no");
    wprintf(L"      layout+bulk    : %s (dirs=%d size=%llu MB)\n",
            HasEngineLayoutAndBulk(dir) ? L"yes" : L"no",
            (DirExists(Join(dir, L"Binaries")) || DirExists(Join(dir, L"Engine")) ||
             DirExists(Join(dir, L"Win64")) || DirExists(Join(dir, L"Game"))) ? 1 : 0,
            (unsigned long long)(total / (1024ull * 1024ull)));
    wprintf(L"      electron shape : %s\n", HasElectronShape(dir) ? L"yes" : L"no");
    wprintf(L"      big archive    : %s\n", HasBigContentArchive(dir, 2) ? L"yes" : L"no");
    wprintf(L"      bulk(depth 2)  : %s\n", bulk ? L"yes" : L"no");
}

int wmain(int argc, wchar_t** argv)
{
    if (argc < 2) {
        wprintf(L"usage: probe_scan.exe <exe-or-folder> [...]   |   probe_scan.exe --shortcuts\n");
        return 2;
    }

    if (wcscmp(argv[1], L"--shortcuts") == 0) {
        std::vector<std::wstring> exes = ShortcutExes();
        wprintf(L"shortcut exes (%u):\n", (unsigned)exes.size());
        for (size_t i = 0; i < exes.size(); ++i) wprintf(L"  %s\n", exes[i].c_str());
        return 0;
    }

    for (int i = 1; i < argc; ++i) {
        std::wstring path = argv[i];
        wprintf(L"\n=== %s\n", path.c_str());
        if (!FileExists(path) && !DirExists(path)) { wprintf(L"    (missing)\n"); continue; }

        std::wstring dir = DirExists(path) ? path : DirName(path);
        DumpFolder(L"folder", dir);
        const std::wstring leaf = Lower(BaseName(dir));
        if (leaf == L"bin" || leaf == L"win64" || leaf == L"binaries" || leaf == L"win32" ||
            leaf == L"x64" || leaf == L"game") {
            DumpFolder(L"parent", DirName(dir));
        }
        wprintf(L"  -> LooksLikeGameFolder : %s\n", LooksLikeGameFolder(path) ? L"YES" : L"no");
        const std::wstring exe = Lower(BaseName(path));
        wprintf(L"  -> LooksLikeHelper     : %s\n", LooksLikeHelper(exe) ? L"yes" : L"no");
        wprintf(L"  -> LooksLikeNonGameApp : %s\n", LooksLikeNonGameApp(exe, exe) ? L"yes" : L"no");
        wprintf(L"  -> GamesIsIgnored      : %s\n", GamesIsIgnored(exe) ? L"yes" : L"no");
        wprintf(L"  -> GamesContains       : %s\n", GamesContains(exe) ? L"yes" : L"no");
    }
    return 0;
}

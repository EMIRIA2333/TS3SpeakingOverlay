/*
 * ts3-speaking-overlay - always-on-top "who is talking" overlay (KOOK style)
 * games.cpp : the game whitelist and the "is a game running" gate
 *
 * Storage: the same overlay.ini the renderer writes, section [games].
 *   cs2.exe=1|0      -> enabled | autoAdded
 *   autoDetect=1     -> scan for newly installed Steam games automatically
 *
 * The gate no longer looks at the foreground window: the overlay is shown while
 * any *enabled* executable of the list is running, whatever has focus.
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
#include <shlobj.h>
#include <shobjidl.h>
#include <tlhelp32.h>
#include <objbase.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <vector>

#include "games.h"

namespace ts3ovl {

namespace {

std::wstring        g_iniPath;
std::mutex          g_lock;
std::vector<GameEntry> g_games;

/* Executables the user removed from the list. Without this an auto scan keeps
 * re-adding them a minute later, which makes "从列表删除" look broken. Any
 * entry in here is skipped by GamesScanForNew() and dropped by GamesAdd(). */
std::set<std::wstring> g_ignored;

bool                g_autoDetect = true;

/* Cache for the per-frame gate: enumerating the process list on every repaint
 * (25 fps) is wasteful, half a second of staleness is invisible. */
bool         g_cacheValid = false;
bool         g_cacheAny   = false;
std::wstring g_cacheName;
DWORD        g_cacheAt    = 0;
const DWORD  kCacheMs     = 400;

const wchar_t* const kSection = L"games";
const wchar_t* const kAutoKey = L"autoDetect";
/* Serialised as one line, "ignored=a.exe|b.exe", so the [games] block stays a
 * simple list of key=value pairs that LoadLocked() can walk. */
const wchar_t* const kIgnoredKey = L"ignored";
std::wstring Lower(const std::wstring& s)
{
    std::wstring r;
    r.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        wchar_t c = s[i];
        if (c >= L'A' && c <= L'Z') c = (wchar_t)(c - L'A' + L'a');
        r.push_back(c);
    }
    return r;
}

std::wstring BaseName(const std::wstring& path)
{
    const size_t p = path.find_last_of(L"\\/");
    return (p == std::wstring::npos) ? path : path.substr(p + 1);
}

std::wstring DirName(const std::wstring& path)
{
    const size_t p = path.find_last_of(L"\\/");
    return (p == std::wstring::npos) ? std::wstring() : path.substr(0, p);
}

bool FileExists(const std::wstring& p)
{
    const DWORD a = GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

bool DirExists(const std::wstring& p)
{
    const DWORD a = GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

std::wstring Join(const std::wstring& a, const std::wstring& b)
{
    if (a.empty()) return b;
    std::wstring r = a;
    if (r[r.size() - 1] != L'\\' && r[r.size() - 1] != L'/') r += L'\\';
    return r + b;
}

/* ---------------------------------------------------------------- */
/* ini                                                              */
/* ---------------------------------------------------------------- */

std::vector<std::wstring> SplitLines(const std::wstring& buf)
{
    std::vector<std::wstring> out;
    size_t i = 0;
    while (i < buf.size()) {
        const size_t j = buf.find(L'\0', i);
        const std::wstring line = buf.substr(i, (j == std::wstring::npos ? buf.size() : j) - i);
        if (!line.empty()) out.push_back(line);
        if (j == std::wstring::npos) break;
        i = j + 1;
    }
    return out;
}

void LoadLocked()
{
    g_games.clear();
    g_ignored.clear();
    g_autoDetect = true;
    if (g_iniPath.empty()) return;

    std::vector<wchar_t> buf(32768, 0);
    const DWORD n = GetPrivateProfileSectionW(kSection, &buf[0], (DWORD)buf.size(), g_iniPath.c_str());
    if (n == 0) return;

    const std::wstring all(&buf[0], n);
    const std::vector<std::wstring> lines = SplitLines(all);
    for (size_t i = 0; i < lines.size(); ++i) {
        const std::wstring& line = lines[i];
        const size_t eq = line.find(L'=');
        if (eq == std::wstring::npos) continue;

        std::wstring key = line.substr(0, eq);
        std::wstring val = line.substr(eq + 1);
        if (Lower(key) == Lower(kAutoKey)) {
            g_autoDetect = (_wtoi(val.c_str()) != 0);
            continue;
        }
        if (Lower(key) == Lower(kIgnoredKey)) {
            size_t p = 0;
            while (p <= val.size()) {
                const size_t bar = val.find(L'|', p);
                const std::wstring one =
                    (bar == std::wstring::npos) ? val.substr(p) : val.substr(p, bar - p);
                const std::wstring lc = Lower(one);
                if (!lc.empty()) g_ignored.insert(lc);
                if (bar == std::wstring::npos) break;
                p = bar + 1;
            }
            continue;
        }
        if (key.empty()) continue;

        GameEntry e;
        e.exe = Lower(key);
        const size_t bar = val.find(L'|');
        e.enabled  = (_wtoi(val.c_str()) != 0);
        e.autoAdded = (bar != std::wstring::npos) && (_wtoi(val.c_str() + bar + 1) != 0);
        g_games.push_back(e);
    }
}

/* LoadLocked() is the only loader and runs with g_lock held; GamesInit() is the
 * single entry point that triggers it. Every public accessor below assumes that
 * has happened - overlay.cpp calls GamesInit() from OverlayStart(), i.e. during
 * ts3plugin_init, before any settings window can exist. */

/* ---------------------------------------------------------------- */
/* process helpers                                                  */
/* ---------------------------------------------------------------- */

bool SnapshotExes(std::set<std::wstring>* out)
{
    out->clear();
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return false;

    PROCESSENTRY32W pe;
    ZeroMemory(&pe, sizeof(pe));
    pe.dwSize = sizeof(pe);
    if (Process32FirstW(snap, &pe)) {
        do {
            if (pe.szExeFile[0]) out->insert(Lower(pe.szExeFile));
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    return true;
}

/* ---------------------------------------------------------------- */
/* game scan helpers                                                */
/* ---------------------------------------------------------------- */

std::wstring RegString(HKEY root, const wchar_t* sub, const wchar_t* value)
{
    std::wstring result;
    HKEY key = NULL;
    if (RegOpenKeyExW(root, sub, 0, KEY_READ, &key) != ERROR_SUCCESS) return result;

    wchar_t buf[1024];
    DWORD size = sizeof(buf);
    DWORD type = 0;
    if (RegQueryValueExW(key, value, NULL, &type, (LPBYTE)buf, &size) == ERROR_SUCCESS &&
        (type == REG_SZ || type == REG_EXPAND_SZ)) {
        buf[(sizeof(buf) / sizeof(buf[0])) - 1] = 0;
        result = buf;
    }
    RegCloseKey(key);
    return result;
}

std::string ReadWholeFileA(const std::wstring& path)
{
    std::string data;
    FILE* f = NULL;
    if (_wfopen_s(&f, path.c_str(), L"rb") != 0 || !f) return data;
    char buf[8192];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) data.append(buf, n);
    fclose(f);
    return data;
}

/* All "path" values of a libraryfolders.vdf (and the steam root itself). */
void ParseLibraryVdf(const std::string& vdf, std::vector<std::wstring>* out)
{
    size_t pos = 0;
    while ((pos = vdf.find("\"path\"", pos)) != std::string::npos) {
        size_t q1 = vdf.find('"', pos + 6);
        if (q1 == std::string::npos) break;
        const size_t q2 = vdf.find('"', q1 + 1);
        if (q2 == std::string::npos) break;
        std::string raw = vdf.substr(q1 + 1, q2 - q1 - 1);
        std::string unescaped;
        for (size_t i = 0; i < raw.size(); ++i) {
            if (raw[i] == '\\' && i + 1 < raw.size() && raw[i + 1] == '\\') { unescaped += '\\'; ++i; }
            else unescaped += raw[i];
        }
        const int len = MultiByteToWideChar(CP_UTF8, 0, unescaped.c_str(), -1, NULL, 0);
        if (len > 1) {
            std::wstring w((size_t)(len - 1), L'\0');
            MultiByteToWideChar(CP_UTF8, 0, unescaped.c_str(), -1, &w[0], len);
            if (!w.empty()) out->push_back(w);
        }
        pos = q2 + 1;
    }
}

std::string AcfValue(const std::string& acf, const char* key)
{
    const std::string pat = std::string("\"") + key + "\"";
    size_t pos = acf.find(pat);
    if (pos == std::string::npos) return std::string();
    size_t q1 = acf.find('"', pos + pat.size());
    if (q1 == std::string::npos) return std::string();
    size_t q2 = acf.find('"', q1 + 1);
    if (q2 == std::string::npos) return std::string();
    return acf.substr(q1 + 1, q2 - q1 - 1);
}

std::wstring Utf8ToWide(const std::string& s)
{
    if (s.empty()) return std::wstring();
    const int len = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, NULL, 0);
    if (len <= 1) return std::wstring();
    std::wstring w((size_t)(len - 1), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, &w[0], len);
    return w;
}

bool LooksLikeHelper(const std::wstring& exeLower)
{
    static const wchar_t* const kBad[] = {
        L"unins", L"uninstall", L"vcredist", L"dxsetup", L"dxwebsetup", L"dotnetfx",
        L"crashhandler", L"crashreport", L"unitycrashhandler", L"ue4prereqsetup",
        L"prereqsetup", L"setup.exe", L"installer", L"launcher_installer",
        L"easyanticheat", L"be_service", L"battleye", L"eac_", L"vgui", L"steamerrorreporter",
        L"touchup", L"cleanup", L"repair", L"diagnostics", L"benchmark",
        /* Engine helper programs that sit right next to the real exe and are
         * often *larger* than it (CS2's vconsole2.exe is 4.9 MB while cs2.exe
         * is 2.8 MB), so size-based scoring alone picks the wrong one. */
        L"vconsole", L"vguimatsurface", L"vtex", L"vpk", L"vrad", L"vbsp", L"vstub",
        L"crashpad", L"crashsender", L"dumper", L"reporter",
        L"legacy_app", L"dedicated", L"-tool", L"-server",
        /* redistributables ship inside game folders and are the only exe in the
         * folder, so the scoring would happily pick them */
        L"vc_redist", L"vcredist", L"vcrun", L"dxwebsetup", L"dxsetup",
        L"oalinst", L"physx", L"ue4prereq", L"dotnetfx", L"ndp4",
        NULL
    };
    if (exeLower.size() < 4) return true;
    if (exeLower.compare(exeLower.size() - 4, 4, L".exe") != 0) return true;
    for (int i = 0; kBad[i]; ++i) {
        if (exeLower.find(kBad[i]) != std::wstring::npos) return true;
    }
    return false;
}

void CollectExes(const std::wstring& dir, int depth, std::vector<std::wstring>* out)
{
    if (depth < 0 || out->size() > 40) return;

    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(Join(dir, L"*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;

    std::vector<std::wstring> subdirs;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) continue;
        const std::wstring name = fd.cFileName;
        if (name == L"." || name == L"..") continue;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            const std::wstring low = Lower(name);
            if (low == L"_commonredist" || low == L"directx" || low == L"dotnet" ||
                low == L"support" || low == L"redist" || low == L"installers") {
                continue;
            }
            subdirs.push_back(name);
            continue;
        }
        const std::wstring low = Lower(name);
        if (low.size() > 4 && low.compare(low.size() - 4, 4, L".exe") == 0) {
            if (!LooksLikeHelper(low)) out->push_back(Join(dir, name));
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);

    for (size_t i = 0; i < subdirs.size(); ++i) CollectExes(Join(dir, subdirs[i]), depth - 1, out);
}

/* Steam libraries also contain programs that are not games (OBS, Wallpaper
 * Engine, redistributables...). They would otherwise be auto-registered and
 * would open the overlay while the user is not playing anything. */
bool LooksLikeNonGameApp(const std::wstring& titleLower, const std::wstring& exeLower)
{
    static const wchar_t* const kNotGames[] = {
        L"obs studio", L"wallpaper engine", L"steamworks", L"steamvr", L"steam linux",
        L"proton", L"steam audio", L"unity hub", L"unreal engine", L"blender",
        L"visual studio", L"source sdk", L"goldSrc", L"dedicated server",
        L"epic games launcher", L"unreal engine prerequisites",
        NULL
    };
    static const wchar_t* const kNotGameExes[] = {
        L"obs64.exe", L"obs32.exe", L"wallpaperui.exe", L"wallpaper32.exe",
        L"wallpaper64.exe", L"webwallpaper32.exe", L"webwallpaper64.exe",
        L"steam.exe", L"steamwebhelper.exe", L"steamservice.exe",
        L"steamerrorreporter.exe", L"vrmonitor.exe", L"vrserver.exe",
        L"unityhub.exe", L"unrealeditor.exe",
        /* launchers themselves - they live next to the games and must never
         * gate the overlay */
        L"epicgameslauncher.exe", L"wegame.exe", L"galaxyclient.exe",
        L"galaxyclientservice.exe", L"battle.net.exe", L"agent.exe",
        L"origin.exe", L"eadesktop.exe", L"eabackgroundservice.exe",
        L"ubisoftconnect.exe", L"upc.exe", L"uplay.exe",
        L"riotclientservices.exe", L"riotclientcrashhandler.exe",
        L"xboxapp.exe", L"gamebar.exe", L"gamebarftserver.exe",
        L"launcher.exe", L"launcher_public.exe", L"unins000.exe",
        NULL
    };
    for (int i = 0; kNotGames[i]; ++i) {
        if (!titleLower.empty() && titleLower.find(kNotGames[i]) != std::wstring::npos) return true;
    }
    for (int i = 0; kNotGameExes[i]; ++i) {
        if (exeLower == kNotGameExes[i]) return true;
    }
    return false;
}

/* Pick the most likely main executable of a game folder. */std::wstring PickGameExe(const std::wstring& gameDir, const std::wstring& installdir)
{
    std::vector<std::wstring> found;
    CollectExes(gameDir, 3, &found);
    if (found.empty()) return std::wstring();

    std::wstring normFolder = Lower(installdir);
    normFolder.erase(std::remove_if(normFolder.begin(), normFolder.end(),
                                    [](wchar_t c) { return c == L' ' || c == L'-' || c == L'_' || c == L':'; }),
                     normFolder.end());

    std::wstring best;
    double bestScore = -1.0;
    for (size_t i = 0; i < found.size(); ++i) {
        const std::wstring base = Lower(BaseName(found[i]));
        std::wstring stem = base.substr(0, base.size() - 4);
        std::wstring normStem = stem;
        normStem.erase(std::remove_if(normStem.begin(), normStem.end(),
                                      [](wchar_t c) { return c == L' ' || c == L'-' || c == L'_' || c == L':'; }),
                       normStem.end());

        double score = 0.0;
        if (!normStem.empty() && normStem == normFolder) score += 200.0;
        else if (!normStem.empty() && normFolder.size() > 3 &&
                 (normStem.find(normFolder) != std::wstring::npos ||
                  normFolder.find(normStem) != std::wstring::npos)) {
            score += 80.0;
        }
        const std::wstring pathLow = Lower(found[i]);
        if (pathLow.find(L"\\bin\\win64\\") != std::wstring::npos) score += 40.0;
        else if (pathLow.find(L"\\bin\\") != std::wstring::npos) score += 20.0;
        else if (pathLow.find(L"\\win64\\") != std::wstring::npos) score += 25.0;
        if (pathLow.find(L"\\bin\\") == std::wstring::npos && pathLow.find(L"\\game\\") == std::wstring::npos) {
            score += 10.0;   /* shallow exe is more likely the entry point */
        }

        WIN32_FILE_ATTRIBUTE_DATA fad;
        if (GetFileAttributesExW(found[i].c_str(), GetFileExInfoStandard, &fad)) {
            const ULONGLONG size = ((ULONGLONG)fad.nFileSizeHigh << 32) | fad.nFileSizeLow;
            score += (double)(size / (1024ull * 1024ull)) * 0.5;   /* MB * 0.5, capped below */
            if (size > 512ull * 1024ull * 1024ull) score -= 200.0; /* data blob, not the game */
        }
        if (score > bestScore) { bestScore = score; best = found[i]; }
    }
    return best;
}

std::vector<std::wstring> SteamRoots()
{
    std::vector<std::wstring> roots;
    std::wstring p = RegString(HKEY_CURRENT_USER, L"Software\\Valve\\Steam", L"SteamPath");
    if (p.empty()) p = RegString(HKEY_LOCAL_MACHINE, L"SOFTWARE\\WOW6432Node\\Valve\\Steam", L"InstallPath");
    if (p.empty()) p = RegString(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Valve\\Steam", L"InstallPath");
    if (!p.empty()) roots.push_back(p);

    /* library folders listed in the vdf files of every known root */
    std::vector<std::wstring> libs;
    for (size_t i = 0; i < roots.size(); ++i) {
        const std::wstring root = roots[i];
        libs.push_back(root);
        const wchar_t* const candidates[] = {
            L"steamapps\\libraryfolders.vdf", L"config\\libraryfolders.vdf", NULL
        };
        for (int c = 0; candidates[c]; ++c) {
            const std::wstring vdfPath = Join(root, candidates[c]);
            if (!FileExists(vdfPath)) continue;
            std::vector<std::wstring> more;
            ParseLibraryVdf(ReadWholeFileA(vdfPath), &more);
            for (size_t m = 0; m < more.size(); ++m) libs.push_back(more[m]);
        }
    }

    std::vector<std::wstring> unique;
    for (size_t i = 0; i < libs.size(); ++i) {
        if (libs[i].empty()) continue;
        bool dup = false;
        for (size_t j = 0; j < unique.size(); ++j) {
            if (Lower(unique[j]) == Lower(libs[i])) { dup = true; break; }
        }
        if (!dup && DirExists(libs[i])) unique.push_back(libs[i]);
    }
    return unique;
}

/* ---------------------------------------------------------------- */
/* other launchers                                                  */
/* ---------------------------------------------------------------- */

/* One installed game folder that the scan should look at. `name` is what the
 * launcher calls the game; PickGameExe() uses it to score the candidates. */
struct GameFolder {
    std::wstring dir;
    std::wstring name;
};

/* Folders inside a launcher tree that hold infrastructure, not games. WeGame
 * for example keeps "common_apps\dependency_shared(999998)\vc2015-2022x64\
 * vc_redist.x64.exe" right next to the real titles, and that file scores well
 * as a "main executable" because it is the only exe in its folder. */
bool LooksLikeSupportFolder(const std::wstring& nameLower)
{
    static const wchar_t* const kBad[] = {
        L"common_apps", L"downloading", L"download", L"rail_user_data", L"rail_apps",
        L"dependency", L"dependencies", L"_commonredist", L"commonredist",
        L"redistributables", L"redist", L"directx", L"dotnet", L"vcredist",
        L"launcher", L"eosoverlay", L"epic online services", L"gamesave",
        L"support", L"tools", L"engine", L"hosting", L"bootstrap",
        NULL
    };
    if (nameLower.empty()) return true;
    for (int i = 0; kBad[i]; ++i) {
        if (nameLower.find(kBad[i]) != std::wstring::npos) return true;
    }
    return false;
}

void AddFolder(std::vector<GameFolder>* out, const std::wstring& dir, const std::wstring& name)
{
    if (dir.empty() || !DirExists(dir)) return;
    const std::wstring leaf = Lower(name.empty() ? BaseName(dir) : name);
    if (LooksLikeSupportFolder(leaf)) return;
    const std::wstring low = Lower(dir);
    for (size_t i = 0; i < out->size(); ++i) {
        if (Lower((*out)[i].dir) == low) return;   /* same folder reached twice */
    }
    GameFolder f;
    f.dir  = dir;
    f.name = name.empty() ? BaseName(dir) : name;
    out->push_back(f);
}

/* Every sub folder of `parent` is treated as one game. */
void AddChildFolders(std::vector<GameFolder>* out, const std::wstring& parent)
{
    if (!DirExists(parent)) return;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(Join(parent, L"*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) continue;
        const std::wstring name = fd.cFileName;
        if (name == L"." || name == L"..") continue;
        AddFolder(out, Join(parent, name), name);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

void RegEnumSubKeys(HKEY root, const wchar_t* sub, std::vector<std::wstring>* out)
{
    HKEY key = NULL;
    if (RegOpenKeyExW(root, sub, 0, KEY_READ, &key) != ERROR_SUCCESS) return;
    wchar_t name[256];
    for (DWORD i = 0;; ++i) {
        DWORD len = 256;
        if (RegEnumKeyExW(key, i, name, &len, NULL, NULL, NULL, NULL) != ERROR_SUCCESS) break;
        out->push_back(name);
    }
    RegCloseKey(key);
}

std::wstring RegStringBoth(HKEY root, const wchar_t* sub64, const wchar_t* sub32,
                           const wchar_t* value)
{
    std::wstring v = RegString(root, sub64, value);
    if (v.empty() && sub32) v = RegString(root, sub32, value);
    return v;
}

std::string JsonRaw(const std::string& json, const char* key)
{
    const std::string pat = std::string("\"") + key + "\"";
    size_t pos = json.find(pat);
    if (pos == std::string::npos) return std::string();
    pos = json.find(':', pos + pat.size());
    if (pos == std::string::npos) return std::string();
    const size_t q1 = json.find('"', pos);
    if (q1 == std::string::npos) return std::string();
    const size_t q2 = json.find('"', q1 + 1);
    if (q2 == std::string::npos) return std::string();

    const std::string raw = json.substr(q1 + 1, q2 - q1 - 1);
    std::string un;
    for (size_t i = 0; i < raw.size(); ++i) {
        if (raw[i] == '\\' && i + 1 < raw.size()) { un += raw[i + 1]; ++i; continue; }
        un += raw[i];
    }
    return un;
}

std::vector<std::wstring> FixedDrives()
{
    std::vector<std::wstring> out;
    const DWORD mask = GetLogicalDrives();
    for (int i = 0; i < 26; ++i) {
        if ((mask & (1u << i)) == 0) continue;
        wchar_t root[4] = { (wchar_t)(L'A' + i), L':', L'\\', 0 };
        if (GetDriveTypeW(root) == DRIVE_FIXED) out.push_back(root);
    }
    return out;
}

/* Epic Games: LauncherInstalled.dat has one InstallLocation per installed app;
 * the per-app .item manifests add a human readable DisplayName. */
void AddEpic(std::vector<GameFolder>* out)
{
    const std::wstring dat = L"C:\\ProgramData\\Epic\\UnrealEngineLauncher\\LauncherInstalled.dat";
    const std::string json = ReadWholeFileA(dat);
    size_t pos = 0;
    while ((pos = json.find("\"InstallLocation\"", pos)) != std::string::npos) {
        const std::wstring dir = Utf8ToWide(JsonRaw(json.substr(pos), "InstallLocation"));
        AddFolder(out, dir, BaseName(dir));
        pos += 17;
    }

    const std::wstring manDir = L"C:\\ProgramData\\Epic\\EpicGamesLauncher\\Data\\Manifests";
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(Join(manDir, L"*.item").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        const std::string item = ReadWholeFileA(Join(manDir, fd.cFileName));
        if (item.empty()) continue;
        const std::wstring dir = Utf8ToWide(JsonRaw(item, "InstallLocation"));
        if (dir.empty()) continue;
        AddFolder(out, dir, Utf8ToWide(JsonRaw(item, "DisplayName")));
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

/* GOG Galaxy keeps one key per game with both the folder and the title. */
void AddGog(std::vector<GameFolder>* out)
{
    static const wchar_t* const kRoots[] = {
        L"SOFTWARE\\WOW6432Node\\GOG.com\\Games", L"SOFTWARE\\GOG.com\\Games", NULL
    };
    for (int r = 0; kRoots[r]; ++r) {
        std::vector<std::wstring> ids;
        RegEnumSubKeys(HKEY_LOCAL_MACHINE, kRoots[r], &ids);
        for (size_t i = 0; i < ids.size(); ++i) {
            const std::wstring sub = std::wstring(kRoots[r]) + L"\\" + ids[i];
            const std::wstring path = RegString(HKEY_LOCAL_MACHINE, sub.c_str(), L"path");
            const std::wstring name = RegString(HKEY_LOCAL_MACHINE, sub.c_str(), L"gameName");
            AddFolder(out, path, name);
        }
    }
}

/* Battle.net keeps an InstallPath per title. */
void AddBattleNet(std::vector<GameFolder>* out)
{
    static const wchar_t* const kRoots[] = {
        L"SOFTWARE\\WOW6432Node\\Blizzard Entertainment",
        L"SOFTWARE\\Blizzard Entertainment", NULL
    };
    for (int r = 0; kRoots[r]; ++r) {
        std::vector<std::wstring> games;
        RegEnumSubKeys(HKEY_LOCAL_MACHINE, kRoots[r], &games);
        for (size_t i = 0; i < games.size(); ++i) {
            if (Lower(games[i]) == L"launcher") continue;
            const std::wstring sub = std::wstring(kRoots[r]) + L"\\" + games[i];
            const std::wstring path = RegString(HKEY_LOCAL_MACHINE, sub.c_str(), L"InstallPath");
            AddFolder(out, path, games[i]);
        }
    }
}

/* EA app / Origin: one key per title with "Install Dir". */
void AddEa(std::vector<GameFolder>* out)
{
    static const wchar_t* const kRoots[] = {
        L"SOFTWARE\\WOW6432Node\\EA Games", L"SOFTWARE\\EA Games",
        L"SOFTWARE\\WOW6432Node\\Electronic Arts\\EA Games", NULL
    };
    for (int r = 0; kRoots[r]; ++r) {
        std::vector<std::wstring> games;
        RegEnumSubKeys(HKEY_LOCAL_MACHINE, kRoots[r], &games);
        for (size_t i = 0; i < games.size(); ++i) {
            const std::wstring sub = std::wstring(kRoots[r]) + L"\\" + games[i];
            std::wstring path = RegString(HKEY_LOCAL_MACHINE, sub.c_str(), L"Install Dir");
            if (path.empty()) path = RegString(HKEY_LOCAL_MACHINE, sub.c_str(), L"InstallDir");
            AddFolder(out, path, games[i]);
        }
    }
}

/* Ubisoft Connect: one key per title with InstallDir. */
void AddUbisoft(std::vector<GameFolder>* out)
{
    static const wchar_t* const kRoots[] = {
        L"SOFTWARE\\WOW6432Node\\Ubisoft", L"SOFTWARE\\Ubisoft", NULL
    };
    for (int r = 0; kRoots[r]; ++r) {
        std::vector<std::wstring> games;
        RegEnumSubKeys(HKEY_LOCAL_MACHINE, kRoots[r], &games);
        for (size_t i = 0; i < games.size(); ++i) {
            const std::wstring low = Lower(games[i]);
            if (low == L"launcher" || low == L"installer") continue;
            const std::wstring sub = std::wstring(kRoots[r]) + L"\\" + games[i];
            std::wstring path = RegString(HKEY_LOCAL_MACHINE, sub.c_str(), L"InstallDir");
            if (path.empty()) path = RegString(HKEY_LOCAL_MACHINE, sub.c_str(), L"Install Dir");
            AddFolder(out, path, games[i]);
        }
    }
}

/* WeGame, Riot, Xbox and the usual "my games live here" folders are plain
 * directory trees, so every child of them is one game. */
void AddWellKnownTrees(std::vector<GameFolder>* out)
{
    static const wchar_t* const kChildren[] = {
        L"WeGameApps", L"Riot Games", L"XboxGames", L"Epic Games", L"GOG Games",
        L"EA Games", L"Games", L"Game", L"Ubisoft Game Launcher\\games",
        NULL
    };

    /* the WeGame install root from the registry, if the games do not sit next
     * to the drive root but inside the launcher folder */
    const std::wstring wegame = RegStringBoth(
        HKEY_LOCAL_MACHINE, L"SOFTWARE\\Tencent\\WeGame", L"SOFTWARE\\WOW6432Node\\Tencent\\WeGame",
        L"InstallPath");
    if (!wegame.empty()) {
        AddChildFolders(out, Join(wegame, L"games"));
        AddChildFolders(out, Join(wegame, L"Games"));
    }

    const std::wstring riot = RegStringBoth(
        HKEY_LOCAL_MACHINE, L"SOFTWARE\\Riot Games, Inc.\\Riot Client", NULL, L"InstallLocation");
    if (!riot.empty()) {
        AddChildFolders(out, Join(riot, L"..\\.."));
    }

    const std::vector<std::wstring> drives = FixedDrives();
    for (size_t d = 0; d < drives.size(); ++d) {
        for (int c = 0; kChildren[c]; ++c) AddChildFolders(out, Join(drives[d], kChildren[c]));
    }
}

/* ------------------------------------------------------------------ */
/* desktop / start-menu shortcuts (.lnk)                              */
/* ------------------------------------------------------------------ */
/* Store-based detection only finds games whose launcher wrote a manifest or a
 * registry key. Anything installed from a zip, a portable copy, or a launcher
 * we do not know (and every game the user only reaches through a desktop icon)
 * has just a shortcut. Those shortcuts name the real executable, so they are
 * resolved and their targets fed into the normal scoring below. */

bool ReadShortcutTarget(const std::wstring& lnkPath, std::wstring* exeOut)
{
    if (!exeOut) return false;
    exeOut->clear();

    IShellLinkW* link = NULL;
    HRESULT hr = CoCreateInstance(CLSID_ShellLink, NULL, CLSCTX_INPROC_SERVER,
                                  IID_IShellLinkW, (void**)&link);
    if (FAILED(hr) || !link) return false;

    bool ok = false;
    IPersistFile* file = NULL;
    if (SUCCEEDED(link->QueryInterface(IID_IPersistFile, (void**)&file)) && file) {
        if (SUCCEEDED(file->Load(lnkPath.c_str(), STGM_READ))) {
            wchar_t target[MAX_PATH * 2] = {0};
            WIN32_FIND_DATAW fd;
            ZeroMemory(&fd, sizeof(fd));
            /* SLGP_RAWPATH keeps the real path (SLGP_UNCPRIORITY would rewrite
             * local paths to UNC names on some setups) */
            if (SUCCEEDED(link->GetPath(target, MAX_PATH * 2, &fd, SLGP_RAWPATH)) &&
                target[0]) {
                *exeOut = target;
                ok = true;
            }
        }
        file->Release();
    }
    link->Release();
    return ok;
}

/* ---------------------------------------------------------------- */
/* "is this folder a game?" for shortcuts                            */
/* ---------------------------------------------------------------- */

/* Electron/CEF applications (chat clients, launchers, dev tools) ship
 * icudtl.dat and resources.pak, sometimes next to a >100 MB helper archive
 * (Doubao's local_webcontents\biz\biz.pak is 186 MB), so they would otherwise
 * pass the big-archive test. They are never games. */
bool HasElectronShape(const std::wstring& dir)
{
    if (FileExists(Join(dir, L"icudtl.dat"))) return true;
    if (FileExists(Join(dir, L"resources\\app.asar"))) return true;

    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(Join(dir, L"*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return false;
    bool found = false;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
        if (fd.cFileName[0] == L'.') continue;
        const std::wstring sub = Join(dir, fd.cFileName);
        if (FileExists(Join(sub, L"icudtl.dat")) ||
            FileExists(Join(sub, L"app.asar")) ||
            FileExists(Join(sub, L"resources\\app.asar"))) {
            found = true;
            break;
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return found;
}

/* Large content archive = the folder is a game (Unity .pak, RPG Maker .rgss3a,
 * Ren'Py .rpa, Godot .pck, Source .vpk). Chrome/CEF resource paks are excluded
 * by name and by their small size. Threshold 40 MB: 植物大战僵尸's main.pak is
 * 139 MB while the biggest non-game .pak on the test machine was 14 MB. */
bool HasBigContentArchive(const std::wstring& dir, int depth)
{
    if (depth < 0) return false;

    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(Join(dir, L"*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return false;

    bool found = false;
    do {
        if (fd.cFileName[0] == L'.') continue;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) continue;

        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            const std::wstring low = Lower(fd.cFileName);
            if (low == L"locales" || low == L"swiftshader" || low == L"copying" ||
                low == L"_commonredist" || low == L"redist" || low == L"directx") {
                continue;
            }
            continue;   /* recursed below, after closing the handle */
        }

        const std::wstring low = Lower(fd.cFileName);
        if (low.find(L"chrome") != std::wstring::npos) continue;
        if (low.find(L"_percent") != std::wstring::npos) continue;
        if (low.find(L"resources.pak") != std::wstring::npos) continue;

        bool extOk = false;
        static const wchar_t* const kExts[] = { L".pak", L".rpa", L".rgss3a", L".pck", L".vpk", NULL };
        for (int i = 0; kExts[i]; ++i) {
            const size_t n = wcslen(kExts[i]);
            if (low.size() > n && low.compare(low.size() - n, n, kExts[i]) == 0) { extOk = true; break; }
        }
        if (!extOk) continue;

        const ULONGLONG size = ((ULONGLONG)fd.nFileSizeHigh << 32) | fd.nFileSizeLow;
        if (size >= 40ULL * 1024ULL * 1024ULL) { found = true; break; }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    if (found || depth == 0) return found;

    /* second pass for the sub folders - the handle is closed by now */
    h = FindFirstFileW(Join(dir, L"*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return false;
    std::vector<std::wstring> subs;
    do {
        if (fd.cFileName[0] == L'.') continue;
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) continue;
        const std::wstring low = Lower(fd.cFileName);
        if (low == L"locales" || low == L"swiftshader" || low == L"copying" ||
            low == L"node_modules" || low == L"windows") {
            continue;
        }
        subs.push_back(fd.cFileName);
    } while (FindNextFileW(h, &fd));
    FindClose(h);

    for (size_t i = 0; i < subs.size(); ++i) {
        if (HasBigContentArchive(Join(dir, subs[i]), depth - 1)) return true;
    }
    return false;
}

/* Engine fingerprints. Checked in the exe folder first, then one level up,
 * because UE and Unity games usually keep the exe in Binaries\Win64 or bin\. */
bool HasEngineMarker(const std::wstring& dir)
{
    static const wchar_t* const kFiles[] = {
        L"UnityPlayer.dll", L"GameAssembly.dll", L"UnityCrashHandler64.exe",
        L"Engine\\Binaries\\Win64", L"Engine\\Content", L"Content\\Paks", L"GodotSharp",
        NULL
    };
    for (int i = 0; kFiles[i]; ++i) {
        const DWORD a = GetFileAttributesW(Join(dir, kFiles[i]).c_str());
        if (a != INVALID_FILE_ATTRIBUTES) return true;
    }

    /* <Game>_Data\globalgamemanagers is the definitive Unity marker; the bare
     * *_Data folder name alone is not (many apps ship <App>_Data). */
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(Join(dir, L"*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return false;
    bool found = false;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
        if (fd.cFileName[0] == L'.') continue;
        const std::wstring sub = Join(dir, fd.cFileName);
        if (FileExists(Join(sub, L"globalgamemanagers"))) { found = true; break; }
        if (DirExists(Join(sub, L"Content\\Paks"))) { found = true; break; }
        if (FileExists(Join(sub, L"UnityPlayer.dll"))) { found = true; break; }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return found;
}

/* Never treat system / program folders as games, even if they contain a
 * UnityPlayer.dll (nothing does today, but the check is cheap). */
bool IsSystemFolder(const std::wstring& dirLower)
{
    if (dirLower.find(L"\\windows\\") != std::wstring::npos) return true;
    if (dirLower.size() > 8 && dirLower.compare(dirLower.size() - 8, 8, L"\\windows") == 0) return true;
    if (dirLower.find(L"\\program files") != std::wstring::npos) return true;
    if (dirLower.find(L"\\appdata\\") != std::wstring::npos) return true;
    if (dirLower.find(L"\\programdata\\") != std::wstring::npos) return true;
    return false;
}

/* Recursive size probe with an early exit once the target is reached, so a
 * huge folder costs at most one traversal of the first directories. */
bool FolderAtLeast(const std::wstring& dir, int depth, ULONGLONG* total)
{
    if (depth < 0 || !total) return false;

    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(Join(dir, L"*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return false;

    std::vector<std::wstring> subs;
    bool reached = false;
    do {
        if (fd.cFileName[0] == L'.') continue;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) continue;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            subs.push_back(fd.cFileName);
        } else {
            *total += ((ULONGLONG)fd.nFileSizeHigh << 32) | fd.nFileSizeLow;
            if (*total >= 400ULL * 1024ULL * 1024ULL) { reached = true; break; }
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    if (reached) return true;

    for (size_t i = 0; i < subs.size(); ++i) {
        if (FolderAtLeast(Join(dir, subs[i]), depth - 1, total)) return true;
    }
    return *total >= 400ULL * 1024ULL * 1024ULL;
}

/* Big install folder that carries an engine-layout directory (Binaries/Engine/
 * Game/Win64). Used for games whose engine files sit one level below the exe,
 * e.g. 去约/ACLOS: aclos-launcher.exe with Binaries\ and Launcher\ next to it,
 * 814 MB total. Threshold 400 MB keeps small Electron tools out. */
bool HasEngineLayoutAndBulk(const std::wstring& dir)
{
    static const wchar_t* const kDirs[] = { L"Binaries", L"Engine", L"Win64", L"Game", NULL };
    bool layout = false;
    for (int i = 0; kDirs[i]; ++i) {
        if (DirExists(Join(dir, kDirs[i]))) { layout = true; break; }
    }
    if (!layout) return false;

    ULONGLONG total = 0;
    return FolderAtLeast(dir, 2, &total);
}

/* The decision for a shortcut target: engine marker, or a big content archive
 * in a folder that is not an Electron app. */
bool LooksLikeGameFolder(const std::wstring& exePath)
{
    std::wstring dir = DirName(exePath);
    if (dir.empty()) return false;
    if (IsSystemFolder(Lower(dir))) return false;

    if (HasEngineMarker(dir)) return true;

    const std::wstring leaf = Lower(BaseName(dir));
    if (leaf == L"bin" || leaf == L"win64" || leaf == L"binaries" || leaf == L"win32" ||
        leaf == L"x64" || leaf == L"game") {
        const std::wstring parent = DirName(dir);
        if (!parent.empty() && !IsSystemFolder(Lower(parent)) && HasEngineMarker(parent)) return true;
    }

    /* A 400 MB folder laid out like a game (Binaries/Engine/Game/Win64) - 去约
     * keeps its payload there while shipping an Electron helper in Launcher\,
     * so this test must not be gated by the Electron check below. */
    if (HasEngineLayoutAndBulk(dir)) return true;

    if (HasElectronShape(dir)) return false;
    return HasBigContentArchive(dir, 2);
}

void CollectShortcutsIn(const std::wstring& dir, int depth, std::vector<std::wstring>* out)
{
    if (!out || depth < 0 || !DirExists(dir)) return;

    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(Join(dir, L"*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (fd.cFileName[0] == L'.') continue;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) continue;

        const std::wstring full = Join(dir, fd.cFileName);
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            if (depth > 0) CollectShortcutsIn(full, depth - 1, out);
            continue;
        }

        const std::wstring lower = Lower(fd.cFileName);
        if (lower.size() < 4 || lower.compare(lower.size() - 4, 4, L".lnk") != 0) continue;

        std::wstring target;
        if (!ReadShortcutTarget(full, &target)) continue;
        if (!FileExists(target)) continue;

        const std::wstring exe = Lower(BaseName(target));
        if (exe.size() >= 4 && exe.compare(exe.size() - 4, 4, L".exe") == 0) {
            if (!LooksLikeGameFolder(target)) continue;
            out->push_back(exe);
            continue;
        }

        /* Some shortcuts point at an icon or a readme sitting next to the
         * game (植物大战僵尸's desktop shortcut targets logo.ico). Fall back to
         * the containing folder so the real exe can still be found. */
        const std::wstring folder = DirName(target);
        if (folder.empty() || IsSystemFolder(Lower(folder))) continue;
        if (!LooksLikeGameFolder(target)) continue;
        const std::wstring picked = PickGameExe(folder, BaseName(folder));
        if (picked.empty()) continue;
        out->push_back(Lower(BaseName(picked)));
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

/* Shortcut folders that users actually use: both desktops and the start menu
 * (all users + current user, including the per-user Start Menu\Programs tree).
 * Returns lower-cased exe names, because a shortcut has no install folder. */
std::vector<std::wstring> ShortcutExes()
{
    std::vector<std::wstring> out;

    /* CLSID_ShellLink is a COM class: CoCreateInstance fails with
     * CO_E_NOTINITIALIZED (0x800401F0) on a thread that never ran
     * CoInitialize, which silently produced an empty shortcut scan.
     * Balanced with CoUninitialize below. */
    HRESULT coHr = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    const bool coInit = SUCCEEDED(coHr);

    struct Folder {
        int   csidl;
        int   depth;
    };
    static const Folder kFolders[] = {
        { CSIDL_DESKTOPDIRECTORY,        0 },
        { CSIDL_COMMON_DESKTOPDIRECTORY, 0 },
        { CSIDL_PROGRAMS,                3 },
        { CSIDL_COMMON_PROGRAMS,         3 },
    };

    for (size_t i = 0; i < sizeof(kFolders) / sizeof(kFolders[0]); ++i) {
        wchar_t path[MAX_PATH * 2] = {0};
        if (FAILED(SHGetFolderPathW(NULL, kFolders[i].csidl, NULL, SHGFP_TYPE_CURRENT, path))) {
            continue;
        }
        if (!path[0]) continue;
        CollectShortcutsIn(path, kFolders[i].depth, &out);
    }

    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    if (coInit) CoUninitialize();
    return out;
}

/* Everything the scan should look at, Steam plus the other launchers. */
std::vector<GameFolder> LauncherGameFolders(){
    std::vector<GameFolder> out;

    /* ---- Steam: the acf files give both folder and title ---- */
    const std::vector<std::wstring> roots = SteamRoots();
    for (size_t r = 0; r < roots.size(); ++r) {
        std::wstring lib = roots[r];
        if (DirExists(Join(lib, L"steamapps"))) lib = Join(lib, L"steamapps");
        if (!DirExists(lib)) continue;

        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileW(Join(lib, L"appmanifest_*.acf").c_str(), &fd);
        if (h == INVALID_HANDLE_VALUE) continue;
        do {
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
            const std::string acf = ReadWholeFileA(Join(lib, fd.cFileName));
            if (acf.empty()) continue;
            const std::wstring installdir = Utf8ToWide(AcfValue(acf, "installdir"));
            if (installdir.empty()) continue;
            AddFolder(&out, Join(Join(lib, L"common"), installdir), installdir);
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }

    AddEpic(&out);
    AddGog(&out);
    AddBattleNet(&out);
    AddEa(&out);
    AddUbisoft(&out);
    AddWellKnownTrees(&out);
    return out;
}

} // namespace

/* ---------------------------------------------------------------- */
/* public API                                                       */
/* ---------------------------------------------------------------- */

void GamesInit(const std::wstring& iniPath)
{
    std::lock_guard<std::mutex> lk(g_lock);
    g_iniPath = iniPath;
    g_cacheValid = false;
    LoadLocked();
}

bool GamesIsIgnored(const std::wstring& exe)
{
    const std::wstring key = Lower(exe);
    std::lock_guard<std::mutex> lk(g_lock);
    return g_ignored.count(key) != 0;
}

void GamesSetIgnored(const std::wstring& exe, bool ignored)
{
    const std::wstring key = Lower(exe);
    if (key.empty()) return;
    {
        std::lock_guard<std::mutex> lk(g_lock);
        if (ignored) g_ignored.insert(key);
        else         g_ignored.erase(key);
    }
    GamesSave();
}

void WriteSection(const std::wstring& blob)
{
    /* WritePrivateProfileSectionW() rewrites the whole file and silently drops
     * every other section - it deleted [overlay] (position, lock, gate) the
     * first time a scan added a game. So the file is edited textually: keep
     * everything outside [games] byte for byte and swap only that block. */
    if (g_iniPath.empty()) return;

    std::string text;
    {
        HANDLE f = CreateFileW(g_iniPath.c_str(), GENERIC_READ, FILE_SHARE_READ, NULL,
                               OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
        if (f != INVALID_HANDLE_VALUE) {
            LARGE_INTEGER size;
            if (GetFileSizeEx(f, &size) && size.QuadPart > 0 && size.QuadPart < (1 << 20)) {
                text.resize((size_t)size.QuadPart);
                DWORD got = 0;
                ReadFile(f, &text[0], (DWORD)text.size(), &got, NULL);
                text.resize(got);
            }
            CloseHandle(f);
        }
    }

    /* blob is "key=value\0key=value\0\0"; turn it into CRLF lines */
    std::string body;
    {
        const wchar_t* p = blob.c_str();
        while (*p) {
            std::wstring line(p);
            p += line.size() + 1;
            const int n = WideCharToMultiByte(CP_ACP, 0, line.c_str(), (int)line.size(),
                                              NULL, 0, NULL, NULL);
            std::string narrow((size_t)n, '\0');
            WideCharToMultiByte(CP_ACP, 0, line.c_str(), (int)line.size(), &narrow[0], n,
                                NULL, NULL);
            body += narrow;
            body += "\r\n";
        }
    }

    const std::string header = "[games]";
    std::string out;
    const size_t start = text.find(header);
    if (start == std::string::npos) {
        out = text;
        if (!out.empty() && out[out.size() - 1] != '\n') out += "\r\n";
        out += header + "\r\n" + body;
    } else {
        /* the section ends at the next line that starts with '[' */
        size_t end = text.find("\n[", start);
        end = (end == std::string::npos) ? text.size() : end + 1;
        out = text.substr(0, start) + header + "\r\n" + body + text.substr(end);
    }

    HANDLE w = CreateFileW(g_iniPath.c_str(), GENERIC_WRITE, 0, NULL,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (w == INVALID_HANDLE_VALUE) return;
    DWORD wrote = 0;
    WriteFile(w, out.c_str(), (DWORD)out.size(), &wrote, NULL);
    CloseHandle(w);
}

void GamesSave()
{
    if (g_iniPath.empty()) return;

    std::wstring blob;
    {
        std::lock_guard<std::mutex> lk(g_lock);
        for (size_t i = 0; i < g_games.size(); ++i) {
            wchar_t val[16];
            wsprintfW(val, L"%d|%d", g_games[i].enabled ? 1 : 0, g_games[i].autoAdded ? 1 : 0);
            blob += g_games[i].exe;
            blob += L'=';
            blob += val;
            blob += L'\0';
        }
        blob += kAutoKey;
        blob += L'=';
        blob += (g_autoDetect ? L"1" : L"0");
        blob += L'\0';
        if (!g_ignored.empty()) {
            blob += kIgnoredKey;
            blob += L'=';
            bool first = true;
            for (std::set<std::wstring>::const_iterator it = g_ignored.begin();
                 it != g_ignored.end(); ++it) {
                if (!first) blob += L'|';
                blob += *it;
                first = false;
            }
            blob += L'\0';
        }
        blob += L'\0';
    }
    WriteSection(blob);
}

std::vector<GameEntry> GamesList()
{
    std::lock_guard<std::mutex> lk(g_lock);
    return g_games;
}

bool GamesContains(const std::wstring& exe)
{
    const std::wstring key = Lower(exe);
    std::lock_guard<std::mutex> lk(g_lock);
    for (size_t i = 0; i < g_games.size(); ++i) {
        if (g_games[i].exe == key) return true;
    }
    return false;
}

bool GamesAdd(const std::wstring& exe)
{
    std::wstring key = Lower(BaseName(exe));
    if (key.empty() || key.size() <= 4) return false;
    if (key.compare(key.size() - 4, 4, L".exe") != 0) return false;

    bool added = false;
    {
        std::lock_guard<std::mutex> lk(g_lock);
        /* The user removed this one earlier: respect that until it is added by
         * hand (GamesAdd() with the ignore flag cleared by settings.cpp). */
        if (g_ignored.count(key) != 0) return false;
        bool exists = false;
        for (size_t i = 0; i < g_games.size(); ++i) {
            if (g_games[i].exe == key) { exists = true; break; }
        }
        if (!exists) {
            GameEntry e;
            e.exe = key;
            e.enabled = true;      /* a newly added game shows the overlay by default */
            e.autoAdded = false;
            g_games.push_back(e);
            added = true;
        }
    }
    if (added) { GamesSave(); g_cacheValid = false; }
    return added;
}

bool GamesRemove(const std::wstring& exe)
{
    const std::wstring key = Lower(exe);
    bool removed = false;
    {
        std::lock_guard<std::mutex> lk(g_lock);
        for (size_t i = 0; i < g_games.size(); ++i) {
            if (g_games[i].exe == key) { g_games.erase(g_games.begin() + (ptrdiff_t)i); removed = true; break; }
        }
    }
    if (removed) { GamesSave(); g_cacheValid = false; }
    return removed;
}

bool GamesEntryEnabled(const std::wstring& exe)
{
    const std::wstring key = Lower(exe);
    std::lock_guard<std::mutex> lk(g_lock);
    for (size_t i = 0; i < g_games.size(); ++i) {
        if (g_games[i].exe == key) return g_games[i].enabled;
    }
    return false;
}

bool GamesSetEnabled(const std::wstring& exe, bool enabled)
{
    const std::wstring key = Lower(exe);
    bool changed = false;
    {
        std::lock_guard<std::mutex> lk(g_lock);
        for (size_t i = 0; i < g_games.size(); ++i) {
            if (g_games[i].exe == key && g_games[i].enabled != enabled) {
                g_games[i].enabled = enabled;
                changed = true;
            }
        }
    }
    if (changed) { GamesSave(); g_cacheValid = false; }
    return changed;
}

void GamesSetAutoDetect(bool on)
{
    {
        std::lock_guard<std::mutex> lk(g_lock);
        if (g_autoDetect == on) return;
        g_autoDetect = on;
    }
    GamesSave();
}

bool GamesAutoDetect()
{
    std::lock_guard<std::mutex> lk(g_lock);
    return g_autoDetect;
}

bool GamesAnyRunning()
{
    return !GamesRunningName().empty();
}

std::wstring GamesRunningName()
{
    {
        std::lock_guard<std::mutex> lk(g_lock);
        const DWORD now = GetTickCount();
        if (g_cacheValid && (DWORD)(now - g_cacheAt) < kCacheMs) return g_cacheName;
    }

    std::set<std::wstring> running;
    SnapshotExes(&running);

    std::wstring hit;
    {
        std::lock_guard<std::mutex> lk(g_lock);
        for (size_t i = 0; i < g_games.size(); ++i) {
            if (!g_games[i].enabled) continue;
            if (running.count(g_games[i].exe)) { hit = g_games[i].exe; break; }
        }
        g_cacheName  = hit;
        g_cacheAny   = !hit.empty();
        g_cacheValid = true;
        g_cacheAt    = GetTickCount();
    }
    return hit;
}

std::vector<std::wstring> GamesScanForNew()
{
    std::vector<std::wstring> added;

    /* Steam, Epic, GOG, Battle.net, EA, Ubisoft, WeGame, Riot, Xbox and the
     * usual "Games" trees - see LauncherGameFolders(). */
    const std::vector<GameFolder> folders = LauncherGameFolders();
    for (size_t r = 0; r < folders.size(); ++r) {
        const std::wstring exePath = PickGameExe(folders[r].dir, folders[r].name);
        if (exePath.empty()) continue;

        const std::wstring exe = Lower(BaseName(exePath));
        /* Store libraries hold tools too (OBS, Wallpaper Engine, SDKs, engine
         * helpers). They are not games: registering them would pop the overlay
         * while the user is doing something else entirely. */
        if (LooksLikeNonGameApp(Lower(folders[r].name), exe)) continue;
        if (GamesIsIgnored(exe)) continue;
        if (GamesContains(exe)) continue;
        if (GamesAdd(exe)) {
            /* ensure the auto flag survives the reload */
            {
                std::lock_guard<std::mutex> lk(g_lock);
                for (size_t i = 0; i < g_games.size(); ++i) {
                    if (g_games[i].exe == exe) g_games[i].autoAdded = true;
                }
            }
            added.push_back(exe);
        }
    }
    if (!added.empty()) GamesSave();
    return added;
}

std::vector<std::wstring> GamesScanShortcuts()
{
    std::vector<std::wstring> added;

    const std::vector<std::wstring> exes = ShortcutExes();
    for (size_t i = 0; i < exes.size(); ++i) {
        const std::wstring& exe = exes[i];
        /* A shortcut is deliberately generic: it says "this is on my desktop",
         * not "this is a game". Only the shape filters plus the ignore list
         * decide, and uninstallers/support tools are dropped here too. */
        if (LooksLikeHelper(exe)) continue;
        if (LooksLikeNonGameApp(exe, exe)) continue;
        if (GamesIsIgnored(exe)) continue;
        if (GamesContains(exe)) continue;
        if (GamesAdd(exe)) {
            {
                std::lock_guard<std::mutex> lk(g_lock);
                for (size_t g = 0; g < g_games.size(); ++g) {
                    if (g_games[g].exe == exe) g_games[g].autoAdded = true;
                }
            }
            added.push_back(exe);
        }
    }
    if (!added.empty()) GamesSave();
    return added;
}
std::wstring GamesExeOfProcess(unsigned long pid)
{
    std::wstring result;
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, (DWORD)pid);
    if (!h) return result;

    wchar_t path[MAX_PATH * 2];
    DWORD len = (DWORD)(sizeof(path) / sizeof(path[0]));
    if (QueryFullProcessImageNameW(h, 0, path, &len) && len > 0) {
        result = Lower(BaseName(path));
    }
    CloseHandle(h);
    return result;
}

std::wstring GamesExeOfWindow(unsigned long long hwnd)
{
    DWORD pid = 0;
    GetWindowThreadProcessId((HWND)(uintptr_t)hwnd, &pid);
    if (!pid) return std::wstring();
    return GamesExeOfProcess(pid);
}

std::vector<std::wstring> GamesVisibleWindowExes()
{
    struct Ctx {
        std::set<std::wstring>* out;
    } ctx;
    std::set<std::wstring> found;
    ctx.out = &found;

    struct Helper {
        static BOOL CALLBACK Proc(HWND hwnd, LPARAM lp)
        {
            Ctx* c = (Ctx*)lp;
            if (!IsWindowVisible(hwnd)) return TRUE;
            if (GetWindow(hwnd, GW_OWNER) != NULL) return TRUE;
            wchar_t title[8] = {0};
            if (GetWindowTextW(hwnd, title, 8) == 0) return TRUE;
            DWORD pid = 0;
            GetWindowThreadProcessId(hwnd, &pid);
            if (!pid) return TRUE;
            const std::wstring exe = GamesExeOfProcess((unsigned long)pid);
            if (!exe.empty()) c->out->insert(exe);
            return TRUE;
        }
    };
    EnumWindows(Helper::Proc, (LPARAM)&ctx);

    std::vector<std::wstring> result(found.begin(), found.end());
    return result;
}

} // namespace ts3ovl

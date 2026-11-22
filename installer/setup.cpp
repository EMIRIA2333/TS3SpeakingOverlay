// TS3 Speaking Overlay installer
// Single self-contained EXE: embeds the plugin DLL as a resource, finds the
// running/installed TeamSpeak 3 client, installs the DLL and registers it in
// addons.ini, and can uninstall again.
//
// Build: see build-installer.ps1

#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>
#include <commctrl.h>
#include <tlhelp32.h>
#include <shlobj.h>
#include <shellapi.h>

#include <string>
#include <cwctype>
#include <vector>

#include "res.h"

// ---------------------------------------------------------------- constants

static const wchar_t* const kAppTitle = L"TS3 Speaking Overlay 安装程序";
static const wchar_t* const kDllName = L"ts3_speaking_overlay_win64.dll";
static const wchar_t* const kAddonKey = L"SpeakingOverlay";
static const wchar_t* const kWindowClass = L"TS3OvlSetupWnd";

// Installer build id, bumped by the build script.
#ifndef TS3OVL_SETUP_BUILD
#define TS3OVL_SETUP_BUILD "dev"
#endif

// ------------------------------------------------------------------- helpers

static void AppendLog(HWND logEdit, const std::wstring& line) {
    int len = GetWindowTextLengthW(logEdit);
    SendMessageW(logEdit, EM_SETSEL, (WPARAM)len, (LPARAM)len);
    std::wstring text = line + L"\r\n";
    SendMessageW(logEdit, EM_REPLACESEL, FALSE, (LPARAM)text.c_str());
    SendMessageW(logEdit, EM_SCROLLCARET, 0, 0);
    UpdateWindow(logEdit);
}

static void SetStatus(HWND status, const std::wstring& text, COLORREF color) {
    SetWindowTextW(status, text.c_str());
    InvalidateRect(status, NULL, TRUE);
    UpdateWindow(status);
    (void)color;
}

static bool FileExists(const std::wstring& path) {
    DWORD attr = GetFileAttributesW(path.c_str());
    return attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY);
}

static bool DirExists(const std::wstring& path) {
    DWORD attr = GetFileAttributesW(path.c_str());
    return attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY);
}

static std::wstring JoinPath(const std::wstring& a, const std::wstring& b) {
    if (a.empty()) return b;
    wchar_t last = a[a.size() - 1];
    if (last == L'\\' || last == L'/') return a + b;
    return a + L"\\" + b;
}

static std::wstring ExeDir() {
    wchar_t buf[MAX_PATH * 2];
    DWORD n = GetModuleFileNameW(NULL, buf, (DWORD)(sizeof(buf) / sizeof(buf[0])));
    std::wstring full(buf, n);
    size_t pos = full.find_last_of(L"\\/");
    return (pos == std::wstring::npos) ? std::wstring() : full.substr(0, pos);
}

// ------------------------------------------------------------- TS3 discovery

struct Ts3Location {
    std::wstring exePath;     // ...\ts3client_win64.exe or ...\ts3client.exe
    std::wstring configDir;   // ...\config  (contains plugins\ and addons.ini)
    std::wstring pluginsDir;  // ...\config\plugins
    std::wstring addonsIni;   // ...\config\addons.ini
    std::wstring foundBy;     // how we located it, for the log
    bool is64 = true;         // which plugin bitness this client needs
    bool valid = false;
};

// The two builds exist because 64-bit TS3 (3.6.x) needs API 26 while the 32-bit
// client (<= 3.5.x) needs API 25 and rejects anything newer.  Each client uses
// its own plugins directory, so both DLLs can be installed side by side.
static const wchar_t* const kDllName64 = L"ts3_speaking_overlay_win64.dll";
static const wchar_t* const kDllName32 = L"ts3_speaking_overlay_win32.dll";

static const wchar_t* DllNameFor(bool is64) { return is64 ? kDllName64 : kDllName32; }

// Decide the bitness from the client exe name, falling back to reading the PE
// header (a portable copy may have been renamed).
static bool ClientIs64Bit(const std::wstring& exePath) {
    std::wstring lower = exePath;
    for (size_t i = 0; i < lower.size(); ++i) lower[i] = towlower(lower[i]);
    if (lower.size() >= 8 && lower.compare(lower.size() - 8, 8, L"win64.exe") == 0) return true;
    if (lower.size() >= 4 && lower.compare(lower.size() - 4, 4, L".exe") == 0 &&
        lower.find(L"win32") != std::wstring::npos) {
        return false;
    }

    HANDLE h = CreateFileW(exePath.c_str(), GENERIC_READ,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return true;   // assume 64-bit, the common case
    bool is64 = true;
    IMAGE_DOS_HEADER dos;
    DWORD read = 0;
    if (ReadFile(h, &dos, sizeof(dos), &read, NULL) && read == sizeof(dos) &&
        dos.e_magic == IMAGE_DOS_SIGNATURE) {
        if (SetFilePointer(h, dos.e_lfanew, NULL, FILE_BEGIN) != INVALID_SET_FILE_POINTER) {
            DWORD sig = 0;
            if (ReadFile(h, &sig, sizeof(sig), &read, NULL) && read == sizeof(sig) &&
                sig == IMAGE_NT_SIGNATURE) {
                WORD machine = 0;
                if (ReadFile(h, &machine, sizeof(machine), &read, NULL) && read == sizeof(machine)) {
                    is64 = (machine == IMAGE_FILE_MACHINE_AMD64);
                }
            }
        }
    }
    CloseHandle(h);
    return is64;
}

static void FinishLocation(Ts3Location& loc, const std::wstring& exePath,
                           const std::wstring& configDir, const std::wstring& foundBy) {
    loc.exePath = exePath;
    loc.configDir = configDir;
    loc.pluginsDir = JoinPath(configDir, L"plugins");
    loc.addonsIni = JoinPath(configDir, L"addons.ini");
    loc.foundBy = foundBy;
    // Which DLL to install depends on the client's bitness: the 64-bit client
    // wants API 26, the 32-bit client wants API 25. Fall back to 64-bit when the
    // exe cannot be inspected (the filename usually says win64 anyway).
    loc.is64 = exePath.empty() ? true : ClientIs64Bit(exePath);
    // A config dir that does not exist yet still counts: DoInstall creates it.
    loc.valid = !configDir.empty();
}

// Read the install dir out of HKCU\Software\TeamSpeak 3 Client (default value).
// TS3 portable installs register their own directory there.
static bool ConfigDirFromRegistry(std::wstring* exeDirOut) {
    HKEY key = NULL;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\TeamSpeak 3 Client", 0,
                      KEY_READ, &key) != ERROR_SUCCESS) {
        return false;
    }
    wchar_t buf[MAX_PATH * 2];
    DWORD size = (DWORD)sizeof(buf);
    DWORD type = 0;
    LONG rc = RegQueryValueExW(key, NULL, NULL, &type, (LPBYTE)buf, &size);
    RegCloseKey(key);
    if (rc != ERROR_SUCCESS || (type != REG_SZ && type != REG_EXPAND_SZ) || size < 2) {
        return false;
    }
    std::wstring dir(buf);
    while (!dir.empty() && (dir[dir.size() - 1] == L'\\' || dir[dir.size() - 1] == L'/')) {
        dir.erase(dir.size() - 1);
    }
    if (!dir.empty()) {
        *exeDirOut = dir;
        return true;
    }
    return false;
}

// Locate a running TeamSpeak 3 client and derive its config dir.
static bool RunningTs3(Ts3Location& loc) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return false;
    PROCESSENTRY32W pe;
    ZeroMemory(&pe, sizeof(pe));
    pe.dwSize = sizeof(pe);
    std::wstring foundExe;
    if (Process32FirstW(snap, &pe)) {
        do {
            std::wstring name = pe.szExeFile;
            if (_wcsicmp(name.c_str(), L"ts3client_win64.exe") == 0 ||
                _wcsicmp(name.c_str(), L"ts3client.exe") == 0) {
                HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pe.th32ProcessID);
                if (h) {
                    wchar_t path[MAX_PATH * 2];
                    DWORD n = (DWORD)(sizeof(path) / sizeof(path[0]));
                    if (QueryFullProcessImageNameW(h, 0, path, &n)) {
                        foundExe.assign(path, n);
                    }
                    CloseHandle(h);
                }
                if (!foundExe.empty()) break;
            }
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    if (foundExe.empty()) return false;
    size_t pos = foundExe.find_last_of(L"\\/");
    std::wstring dir = (pos == std::wstring::npos) ? std::wstring() : foundExe.substr(0, pos);
    if (dir.empty()) return false;
    std::wstring configDir = JoinPath(dir, L"config");
    if (!DirExists(configDir)) {
        // Some installs keep the config in %APPDATA%\TS3Client.
        wchar_t appdata[MAX_PATH * 2];
        if (SHGetFolderPathW(NULL, CSIDL_APPDATA, NULL, 0, appdata) == S_OK) {
            std::wstring cfg = JoinPath(appdata, L"TS3Client");
            if (DirExists(cfg)) {
                FinishLocation(loc, foundExe, cfg, L"运行中的 TeamSpeak 3 进程（配置在 %APPDATA%）");
                return true;
            }
        }
        // Use the portable location anyway; DoInstall can create it.
        FinishLocation(loc, foundExe, configDir, L"运行中的 TeamSpeak 3 进程");
        return true;
    }
    FinishLocation(loc, foundExe, configDir, L"运行中的 TeamSpeak 3 进程");
    return true;
}

// Diagnostic text shown in the log when discovery fails, so the user can tell
// "TS3 simply is not running" from "this program is not allowed to look".
static std::wstring DiscoveryDiagnostics() {
    std::wstring s;
    wchar_t regBuf[MAX_PATH * 2];
    DWORD size = (DWORD)sizeof(regBuf);
    regBuf[0] = 0;
    HKEY key = NULL;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\TeamSpeak 3 Client", 0,
                      KEY_READ, &key) == ERROR_SUCCESS) {
        DWORD type = 0;
        if (RegQueryValueExW(key, NULL, NULL, &type, (LPBYTE)regBuf, &size) == ERROR_SUCCESS &&
            regBuf[0]) {
            s += std::wstring(L"  · 注册表 HKCU\\Software\\TeamSpeak 3 Client = ") + regBuf + L"\n";
        } else {
            s += L"  · 注册表 HKCU\\Software\\TeamSpeak 3 Client 没有默认值\n";
        }
        RegCloseKey(key);
    } else {
        s += L"  · 读不到 HKCU\\Software\\TeamSpeak 3 Client\n";
    }
    // Can we even see the process list? Low integrity callers see an empty list.
    DWORD procs = 0;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap != INVALID_HANDLE_VALUE) {
        PROCESSENTRY32W pe;
        ZeroMemory(&pe, sizeof(pe));
        pe.dwSize = sizeof(pe);
        if (Process32FirstW(snap, &pe)) {
            do { ++procs; } while (Process32NextW(snap, &pe));
        }
        CloseHandle(snap);
    }
    if (procs < 8) {
        s += L"  · 只能看到 " + std::to_wstring(procs) +
             L" 个进程 —— 本程序运行在低完整性级别下，看不到 TeamSpeak 3，也写不进它的目录。\n";
        s += L"  · 处理方法：把这个 EXE 复制到一个普通文件夹（例如桌面）再运行，\n";
        s += L"    或对该文件夹执行 icacls <目录> /setintegritylevel \"(OI)(CI)Medium\" /T\n";
    } else {
        s += L"  · 进程列表可见（共 " + std::to_wstring(procs) + L" 个），其中没有 ts3client*.exe\n";
    }
    return s;
}

// Append a directory that looks like a TS3 install root: it directly contains
// a ts3client*.exe, or a config sub directory.
static void PushIfTs3Dir(const std::wstring& dir, std::vector<std::wstring>* roots) {
    if (dir.empty()) return;
    if (FileExists(JoinPath(dir, L"ts3client_win64.exe")) ||
        FileExists(JoinPath(dir, L"ts3client.exe")) ||
        DirExists(JoinPath(dir, L"config"))) {
        for (size_t i = 0; i < roots->size(); ++i) {
            if (_wcsicmp((*roots)[i].c_str(), dir.c_str()) == 0) return;
        }
        roots->push_back(dir);
    }
}

// Breadth-first, depth-limited directory walk looking for ts3client*.exe.
// Covers installs the fixed candidate list misses (custom folders, other names)
// without degenerating into a whole-disk search.
static void ScanForTs3(const std::wstring& startDir, int maxDepth,
                       std::vector<std::wstring>& roots) {
    if (startDir.empty() || !DirExists(startDir)) return;
    std::vector<std::wstring> level;
    level.push_back(startDir);
    for (int depth = 0; depth <= maxDepth && !level.empty(); ++depth) {
        std::vector<std::wstring> next;
        for (size_t i = 0; i < level.size(); ++i) {
            const std::wstring& dir = level[i];
            PushIfTs3Dir(dir, &roots);
            if (depth == maxDepth) continue;
            WIN32_FIND_DATAW fd;
            HANDLE h = FindFirstFileW(JoinPath(dir, L"*").c_str(), &fd);
            if (h == INVALID_HANDLE_VALUE) continue;
            do {
                if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
                    if (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) continue;
                    continue;
                }
                if (fd.cFileName[0] == L'.') continue;
                if (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) continue;
                next.push_back(JoinPath(dir, fd.cFileName));
            } while (FindNextFileW(h, &fd));
            FindClose(h);
        }
        level.swap(next);
    }
}

static void CollectTs3InstallRoots(std::vector<std::wstring>& roots) {
    roots.clear();
    // A) registry: HKCU\Software\TeamSpeak 3 Client\(default)
    std::wstring regDir;
    if (ConfigDirFromRegistry(&regDir)) PushIfTs3Dir(regDir, &roots);
    // B) HKCU:\Software\TeamSpeak 3 Client\ConfigLocation hold the config root
    //    on portable installs; D then covers the usual folders.
    // C) every fixed drive, a few common directories
    static const wchar_t* const kSubDirs[] = {
        L"teamspeak", L"TeamSpeak", L"teamspeak3", L"TeamSpeak3", L"ts3",
        L"Program Files\\TeamSpeak 3 Client",
        L"Program Files (x86)\\TeamSpeak 3 Client",
        L"Program Files\\TeamSpeak", L"Program Files (x86)\\TeamSpeak",
        L"Games\\TeamSpeak 3 Client",
    };
    wchar_t drives[512];
    DWORD n = GetLogicalDriveStringsW((DWORD)(sizeof(drives) / sizeof(drives[0])), drives);
    if (n > 0) {
        const wchar_t* p = drives;
        while (*p) {
            std::wstring drive(p);
            if (GetDriveTypeW(drive.c_str()) == DRIVE_FIXED) {
                // uninstall information is a very reliable hint on Windows
                PushIfTs3Dir(JoinPath(drive, L"Program Files\\TeamSpeak 3 Client"), &roots);
                for (size_t i = 0; i < sizeof(kSubDirs) / sizeof(kSubDirs[0]); ++i) {
                    PushIfTs3Dir(JoinPath(drive, kSubDirs[i]), &roots);
                }
                ScanForTs3(drive, 2, roots);
            }
            p += wcslen(p) + 1;
        }
    }
}

// Given a candidate directory, figure out where config\addons.ini lives.
// Accepts either the install root, the config folder, or the plugins folder,
// because that is what a user is most likely to pick in the browse dialog.
static bool LocationFromRoot(const std::wstring& root, Ts3Location& loc) {
    // 0) the user picked the config folder itself, or config\plugins
    std::wstring base = root;
    if (DirExists(JoinPath(base, L"plugins"))) {
        // <...>\config
        std::wstring exe = JoinPath(base, L"ts3client_win64.exe");
        if (!FileExists(exe)) exe = JoinPath(base, L"ts3client.exe");
        FinishLocation(loc, exe, base, L"手动选择：" + base);
        return loc.valid;
    }
    if (DirExists(base) && DirExists(JoinPath(JoinPath(base, L".."), L"plugins")) &&
        FileExists(JoinPath(base, L"addons.ini"))) {
        // <...>\config\plugins was picked, config is the parent
        std::wstring parent = base.substr(0, base.find_last_of(L"\\/"));
        if (!parent.empty()) {
            std::wstring exe = JoinPath(parent, L"ts3client_win64.exe");
            if (!FileExists(exe)) exe = JoinPath(parent, L"ts3client.exe");
            FinishLocation(loc, exe, parent, L"手动选择：" + parent);
            return loc.valid;
        }
    }

    // 1) portable-style: <root>\config\addons.ini  (the layout this project uses)
    std::wstring portableConfig = JoinPath(root, L"config");
    if (DirExists(portableConfig)) {
        std::wstring exe = JoinPath(root, L"ts3client_win64.exe");
        if (!FileExists(exe)) exe = JoinPath(root, L"ts3client.exe");
        FinishLocation(loc, exe, portableConfig, L"目录扫描：" + root);
        return loc.valid;
    }
    // 2) normal install: config lives in %APPDATA%\TS3Client, addons.ini there
    std::wstring exe = JoinPath(root, L"ts3client_win64.exe");
    if (!FileExists(exe)) exe = JoinPath(root, L"ts3client.exe");
    if (FileExists(exe)) {
        wchar_t appdata[MAX_PATH * 2];
        if (SHGetFolderPathW(NULL, CSIDL_APPDATA, NULL, 0, appdata) == S_OK) {
            std::wstring cfg = JoinPath(appdata, L"TS3Client");
            if (DirExists(cfg)) {
                FinishLocation(loc, exe, cfg, L"目录扫描：" + root + L"（配置在 %APPDATA%）");
                return loc.valid;
            }
        }
        // Last resort: create the config next to the exe (portable layout).
        FinishLocation(loc, exe, portableConfig,
                       L"目录扫描：" + root + L"（将使用 " + portableConfig + L"）");
        loc.valid = true;  // dir may not exist yet; DoInstall creates it
        return true;
    }
    loc.valid = false;
    return false;
}

static bool FindTs3(Ts3Location& loc) {
    loc = Ts3Location();
    if (RunningTs3(loc)) return true;

    // 1) addons.ini of a known install
    std::vector<std::wstring> roots;
    CollectTs3InstallRoots(roots);
    for (size_t i = 0; i < roots.size(); ++i) {
        Ts3Location cand;
        if (LocationFromRoot(roots[i], cand)) {
            loc = cand;
            return true;
        }
    }
    return false;
}

// Let the user point at the TS3 folder manually (must contain config\addons.ini
// or ts3client_win64.exe).
static bool BrowseForTs3(HWND owner, Ts3Location& loc) {
    BROWSEINFOW bi;
    ZeroMemory(&bi, sizeof(bi));
    bi.hwndOwner = owner;
    bi.lpszTitle = L"请选择 TeamSpeak 3 的安装目录（里面有 config 文件夹或 ts3client_win64.exe）";
    bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE | BIF_USENEWUI;
    LPITEMIDLIST pidl = SHBrowseForFolderW(&bi);
    if (!pidl) return false;
    wchar_t path[MAX_PATH * 2];
    bool ok = SHGetPathFromIDListW(pidl, path) != FALSE;
    CoTaskMemFree(pidl);
    if (!ok) return false;
    return LocationFromRoot(path, loc);
}

// ------------------------------------------------------------ addons.ini I/O

static bool ReadFileUtf8(const std::wstring& path, std::string* out) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size;
    if (!GetFileSizeEx(h, &size)) { CloseHandle(h); return false; }
    out->clear();
    if (size.QuadPart > 0) {
        out->resize((size_t)size.QuadPart);
        DWORD read = 0;
        if (!ReadFile(h, &(*out)[0], (DWORD)out->size(), &read, NULL)) {
            CloseHandle(h);
            return false;
        }
        out->resize(read);
    }
    CloseHandle(h);
    return true;
}

static bool WriteFileBytes(const std::wstring& path, const std::string& data) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    bool ok = data.empty() ||
              (WriteFile(h, data.data(), (DWORD)data.size(), &written, NULL) && written == data.size());
    CloseHandle(h);
    return ok;
}

static bool BackupFile(const std::wstring& path, const std::wstring& backupPath) {
    if (!FileExists(path)) return false;
    return CopyFileW(path.c_str(), backupPath.c_str(), FALSE) != FALSE;
}

// Is our key already present in the [plugin] section?
static bool AddonRegistered(const std::string& ini) {
    std::string needle = std::string("SpeakingOverlay\\files=");
    return ini.find(needle) != std::string::npos;
}

// Insert "<key>\files=<dll>\r\n" directly after the [plugin] header,
// removing any earlier occurrence of the same key.
static std::string RegisterAddon(const std::string& ini, const std::wstring& dllName, bool* changed) {
    const std::string key = "SpeakingOverlay\\files=";
    std::string out;
    out.reserve(ini.size() + 64);

    // Walk line by line, dropping stale SpeakingOverlay lines.
    size_t pos = 0;
    bool inserted = false;
    bool inPlugin = false;
    while (pos <= ini.size()) {
        size_t nl = ini.find('\n', pos);
        std::string line = (nl == std::string::npos) ? ini.substr(pos) : ini.substr(pos, nl - pos);
        size_t nextPos = (nl == std::string::npos) ? ini.size() + 1 : nl + 1;

        std::string trimmed = line;
        while (!trimmed.empty() && (trimmed[trimmed.size() - 1] == '\r' ||
                                    trimmed[trimmed.size() - 1] == ' ')) {
            trimmed.erase(trimmed.size() - 1);
        }
        if (trimmed.size() >= 2 && trimmed[0] == '[') inPlugin = (trimmed == "[plugin]");

        bool isStale = (trimmed.compare(0, key.size(), key) == 0);
        if (!isStale) {
            out += line;
            if (nl != std::string::npos) out += '\n';
        } else if (!inserted) {
            // ignore: we re-insert a fresh line below
        }

        if (!isStale && inPlugin && trimmed == "[plugin]" && !inserted) {
            out += "SpeakingOverlay\\files=";
            out += std::string(dllName.begin(), dllName.end());
            out += "\r\n";
            inserted = true;
        }
        if (nl == std::string::npos) break;
        pos = nextPos;
    }

    if (!inserted) {
        // No [plugin] section: append one.
        if (!out.empty() && out[out.size() - 1] != '\n') out += "\r\n";
        out += "[plugin]\r\n";
        out += "SpeakingOverlay\\files=";
        out += std::string(dllName.begin(), dllName.end());
        out += "\r\n";
        inserted = true;
    }

    if (changed) *changed = (out != ini);
    return out;
}

// Remove every SpeakingOverlay line from addons.ini.
static std::string UnregisterAddon(const std::string& ini, bool* changed) {
    const std::string key = "SpeakingOverlay\\files=";
    std::string out;
    out.reserve(ini.size());
    size_t pos = 0;
    bool removedAny = false;
    while (pos <= ini.size()) {
        size_t nl = ini.find('\n', pos);
        std::string line = (nl == std::string::npos) ? ini.substr(pos) : ini.substr(pos, nl - pos);
        std::string trimmed = line;
        while (!trimmed.empty() && (trimmed[trimmed.size() - 1] == '\r' ||
                                    trimmed[trimmed.size() - 1] == ' ')) {
            trimmed.erase(trimmed.size() - 1);
        }
        if (trimmed.compare(0, key.size(), key) == 0) {
            removedAny = true;
        } else {
            out += line;
            if (nl != std::string::npos) out += '\n';
        }
        if (nl == std::string::npos) break;
        pos = nl + 1;
    }
    if (changed) *changed = removedAny;
    return out;
}

// ------------------------------------------------------------- TS3 processes

static bool Ts3Running() {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return false;
    PROCESSENTRY32W pe;
    ZeroMemory(&pe, sizeof(pe));
    pe.dwSize = sizeof(pe);
    bool running = false;
    if (Process32FirstW(snap, &pe)) {
        do {
            if (_wcsicmp(pe.szExeFile, L"ts3client_win64.exe") == 0 ||
                _wcsicmp(pe.szExeFile, L"ts3client.exe") == 0) {
                running = true;
                break;
            }
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    return running;
}

static bool StopTs3(std::wstring* err) {
    if (!Ts3Running()) return true;
    // Ask nicely first, then force.
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) { if (err) *err = L"无法枚举进程"; return false; }
    std::vector<DWORD> pids;
    PROCESSENTRY32W pe;
    ZeroMemory(&pe, sizeof(pe));
    pe.dwSize = sizeof(pe);
    if (Process32FirstW(snap, &pe)) {
        do {
            if (_wcsicmp(pe.szExeFile, L"ts3client_win64.exe") == 0 ||
                _wcsicmp(pe.szExeFile, L"ts3client.exe") == 0) {
                pids.push_back(pe.th32ProcessID);
            }
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);

    for (size_t i = 0; i < pids.size(); ++i) {
        HANDLE h = OpenProcess(PROCESS_TERMINATE, FALSE, pids[i]);
        if (h) { TerminateProcess(h, 0); CloseHandle(h); }
    }
    for (int i = 0; i < 60; ++i) {  // up to ~6s
        if (!Ts3Running()) return true;
        Sleep(100);
    }
    if (err) *err = L"TeamSpeak 3 仍在运行，请手动关闭后再试";
    return false;
}

static bool StartTs3(const std::wstring& exePath, std::wstring* err) {
    if (exePath.empty() || !FileExists(exePath)) {
        if (err) *err = L"找不到 ts3client_win64.exe（已跳过自动启动）";
        return false;
    }
    size_t pos = exePath.find_last_of(L"\\/");
    std::wstring workDir = (pos == std::wstring::npos) ? std::wstring() : exePath.substr(0, pos);
    HINSTANCE rc = ShellExecuteW(NULL, L"open", exePath.c_str(), NULL,
                                workDir.empty() ? NULL : workDir.c_str(), SW_SHOWNORMAL);
    if ((INT_PTR)rc <= 32) {
        if (err) *err = L"启动 TeamSpeak 3 失败";
        return false;
    }
    return true;
}

// --------------------------------------------------------------- install job

struct Action {
    enum Kind { kInstall, kUninstall } kind = kInstall;
    Ts3Location loc;
};

struct ActionResult {
    bool ok = false;
    std::wstring message;
};

static std::wstring TempPayloadPath(const std::wstring& dllName) {
    wchar_t temp[MAX_PATH * 2];
    DWORD n = GetTempPathW((DWORD)(sizeof(temp) / sizeof(temp[0])), temp);
    if (n == 0) return std::wstring();
    std::wstring dir(temp, n);
    return JoinPath(dir, L"ts3ovl_payload_" + dllName);
}

// Drop the embedded DLL next to the installer on first use, or use an already
// present one so people can swap in a custom build.
static bool ExtractPayload(const std::wstring& dstPath, bool is64, std::wstring* err) {
    // Pick the payload matching the installed client's bitness: the 64-bit
    // client needs API 26, the 32-bit client needs API 25.
    int resId = is64 ? IDR_PAYLOAD_DLL : IDR_PAYLOAD_DLL32;
    HRSRC res = FindResourceW(NULL, MAKEINTRESOURCEW(resId), RT_RCDATA);
    if (!res && is64) {
        // Fall back to the other one if only a single payload was embedded.
        res = FindResourceW(NULL, MAKEINTRESOURCEW(IDR_PAYLOAD_DLL32), RT_RCDATA);
    }
    if (!res && !is64) {
        res = FindResourceW(NULL, MAKEINTRESOURCEW(IDR_PAYLOAD_DLL), RT_RCDATA);
    }
    if (!res) {
        if (err) *err = L"安装包损坏：找不到内嵌的插件 DLL";
        return false;
    }
    HGLOBAL glob = LoadResource(NULL, res);
    if (!glob) { if (err) *err = L"安装包损坏：无法读取插件 DLL"; return false; }
    DWORD size = SizeofResource(NULL, res);
    void* data = LockResource(glob);
    if (!data || size == 0) { if (err) *err = L"安装包损坏：插件 DLL 为空"; return false; }

    HANDLE h = CreateFileW(dstPath.c_str(), GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        if (err) *err = L"无法写入临时文件：" + dstPath;
        return false;
    }
    DWORD written = 0;
    bool ok = WriteFile(h, data, size, &written, NULL) && written == size;
    CloseHandle(h);
    if (!ok) {
        DeleteFileW(dstPath.c_str());
        if (err) *err = L"写入插件 DLL 失败";
        return false;
    }
    return true;
}

static ActionResult DoInstall(HWND logEdit, HWND status, const std::wstring& setupDir) {
    ActionResult r;
    Ts3Location loc;
    AppendLog(logEdit, L"[1/6] 查找 TeamSpeak 3 ...");
    if (!FindTs3(loc)) {
        SetStatus(status, L"未找到 TeamSpeak 3", 0);
        AppendLog(logEdit, L"  失败：没有自动找到 TeamSpeak 3。探测结果：");
        {
            std::wstring diag = DiscoveryDiagnostics();
            // AppendLog takes one line at a time; split on newlines.
            size_t start = 0;
            while (start < diag.size()) {
                size_t nl = diag.find(L'\n', start);
                std::wstring line = (nl == std::wstring::npos)
                                        ? diag.substr(start)
                                        : diag.substr(start, nl - start);
                if (!line.empty()) AppendLog(logEdit, line);
                if (nl == std::wstring::npos) break;
                start = nl + 1;
            }
        }
        AppendLog(logEdit, L"  → 点下面的“手动选择目录…”指定 TeamSpeak 3 文件夹。");
        r.message = L"未找到 TeamSpeak 3";
        return r;
    }
    AppendLog(logEdit, L"  找到：" + loc.foundBy);
    AppendLog(logEdit, L"  配置目录：" + loc.configDir);

    // Pick the plugin build that matches this client's bitness and say so.
    const std::wstring dllName = DllNameFor(loc.is64);
    AppendLog(logEdit, std::wstring(L"  客户端：") + (loc.is64 ? L"64 位（API 26）" : L"32 位（API 25）"));
    AppendLog(logEdit, L"  将安装：" + dllName);

    AppendLog(logEdit, L"[2/6] 关闭 TeamSpeak 3 ...");
    std::wstring err;
    if (!StopTs3(&err)) {
        SetStatus(status, L"无法关闭 TeamSpeak 3", 0);
        AppendLog(logEdit, L"  失败：" + err);
        r.message = err;
        return r;
    }
    AppendLog(logEdit, L"  已关闭（或本来就没运行）");

    AppendLog(logEdit, L"[3/6] 准备插件文件 ...");
    if (!DirExists(loc.pluginsDir)) {
        if (!CreateDirectoryW(loc.pluginsDir.c_str(), NULL) &&
            GetLastError() != ERROR_ALREADY_EXISTS) {
            err = L"无法创建插件目录：" + loc.pluginsDir;
            SetStatus(status, L"安装失败", 0);
            AppendLog(logEdit, L"  失败：" + err);
            r.message = err;
            return r;
        }
    }
    std::wstring payload = TempPayloadPath(dllName);
    if (!ExtractPayload(payload, loc.is64, &err)) {
        SetStatus(status, L"安装失败", 0);
        AppendLog(logEdit, L"  失败：" + err);
        r.message = err;
        return r;
    }
    std::wstring target = JoinPath(loc.pluginsDir, dllName);
    if (!CopyFileW(payload.c_str(), target.c_str(), FALSE)) {
        DWORD e = GetLastError();
        err = L"复制插件 DLL 失败（错误 " + std::to_wstring(e) + L"）：" + target;
        if (e == ERROR_ACCESS_DENIED || e == ERROR_SHARING_VIOLATION) {
            err += L"\n  → 可能原因：TeamSpeak 3 还没完全退出，或者这个目录需要管理员权限。";
            err += L"\n  → 请确认 TS3 已关闭；如果 TS3 装在 Program Files，请右键本程序 →“以管理员身份运行”。";
        }
        SetStatus(status, L"安装失败", 0);
        // AppendLog is line oriented; split the multi-line message.
        size_t start = 0;
        while (start <= err.size()) {
            size_t nl = err.find(L'\n', start);
            std::wstring line = (nl == std::wstring::npos) ? err.substr(start)
                                                          : err.substr(start, nl - start);
            if (!line.empty()) AppendLog(logEdit, line);
            if (nl == std::wstring::npos) break;
            start = nl + 1;
        }
        r.message = L"复制插件 DLL 失败（错误 " + std::to_wstring(e) + L"）";
        return r;
    }
    DeleteFileW(payload.c_str());
    AppendLog(logEdit, L"  已安装：" + target);

    AppendLog(logEdit, L"[4/6] 注册到 addons.ini ...");
    std::string ini;
    if (!ReadFileUtf8(loc.addonsIni, &ini)) {
        AppendLog(logEdit, L"  addons.ini 不存在，将创建一个新的");
        ini.clear();
    }
    if (AddonRegistered(ini)) {
        AppendLog(logEdit, L"  已存在注册项，将重建");
    }
    std::string backup = std::string(loc.addonsIni.begin(), loc.addonsIni.end());
    backup += ".ts3ovl.bak";
    std::wstring backupW(backup.begin(), backup.end());
    if (BackupFile(loc.addonsIni, backupW)) {
        AppendLog(logEdit, L"  已备份原文件为 addons.ini.ts3ovl.bak");
    }
    bool changed = false;
    std::string updated = RegisterAddon(ini, dllName, &changed);
    if (!WriteFileBytes(loc.addonsIni, updated)) {
        err = L"写入 addons.ini 失败：" + loc.addonsIni;
        SetStatus(status, L"安装失败", 0);
        AppendLog(logEdit, L"  失败：" + err);
        r.message = err;
        return r;
    }
    AppendLog(logEdit, L"  已写入注册项 SpeakingOverlay\\files=" + dllName);

    AppendLog(logEdit, L"[5/6] 启动 TeamSpeak 3 ...");
    if (StartTs3(loc.exePath, &err)) {
        AppendLog(logEdit, L"  已启动：" + loc.exePath);
    } else {
        AppendLog(logEdit, L"  提示：" + err);
    }

    AppendLog(logEdit, L"[6/6] 完成");
    AppendLog(logEdit, L"  在 TS3 里打开 设置 → 附加组件，确认 \"Speaking Overlay\" 已启用。");
    AppendLog(logEdit, L"  插件菜单在顶部 插件(P) → Speaking Overlay。");
    (void)setupDir;
    SetStatus(status, L"安装完成", 0);
    r.ok = true;
    r.message = L"安装完成";
    return r;
}

static ActionResult DoUninstall(HWND logEdit, HWND status) {
    ActionResult r;
    Ts3Location loc;
    AppendLog(logEdit, L"[1/4] 查找 TeamSpeak 3 ...");
    if (!FindTs3(loc)) {
        SetStatus(status, L"未找到 TeamSpeak 3", 0);
        AppendLog(logEdit, L"  失败：没有找到 TeamSpeak 3。");
        r.message = L"未找到 TeamSpeak 3";
        return r;
    }
    AppendLog(logEdit, L"  配置目录：" + loc.configDir);

    AppendLog(logEdit, L"[2/4] 关闭 TeamSpeak 3 ...");
    std::wstring err;
    if (!StopTs3(&err)) {
        SetStatus(status, L"无法关闭 TeamSpeak 3", 0);
        AppendLog(logEdit, L"  失败：" + err);
        r.message = err;
        return r;
    }

    AppendLog(logEdit, L"[3/4] 删除插件文件并清理注册项 ...");
    /* Remove both bitness variants so an uninstall is complete even when the
     * client was upgraded/reinstalled between install and uninstall. */
    const std::wstring names[2] = {kDllName64, kDllName32};
    for (int i = 0; i < 2; ++i) {
        std::wstring target = JoinPath(loc.pluginsDir, names[i]);
        if (FileExists(target)) {
            if (DeleteFileW(target.c_str())) {
                AppendLog(logEdit, L"  已删除：" + names[i]);
            } else {
                AppendLog(logEdit, L"  警告：删除失败 " + names[i] + L"（错误 " +
                                       std::to_wstring(GetLastError()) + L"）");
            }
        }
    }
    std::string ini;
    if (ReadFileUtf8(loc.addonsIni, &ini)) {
        bool changed = false;
        std::string updated = UnregisterAddon(ini, &changed);
        if (changed && WriteFileBytes(loc.addonsIni, updated)) {
            AppendLog(logEdit, L"  已从 addons.ini 移除注册项");
        } else if (!changed) {
            AppendLog(logEdit, L"  addons.ini 里没有注册项");
        }
    }

    AppendLog(logEdit, L"[4/4] 完成（TeamSpeak 3 已保持关闭）");
    SetStatus(status, L"卸载完成", 0);
    r.ok = true;
    r.message = L"卸载完成";
    return r;
}

// ------------------------------------------------------------------- window

struct UiState {
    HWND log = NULL;
    HWND status = NULL;
    HWND install = NULL;
    HWND uninstall = NULL;
    HWND close = NULL;
    HWND restart = NULL;
    HWND browse = NULL;
    HFONT font = NULL;
    HFONT statusFont = NULL;
    bool found = false;
    Ts3Location loc;
};

static UiState g_ui;

static void Layout(HWND hwnd) {
    RECT rc;
    GetClientRect(hwnd, &rc);
    int W = rc.right - rc.left;
    int H = rc.bottom - rc.top;
    int pad = 16;
    int btnH = 32;
    int statusH = 26;

    MoveWindow(g_ui.status, pad, pad, W - 2 * pad, statusH, TRUE);
    MoveWindow(g_ui.log, pad, pad + statusH + 8, W - 2 * pad, H - pad - statusH - 8 - btnH - 12 - pad, TRUE);
    int y = H - pad - btnH;
    int x = pad;
    MoveWindow(g_ui.install, x, y, 96, btnH, TRUE); x += 96 + 8;
    MoveWindow(g_ui.uninstall, x, y, 72, btnH, TRUE); x += 72 + 8;
    MoveWindow(g_ui.browse, x, y, 140, btnH, TRUE); x += 140 + 8;
    MoveWindow(g_ui.restart, x, y, 120, btnH, TRUE);
    MoveWindow(g_ui.close, W - pad - 88, y, 88, btnH, TRUE);
}

static void ShowFoundState() {
    std::wstring msg = L"已找到 TeamSpeak 3：";
    if (g_ui.found) {
        msg += g_ui.loc.configDir;
    } else {
        msg = L"没有自动找到 TeamSpeak 3 —— 点“手动选择目录…”指定";
    }
    SetWindowTextW(g_ui.status, msg.c_str());
}

static void DoFindAndLog() {
    g_ui.found = FindTs3(g_ui.loc);
    if (g_ui.found) {
        AppendLog(g_ui.log, L"已找到 TeamSpeak 3（" + g_ui.loc.foundBy + L"）");
        AppendLog(g_ui.log, L"  程序：" + g_ui.loc.exePath);
        AppendLog(g_ui.log, L"  配置：" + g_ui.loc.configDir);
        AppendLog(g_ui.log, L"");
        AppendLog(g_ui.log, L"点击“安装”即可开始。");
    } else {
        AppendLog(g_ui.log, L"没有自动找到 TeamSpeak 3。");
        std::wstring diag = DiscoveryDiagnostics();
        size_t start = 0;
        while (start < diag.size()) {
            size_t nl = diag.find(L'\n', start);
            std::wstring line = (nl == std::wstring::npos) ? diag.substr(start)
                                                           : diag.substr(start, nl - start);
            if (!line.empty()) AppendLog(g_ui.log, line);
            if (nl == std::wstring::npos) break;
            start = nl + 1;
        }
        AppendLog(g_ui.log, L"请点下面的“手动选择目录…”按钮指定 TS3 文件夹。");
    }
    ShowFoundState();
}

// ---------------------------------------------------------------- entry point

static void CommonInit(HWND hwnd) {
    HINSTANCE inst = (HINSTANCE)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    (void)inst;

    HDC dc = GetDC(hwnd);
    LOGFONTW lf;
    ZeroMemory(&lf, sizeof(lf));
    lf.lfHeight = -MulDiv(9, GetDeviceCaps(dc, LOGPIXELSY), 72);
    lf.lfWeight = FW_NORMAL;
    lf.lfCharSet = DEFAULT_CHARSET;
    lf.lfQuality = CLEARTYPE_QUALITY;
    wcscpy(lf.lfFaceName, L"Microsoft YaHei UI");
    g_ui.font = CreateFontIndirectW(&lf);

    LOGFONTW lfs = lf;
    lfs.lfHeight = -MulDiv(10, GetDeviceCaps(dc, LOGPIXELSY), 72);
    lfs.lfWeight = FW_SEMIBOLD;
    g_ui.statusFont = CreateFontIndirectW(&lfs);
    ReleaseDC(hwnd, dc);

    RECT rc;
    GetClientRect(hwnd, &rc);
    int W = rc.right - rc.left;

    g_ui.status = CreateWindowExW(0, L"STATIC", L"正在查找 TeamSpeak 3 ...",
                                  WS_CHILD | WS_VISIBLE | SS_LEFT | SS_CENTERIMAGE,
                                  0, 0, W, 26, hwnd, (HMENU)IDC_STATUS, NULL, NULL);
    g_ui.log = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                               WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE |
                               ES_AUTOVSCROLL | ES_READONLY | ES_LEFT,
                               0, 0, 0, 0, hwnd, (HMENU)IDC_LOG, NULL, NULL);
    g_ui.install = CreateWindowExW(0, L"BUTTON", L"安装",
                                   WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
                                   0, 0, 0, 0, hwnd, (HMENU)IDC_INSTALL, NULL, NULL);
    g_ui.uninstall = CreateWindowExW(0, L"BUTTON", L"卸载",
                                     WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
                                     0, 0, 0, 0, hwnd, (HMENU)IDC_UNINSTALL, NULL, NULL);
    g_ui.restart = CreateWindowExW(0, L"BUTTON", L"仅重启 TS3",
                                   WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
                                   0, 0, 0, 0, hwnd, (HMENU)IDC_RESTART, NULL, NULL);
    g_ui.browse = CreateWindowExW(0, L"BUTTON", L"手动选择目录…",
                                  WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
                                  0, 0, 0, 0, hwnd, (HMENU)IDC_BROWSE, NULL, NULL);
    g_ui.close = CreateWindowExW(0, L"BUTTON", L"关闭",
                                 WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
                                 0, 0, 0, 0, hwnd, (HMENU)IDC_CLOSE, NULL, NULL);

    HWND all[] = { g_ui.status, g_ui.log, g_ui.install, g_ui.uninstall,
                   g_ui.restart, g_ui.browse, g_ui.close };
    for (size_t i = 0; i < sizeof(all) / sizeof(all[0]); ++i) {
        SendMessageW(all[i], WM_SETFONT, (WPARAM)g_ui.font, TRUE);
    }
    SendMessageW(g_ui.status, WM_SETFONT, (WPARAM)g_ui.statusFont, TRUE);

    Layout(hwnd);
}

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE:
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)((CREATESTRUCTW*)lp)->hInstance);
        CommonInit(hwnd);
        return 0;

    case WM_SIZE:
        if (g_ui.log) Layout(hwnd);
        return 0;

    case WM_GETMINMAXINFO: {
        MINMAXINFO* mmi = (MINMAXINFO*)lp;
        mmi->ptMinTrackSize.x = 640;
        mmi->ptMinTrackSize.y = 420;
        return 0;
    }

    case WM_COMMAND: {
        int id = LOWORD(wp);
        if (id == IDC_BROWSE) {
            AppendLog(g_ui.log, L"");
            AppendLog(g_ui.log, L"请选择 TeamSpeak 3 的安装目录（含 config 文件夹的那个）。");
            Ts3Location picked;
            if (!BrowseForTs3(hwnd, picked)) {
                AppendLog(g_ui.log, L"已取消，或所选目录不是 TeamSpeak 3 的安装目录。");
                AppendLog(g_ui.log, L"提示：应选择包含 ts3client_win64.exe 或 config 文件夹的目录。");
                return 0;
            }
            g_ui.loc = picked;
            g_ui.found = true;
            AppendLog(g_ui.log, L"已手动指定：" + g_ui.loc.foundBy);
            AppendLog(g_ui.log, L"  配置目录：" + g_ui.loc.configDir);
            if (!g_ui.loc.exePath.empty()) AppendLog(g_ui.log, L"  程序：" + g_ui.loc.exePath);
            ShowFoundState();
            return 0;
        }
        if (id == IDC_INSTALL) {
            EnableWindow(g_ui.install, FALSE);
            EnableWindow(g_ui.uninstall, FALSE);
            EnableWindow(g_ui.restart, FALSE);
            SetStatus(g_ui.status, L"正在安装 ...", 0);
            AppendLog(g_ui.log, L"");
            AppendLog(g_ui.log, L"==================== 开始安装 ====================");
            if (!g_ui.found) {
                AppendLog(g_ui.log, L"没有自动定位到 TS3，请在弹出的对话框里手动选择安装目录。");
                if (!BrowseForTs3(hwnd, g_ui.loc)) {
                    AppendLog(g_ui.log, L"已取消选择，安装中止。");
                    SetStatus(g_ui.status, L"安装已取消", 0);
                    EnableWindow(g_ui.install, TRUE);
                    EnableWindow(g_ui.uninstall, TRUE);
                    EnableWindow(g_ui.restart, TRUE);
                    return 0;
                }
                g_ui.found = true;
                AppendLog(g_ui.log, L"手动指定：" + g_ui.loc.configDir);
            }
            SetCursor(LoadCursorW(NULL, IDC_WAIT));
            ActionResult r = DoInstall(g_ui.log, g_ui.status, ExeDir());
            SetCursor(LoadCursorW(NULL, IDC_ARROW));
            AppendLog(g_ui.log, r.ok ? L"安装成功。" : L"安装失败：" + r.message);
            EnableWindow(g_ui.install, TRUE);
            EnableWindow(g_ui.uninstall, TRUE);
            EnableWindow(g_ui.restart, TRUE);
            return 0;
        }
        if (id == IDC_UNINSTALL) {
            if (MessageBoxW(hwnd, L"确定要从 TeamSpeak 3 里卸载 Speaking Overlay 吗？\n\n"
                                  L"会删除插件 DLL 并从 addons.ini 里移除注册项。",
                            kAppTitle, MB_ICONQUESTION | MB_YESNO | MB_DEFBUTTON2) != IDYES) {
                return 0;
            }
            EnableWindow(g_ui.install, FALSE);
            EnableWindow(g_ui.uninstall, FALSE);
            EnableWindow(g_ui.restart, FALSE);
            SetStatus(g_ui.status, L"正在卸载 ...", 0);
            AppendLog(g_ui.log, L"");
            AppendLog(g_ui.log, L"==================== 开始卸载 ====================");
            if (!g_ui.found && !BrowseForTs3(hwnd, g_ui.loc)) {
                AppendLog(g_ui.log, L"已取消选择，卸载中止。");
                SetStatus(g_ui.status, L"卸载已取消", 0);
                EnableWindow(g_ui.install, TRUE);
                EnableWindow(g_ui.uninstall, TRUE);
                EnableWindow(g_ui.restart, TRUE);
                return 0;
            }
            g_ui.found = true;
            SetCursor(LoadCursorW(NULL, IDC_WAIT));
            ActionResult r = DoUninstall(g_ui.log, g_ui.status);
            SetCursor(LoadCursorW(NULL, IDC_ARROW));
            AppendLog(g_ui.log, r.ok ? L"卸载完成。" : L"卸载失败：" + r.message);
            EnableWindow(g_ui.install, TRUE);
            EnableWindow(g_ui.uninstall, TRUE);
            EnableWindow(g_ui.restart, TRUE);
            return 0;
        }
        if (id == IDC_RESTART) {
            if (!g_ui.found) { DoFindAndLog(); }
            std::wstring err;
            SetStatus(g_ui.status, L"正在重启 TeamSpeak 3 ...", 0);
            AppendLog(g_ui.log, L"");
            AppendLog(g_ui.log, L"正在重启 TeamSpeak 3 ...");
            if (StopTs3(&err) && StartTs3(g_ui.loc.exePath, &err)) {
                AppendLog(g_ui.log, L"  已重启。");
                SetStatus(g_ui.status, L"TeamSpeak 3 已重启", 0);
            } else {
                AppendLog(g_ui.log, L"  失败：" + err);
                SetStatus(g_ui.status, L"重启失败", 0);
            }
            return 0;
        }
        if (id == IDC_CLOSE) {
            DestroyWindow(hwnd);
            return 0;
        }
        return 0;
    }

    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;

    case WM_DESTROY:
        if (g_ui.font) DeleteObject(g_ui.font);
        if (g_ui.statusFont) DeleteObject(g_ui.statusFont);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE hPrev, LPWSTR cmdLine, int nShow) {
    (void)hPrev;
    (void)nShow;

    // Silent switches for scripted / one-click use:
    //   /install   install and exit
    //   /uninstall uninstall and exit
    //   /silent    no dialog at all
    bool silent = false;
    bool doInstallNow = false;
    bool doUninstallNow = false;
    {
        int argc = 0;
        LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
        if (argv) {
            for (int i = 1; i < argc; ++i) {
                if (_wcsicmp(argv[i], L"/silent") == 0 || _wcsicmp(argv[i], L"-silent") == 0) silent = true;
                if (_wcsicmp(argv[i], L"/install") == 0 || _wcsicmp(argv[i], L"-install") == 0) doInstallNow = true;
                if (_wcsicmp(argv[i], L"/uninstall") == 0 || _wcsicmp(argv[i], L"-uninstall") == 0) doUninstallNow = true;
            }
            LocalFree(argv);
        }
    }
    (void)cmdLine;

    if (silent && (doInstallNow || doUninstallNow)) {
        Ts3Location loc;
        if (!FindTs3(loc)) return 2;
        std::wstring err;
        if (!StopTs3(&err)) return 3;
        const std::wstring dllName = DllNameFor(loc.is64);
        if (doInstallNow) {
            if (!DirExists(loc.pluginsDir)) CreateDirectoryW(loc.pluginsDir.c_str(), NULL);
            std::wstring payload = TempPayloadPath(dllName);
            if (!ExtractPayload(payload, loc.is64, &err)) return 4;
            std::wstring target = JoinPath(loc.pluginsDir, dllName);
            bool copied = CopyFileW(payload.c_str(), target.c_str(), FALSE) != FALSE;
            DeleteFileW(payload.c_str());
            if (!copied) return 5;
            std::string ini;
            ReadFileUtf8(loc.addonsIni, &ini);
            bool changed = false;
            std::string updated = RegisterAddon(ini, dllName, &changed);
            if (!WriteFileBytes(loc.addonsIni, updated)) return 6;
            StartTs3(loc.exePath, &err);
            return 0;
        }
        if (doUninstallNow) {
            DeleteFileW(JoinPath(loc.pluginsDir, dllName).c_str());
            std::string ini;
            if (ReadFileUtf8(loc.addonsIni, &ini)) {
                bool changed = false;
                std::string updated = UnregisterAddon(ini, &changed);
                if (changed) WriteFileBytes(loc.addonsIni, updated);
            }
            return 0;
        }
    }

    INITCOMMONCONTROLSEX icc;
    icc.dwSize = sizeof(icc);
    icc.dwICC = ICC_STANDARD_CLASSES;
    InitCommonControlsEx(&icc);

    WNDCLASSEXW wc;
    ZeroMemory(&wc, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.hIcon = LoadIconW(hInst, MAKEINTRESOURCEW(IDI_APP));
    wc.hIconSm = wc.hIcon;
    wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = kWindowClass;
    if (!RegisterClassExW(&wc)) return 1;

    int W = 780, H = 560;
    int sx = GetSystemMetrics(SM_CXSCREEN), sy = GetSystemMetrics(SM_CYSCREEN);
    HWND hwnd = CreateWindowExW(0, kWindowClass, kAppTitle,
                                WS_OVERLAPPEDWINDOW,
                                (sx - W) / 2, (sy - H) / 2, W, H,
                                NULL, NULL, hInst, NULL);
    if (!hwnd) return 1;

    ShowWindow(hwnd, SW_SHOW);
    UpdateWindow(hwnd);
    DoFindAndLog();

    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        if (!IsDialogMessageW(hwnd, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    return (int)msg.wParam;
}

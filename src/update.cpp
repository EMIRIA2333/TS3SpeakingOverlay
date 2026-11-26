/*
 * ts3-speaking-overlay - always-on-top "who is talking" overlay (KOOK style)
 * update.cpp : GitHub release check + download + run installer
 *
 * Uses WinHTTP (no third-party dependency). The releases API is queried
 * without a token: the plugin only needs the public newest release.
 * JSON is not parsed with a library - the response is small and the fields we
 * need (tag_name, html_url, browser_download_url, body, prerelease) are unique
 * enough for a targeted scan.
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
#include <shellapi.h>
#include <winhttp.h>

#include <string>
#include <vector>

#include "update.h"
#include "version.h"

namespace ts3ovl {

namespace {

const wchar_t* const kHost       = L"api.github.com";
const wchar_t* const kLatestPath = L"/repos/EMIRIA2333/TS3SpeakingOverlay/releases";
const wchar_t* const kUserAgent  = L"TS3SpeakingOverlay-UpdateCheck";
const wchar_t* const kAssetName  = L"TS3SpeakingOverlay-Setup.exe";

std::wstring AnsiToWide(const std::string& s)
{
    if (s.empty()) return std::wstring();
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, NULL, 0);
    if (n <= 1) return std::wstring();
    std::wstring w((size_t)(n - 1), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, &w[0], n);
    return w;
}

std::string WideToAnsi(const std::wstring& s)
{
    if (s.empty()) return std::string();
    const int n = WideCharToMultiByte(CP_UTF8, 0, s.c_str(), -1, NULL, 0, NULL, NULL);
    if (n <= 1) return std::string();
    std::string a((size_t)(n - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, s.c_str(), -1, &a[0], n, NULL, NULL);
    return a;
}

/* Minimal JSON string unescaping for the few fields we read. */
std::wstring JsonUnescape(const std::string& raw)
{
    std::string out;
    out.reserve(raw.size());
    for (size_t i = 0; i < raw.size(); ++i) {
        char c = raw[i];
        if (c != '\\' || i + 1 >= raw.size()) { out += c; continue; }
        const char n = raw[++i];
        switch (n) {
        case 'n': out += '\n'; break;
        case 't': out += '\t'; break;
        case 'r': out += '\r'; break;
        case '"': out += '"';  break;
        case '\\': out += '\\'; break;
        case '/': out += '/';  break;
        case 'u': {
            if (i + 4 >= raw.size()) { out += '?'; break; }
            unsigned int cp = 0;
            for (int k = 1; k <= 4; ++k) {
                const char h = raw[i + (size_t)k];
                cp <<= 4;
                if (h >= '0' && h <= '9') cp |= (unsigned)(h - '0');
                else if (h >= 'a' && h <= 'f') cp |= (unsigned)(h - 'a' + 10);
                else if (h >= 'A' && h <= 'F') cp |= (unsigned)(h - 'A' + 10);
            }
            i += 4;
            if (cp < 0x80) {
                out += (char)cp;
            } else {
                const wchar_t wbuf[2] = { (wchar_t)cp, 0 };
                out += WideToAnsi(std::wstring(wbuf, 1));
            }
            break;
        }
        default: out += n; break;
        }
    }
    return AnsiToWide(out);
}

/* Value of `"key": "..."` inside a JSON blob, searched from `from`. */
std::wstring JsonStringField(const std::string& json, const char* key, size_t from, size_t* endOut)
{
    const std::string pat = std::string("\"") + key + "\"";
    const size_t pos = json.find(pat, from);
    if (pos == std::string::npos) return std::wstring();
    size_t q = json.find('"', pos + pat.size());
    if (q == std::string::npos) return std::wstring();
    std::string raw;
    for (size_t i = q + 1; i < json.size(); ++i) {
        if (json[i] == '\\' && i + 1 < json.size()) { raw += json[i]; raw += json[i + 1]; ++i; continue; }
        if (json[i] == '"') { if (endOut) *endOut = i + 1; return JsonUnescape(raw); }
        raw += json[i];
    }
    return std::wstring();
}

bool HttpGet(const std::wstring& host, const std::wstring& path, std::string* body, std::wstring* error)
{
    body->clear();
    const HINTERNET session = WinHttpOpen(kUserAgent, WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                          WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) {
        if (error) *error = L"WinHttpOpen 失败";
        return false;
    }
    /* GitHub is reachable through a system proxy on many machines; give the
     * automatic proxy resolution 8 s, then fall back to direct. */
    WinHttpSetTimeouts(session, 8000, 8000, 8000, 15000);

    bool ok = false;
    HINTERNET conn = WinHttpConnect(session, host.c_str(), 443, 0);
    if (!conn) {
        if (error) *error = L"无法连接 GitHub（WinHttpConnect 失败）";
        WinHttpCloseHandle(session);
        return false;
    }

    HINTERNET req = WinHttpOpenRequest(conn, L"GET", path.c_str(), NULL,
                                       WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                       WINHTTP_FLAG_SECURE);
    if (!req) {
        if (error) *error = L"WinHttpOpenRequest 失败";
        WinHttpCloseHandle(conn);
        WinHttpCloseHandle(session);
        return false;
    }

    /* api.github.com requires a User-Agent (we set one) and, since 2022, an
     * Accept header for the REST API. */
    const wchar_t* const headers = L"Accept: application/vnd.github+json\r\n";
    if (!WinHttpSendRequest(req, headers, (DWORD)-1L, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
        !WinHttpReceiveResponse(req, NULL)) {
        const DWORD err = GetLastError();
        if (error) {
            wchar_t buf[256];
            wsprintfW(buf, L"请求失败（网络或代理问题，WinHTTP 错误 %lu）", (unsigned long)err);
            *error = buf;
        }
        WinHttpCloseHandle(req);
        WinHttpCloseHandle(conn);
        WinHttpCloseHandle(session);
        return false;
    }

    DWORD status = 0;
    DWORD statusSize = sizeof(status);
    WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                        WINHTTP_HEADER_NAME_BY_INDEX, &status, &statusSize, WINHTTP_NO_HEADER_INDEX);
    if (status != 200) {
        if (error) {
            wchar_t buf[128];
            wsprintfW(buf, L"GitHub 返回 HTTP %lu", (unsigned long)status);
            *error = buf;
        }
        WinHttpCloseHandle(req);
        WinHttpCloseHandle(conn);
        WinHttpCloseHandle(session);
        return false;
    }

    DWORD avail = 0;
    do {
        if (!WinHttpQueryDataAvailable(req, &avail)) break;
        if (avail == 0) break;
        std::vector<char> buf(avail + 1, 0);
        DWORD read = 0;
        if (!WinHttpReadData(req, &buf[0], avail, &read) || read == 0) break;
        body->append(&buf[0], read);
    } while (true);

    ok = !body->empty();
    if (!ok && error) *error = L"GitHub 返回了空响应";

    WinHttpCloseHandle(req);
    WinHttpCloseHandle(conn);
    WinHttpCloseHandle(session);
    return ok;
}

/* Split "1.2.0-beta1" into numeric components + pre-release flag. */
void ParseVersion(const std::wstring& in, std::vector<int>* parts, std::wstring* pre)
{
    std::wstring v = in;
    if (!v.empty() && (v[0] == L'v' || v[0] == L'V')) v.erase(0, 1);

    parts->clear();
    pre->clear();

    std::wstring num;
    size_t i = 0;
    for (; i < v.size(); ++i) {
        const wchar_t c = v[i];
        if (c >= L'0' && c <= L'9') { num += c; continue; }
        if (c == L'.') { parts->push_back(num.empty() ? 0 : _wtoi(num.c_str())); num.clear(); continue; }
        break;
    }
    parts->push_back(num.empty() ? 0 : _wtoi(num.c_str()));
    if (i < v.size()) *pre = v.substr(i);
}

} // namespace

std::wstring UpdateCurrentVersion()
{
    return TS3OVL_VERSION_W;
}

bool UpdateIsNewer(const std::wstring& candidate, const std::wstring& current)
{
    std::vector<int> a, b;
    std::wstring pa, pb;
    ParseVersion(candidate, &a, &pa);
    ParseVersion(current, &b, &pb);

    const size_t n = (a.size() > b.size()) ? a.size() : b.size();
    for (size_t i = 0; i < n; ++i) {
        const int av = (i < a.size()) ? a[i] : 0;
        const int bv = (i < b.size()) ? b[i] : 0;
        if (av != bv) return av > bv;
    }
    /* same numbers: a release (no suffix) is newer than a pre-release */
    if (pa.empty() && !pb.empty()) return true;
    if (!pa.empty() && pb.empty()) return false;
    return pa > pb;
}

UpdateResult UpdateCheck()
{
    UpdateResult r;

    std::string body;
    std::wstring err;
    if (!HttpGet(kHost, kLatestPath, &body, &err)) {
        r.error = err;
        return r;
    }

    /* /releases returns an array; the first element is the newest. Only look
     * at it, so a stale older release cannot win. Parse up to the first
     * occurrence of `"assets"` of that element. */
    size_t firstObj = body.find('{');
    if (firstObj == std::string::npos) {
        r.error = L"GitHub 响应格式无法解析";
        return r;
    }

    /* tag + page */
    r.latestTag = JsonStringField(body, "tag_name", firstObj, NULL);
    r.pageUrl   = JsonStringField(body, "html_url", firstObj, NULL);
    r.notes     = JsonStringField(body, "body", firstObj, NULL);
    if (r.notes.size() > 1200) { r.notes.resize(1200); r.notes += L"…"; }
    if (r.latestTag.empty()) {
        r.error = L"没有从 GitHub 取到版本号（可能是网络被拦截或 API 限流）";
        return r;
    }
    r.ok = true;

    /* download URL: the first browser_download_url whose file name matches our
     * installer asset. If the release carries no such asset, fall back to the
     * release page so the user can still get it. */
    size_t pos = firstObj;
    while ((pos = body.find("\"browser_download_url\"", pos)) != std::string::npos) {
        size_t end = 0;
        const std::wstring url = JsonStringField(body, "browser_download_url", pos, &end);
        if (!url.empty()) {
            const size_t slash = url.find_last_of(L'/');
            const std::wstring name = (slash == std::wstring::npos) ? url : url.substr(slash + 1);
            if (name == kAssetName) { r.downloadUrl = url; break; }
            if (r.downloadUrl.empty() && name.size() > 4 &&
                name.compare(name.size() - 4, 4, L".exe") == 0) {
                r.downloadUrl = url;   /* any installer beats none */
            }
        }
        pos = (end != 0) ? end : pos + 1;
    }

    r.hasUpdate = UpdateIsNewer(r.latestTag, UpdateCurrentVersion());
    return r;
}

bool UpdateDownload(const std::wstring& url, const std::wstring& dstPath, std::wstring* error)
{
    /* https://host/path?query -> host + path */
    std::wstring rest = url;
    const std::wstring scheme = L"https://";
    if (rest.compare(0, scheme.size(), scheme) != 0) {
        if (error) *error = L"只支持 https 下载地址";
        return false;
    }
    rest = rest.substr(scheme.size());
    const size_t slash = rest.find(L'/');
    if (slash == std::wstring::npos) {
        if (error) *error = L"下载地址无效";
        return false;
    }
    const std::wstring host = rest.substr(0, slash);
    const std::wstring path = rest.substr(slash);

    const HINTERNET session = WinHttpOpen(kUserAgent, WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                          WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) {
        if (error) *error = L"WinHttpOpen 失败";
        return false;
    }
    WinHttpSetTimeouts(session, 15000, 15000, 30000, 120000);

    bool ok = false;
    HINTERNET conn = WinHttpConnect(session, host.c_str(), 443, 0);
    HINTERNET req  = conn ? WinHttpOpenRequest(conn, L"GET", path.c_str(), NULL,
                                               WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                               WINHTTP_FLAG_SECURE) : NULL;
    if (!req || !WinHttpSendRequest(req, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                                    WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
        !WinHttpReceiveResponse(req, NULL)) {
        if (error) {
            wchar_t buf[128];
            wsprintfW(buf, L"下载请求失败（%lu）", (unsigned long)GetLastError());
            *error = buf;
        }
    } else {
        DWORD status = 0;
        DWORD statusSize = sizeof(status);
        WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                            WINHTTP_HEADER_NAME_BY_INDEX, &status, &statusSize, WINHTTP_NO_HEADER_INDEX);
        if (status != 200) {
            if (error) {
                wchar_t buf[128];
                wsprintfW(buf, L"下载返回 HTTP %lu", (unsigned long)status);
                *error = buf;
            }
        } else {
            FILE* f = NULL;
            if (_wfopen_s(&f, dstPath.c_str(), L"wb") != 0 || !f) {
                if (error) *error = L"无法写入下载文件（路径不可写？）";
            } else {
                ok = true;
                DWORD avail = 0;
                while (ok) {
                    if (!WinHttpQueryDataAvailable(req, &avail)) { ok = false; break; }
                    if (avail == 0) break;
                    std::vector<char> buf(avail + 1, 0);
                    DWORD read = 0;
                    if (!WinHttpReadData(req, &buf[0], avail, &read) || read == 0) { ok = false; break; }
                    if (fwrite(&buf[0], 1, read, f) != read) { ok = false; break; }
                }
                fclose(f);
                if (!ok && error) *error = L"下载中断（网络错误或磁盘写入失败）";
            }
        }
    }

    if (req) WinHttpCloseHandle(req);
    if (conn) WinHttpCloseHandle(conn);
    WinHttpCloseHandle(session);
    return ok;
}

bool UpdateRunInstaller(const std::wstring& setupPath, std::wstring* error)
{
    if (GetFileAttributesW(setupPath.c_str()) == INVALID_FILE_ATTRIBUTES) {
        if (error) *error = L"下载的安装包不存在";
        return false;
    }
    /* The installer stops the client, replaces the DLL and starts it again, so
     * it must not be tied to our process; ShellExecuteW hands it off. */
    const HINSTANCE h = ShellExecuteW(NULL, L"open", setupPath.c_str(), L"/silent /install",
                                      NULL, SW_SHOWNORMAL);
    if ((INT_PTR)h <= 32) {
        if (error) {
            wchar_t buf[128];
            wsprintfW(buf, L"无法启动安装包（ShellExecute 返回 %lld）", (long long)(INT_PTR)h);
            *error = buf;
        }
        return false;
    }
    return true;
}

} // namespace ts3ovl

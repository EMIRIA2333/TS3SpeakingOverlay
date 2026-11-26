/*
 * ts3-speaking-overlay - always-on-top "who is talking" overlay (KOOK style)
 * update.h : "check for updates / download / install" support
 *
 * The check queries the GitHub releases API of the project and compares the
 * tag of the newest release with TS3OVL_VERSION. Everything here is blocking
 * (WinHTTP), so the settings window runs it on a worker thread.
 */
#ifndef TS3OVL_UPDATE_H
#define TS3OVL_UPDATE_H

#include <windows.h>

#include <string>

namespace ts3ovl {

struct UpdateResult {
    bool        ok;            /* the check itself succeeded            */
    bool        hasUpdate;     /* newest release is newer than ours     */
    std::wstring latestTag;    /* e.g. "v1.2.0"                         */
    std::wstring downloadUrl;  /* direct URL of the setup executable    */
    std::wstring pageUrl;      /* release page, for "open in browser"   */
    std::wstring notes;        /* release body (truncated)             */
    std::wstring error;

    UpdateResult() : ok(false), hasUpdate(false) {}
};

/* Blocking. Safe to call from any thread (WinHTTP only). */
UpdateResult UpdateCheck();

/* Blocking download; returns false and fills error on failure. */
bool UpdateDownload(const std::wstring& url, const std::wstring& dstPath, std::wstring* error);

/* "1.2.0-beta1" -> true when candidate is strictly newer than current. */
bool UpdateIsNewer(const std::wstring& candidate, const std::wstring& current);

/* The version this build reports (TS3OVL_VERSION). */
std::wstring UpdateCurrentVersion();

/* Launch a downloaded setup with /silent /install (it restarts TS3 itself). */
bool UpdateRunInstaller(const std::wstring& setupPath, std::wstring* error);

} // namespace ts3ovl

#endif /* TS3OVL_UPDATE_H */

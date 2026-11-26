/*
 * ts3-speaking-overlay - always-on-top "who is talking" overlay (KOOK style)
 * games.h : the game whitelist that decides when the overlay is allowed to show
 *
 * The old gate looked at the foreground window (fullscreen? captionless?) which
 * is only a guess. This module replaces it with an explicit list of game
 * executables: the overlay is shown while at least one *enabled* executable
 * from that list is running, no matter which window has focus.
 */
#ifndef TS3OVL_GAMES_H
#define TS3OVL_GAMES_H

#include <windows.h>

#include <string>
#include <vector>

namespace ts3ovl {

struct GameEntry {
    std::wstring exe;      /* lower-case file name, e.g. "cs2.exe" */
    bool enabled;          /* show the overlay while this one runs    */
    bool autoAdded;        /* found by the scanner, not added by hand */
};

/* Load/save the list. iniPath is the same overlay.ini the renderer uses. */
void GamesInit(const std::wstring& iniPath);
void GamesSave();

std::vector<GameEntry> GamesList();

/* Exact file names only; the argument is lower-cased internally.
 * GamesAdd returns false when the entry already exists. */
bool GamesAdd(const std::wstring& exe);
bool GamesRemove(const std::wstring& exe);
bool GamesSetEnabled(const std::wstring& exe, bool enabled);
bool GamesContains(const std::wstring& exe);

/* Current "enabled" flag of one entry (false when it is not listed). */
bool GamesEntryEnabled(const std::wstring& exe);

/* Executables the user deleted from the list. Auto detection must not bring
 * them back, otherwise removing a game looks like it silently failed. */
bool GamesIsIgnored(const std::wstring& exe);
void GamesSetIgnored(const std::wstring& exe, bool ignored);

/* "Detect newly installed games automatically" switch (persisted). */
void GamesSetAutoDetect(bool on);
bool GamesAutoDetect();

/* The gate: true when an enabled executable from the list is running.
 * Result is cached for under a second because Paint() asks on every frame. */
bool GamesAnyRunning();

/* Name of the running enabled game (lower-case), empty when none. */
std::wstring GamesRunningName();

/* Scan the Steam libraries for games that are not in the list yet and add them
 * enabled (a freshly downloaded game shows the overlay by default).
 * Returns the exe names that were added. */
std::vector<std::wstring> GamesScanForNew();

/* Scan the desktop and Start-menu shortcuts (.lnk) and add their targets. This
 * is what finds games that no launcher manifest knows about (zip installs,
 * portable copies, unknown launchers) - the shortcut names the real exe.
 * Returns the exe names that were added. */
std::vector<std::wstring> GamesScanShortcuts();
/* lower-case exe name of the process owning a window / of a pid, "" if unknown */
std::wstring GamesExeOfWindow(unsigned long long hwnd);
std::wstring GamesExeOfProcess(unsigned long pid);

/* Exe names of all visible top-level applications, so the settings window can
 * offer "add the program I am running" without a file dialog. Sorted. */
std::vector<std::wstring> GamesVisibleWindowExes();

} // namespace ts3ovl

#endif /* TS3OVL_GAMES_H */

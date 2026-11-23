/*
 * ts3-speaking-overlay - always-on-top "who is talking" overlay (KOOK style)
 * overlay.h : renderer / windowing layer, independent of the TS3 plugin API
 */
#ifndef TS3OVL_OVERLAY_H
#define TS3OVL_OVERLAY_H

#include <windows.h>
#include <string>
#include <vector>

namespace ts3ovl {

/* One member of the current channel that should be visible in the overlay.
 * Members that are not talking are drawn dimmed instead of being removed, so
 * the panel stays in place and only the highlight moves (KOOK behaviour). */
struct Entry {
    unsigned int id;          /* TS3 client id (anyID) */
    std::wstring name;        /* display name */
    std::wstring avatarPath;  /* cached avatar file, may be empty */
    bool talking;             /* true -> bright + pulsing ring, false -> dimmed */
};

/* Start the overlay thread (creates the layered window).
 * iniPath may be empty -> position/settings are not persisted. */
bool OverlayStart(HINSTANCE hInst, const std::wstring& iniPath);

/* Stop the overlay thread and destroy all GDI+ / window resources. */
void OverlayStop();

/* Replace the full set of visible channel members (thread safe, callable from
 * any thread). Members with talking == false are rendered dimmed, so the list
 * being non-empty is what keeps the overlay on screen. */
void OverlaySetEntries(const std::vector<Entry>& entries);

/* Forward an avatar file path for a client (thread safe). */
void OverlaySetAvatar(unsigned int id, const std::wstring& path);

/* Menu commands (thread safe). */
void OverlayToggleVisible();
void OverlayToggleLock();
void OverlayResetPosition();

/* Toggle "only show the overlay while a game is in the foreground". */
void OverlayToggleOnlyInGame();

/* Current state of the three switches, so the plugin can show them in its
 * menu (TS3's PluginMenuItem has no checked/state field, so the state has to
 * be part of the item text). All of them are thread safe. */
bool OverlayVisibleEnabled();   /* the show/hide switch, not the window state */
bool OverlayIsLocked();         /* position locked -> click-through          */
bool OverlayOnlyInGameEnabled();/* the "only in game" switch                 */

/* True when the overlay currently hides itself because the foreground app is
 * not a game (only meaningful when "only in game" is enabled). */
bool OverlayHiddenByForeground();

/* Persist the two switch values that have no other write path (locked and
 * onlyInGame). Safe to call from any thread; used by the settings window. */
void OverlaySaveSettings();

/* True when the overlay is currently shown (nobody talking -> false). */
bool OverlayIsShowing();

/* Evaluate the game heuristic for the CURRENT foreground window. Exposed so
 * the standalone probe (test_gamedetect) judges exactly like the overlay. */
bool OverlayGameForegroundNow();

/* Evaluate the same heuristic for an explicit window handle. */
bool OverlayClassifyWindow(unsigned long long hwnd);

} // namespace ts3ovl

#endif /* TS3OVL_OVERLAY_H */

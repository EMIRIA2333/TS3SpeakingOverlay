/*
 * ts3-speaking-overlay - always-on-top "who is talking" overlay (KOOK style)
 * settings.h : native settings window
 */
#ifndef TS3OVL_SETTINGS_H
#define TS3OVL_SETTINGS_H

namespace ts3ovl {

/* Show (and raise) the settings window; runs a modal message loop, so it must
 * be called from a thread that is allowed to block - which is what the client
 * gives us through ts3plugin_configure (see PLUGIN_OFFERS_CONFIGURE_NEW_THREAD).
 * `owner` is the client's QWidget* and is only used as a hint. */
void SettingsShow(void* owner);

/* Same, but never blocks: opens the window on its own thread and returns
 * immediately. Used from the plugin menu, i.e. the client's Qt GUI thread. */
void SettingsShowAsync();

/* Same as SettingsShowAsync but jumps straight to the update section, so the
 * "检查更新…" menu item does not have to make the user find the button. */
void SettingsShowUpdateAsync();

/* Close the window if it is open (used from ts3plugin_shutdown). */
void SettingsClose();

} // namespace ts3ovl

#endif /* TS3OVL_SETTINGS_H */

/*
 * ts3-speaking-overlay - always-on-top "who is talking" overlay (KOOK style)
 * settings.h : small native settings window (live switch states)
 *
 * TS3's PluginMenuItem has no checked/state field, the client calls
 * ts3plugin_initMenus exactly once per process and it frees the menu array
 * immediately afterwards, so the plugin menu can never show live states.
 * The client does however offer a real per-plugin config window
 * (Options -> Addons -> Settings, i.e. ts3plugin_configure), which is what
 * this window is for: three checkboxes that always reflect the actual state.
 */
#ifndef TS3OVL_SETTINGS_H
#define TS3OVL_SETTINGS_H

namespace ts3ovl {

/* Show (or raise) the settings window.
 * owner may be NULL; it is only used as the owner window for z-order.
 * Must be called from a thread that pumps messages (the TS3 GUI thread). */
void SettingsShow(void* owner);

/* Same, but never blocks the caller: the window is opened on its own thread.
 * Use this from contexts that must not pump messages (e.g. the plugin menu
 * handler, which runs on the client's Qt GUI thread). */
void SettingsShowAsync();

/* Close the window if it is open (used from ts3plugin_shutdown). */
void SettingsClose();

} // namespace ts3ovl

#endif /* TS3OVL_SETTINGS_H */

/*
 * ts3-speaking-overlay - always-on-top "who is talking" overlay (KOOK style)
 * plugin.h : exported entry points expected by the TeamSpeak 3 client
 */
#ifndef TS3OVL_PLUGIN_H
#define TS3OVL_PLUGIN_H

#if defined(WIN32) || defined(__WIN32__) || defined(_WIN32)
#define PLUGINS_EXPORTDLL __declspec(dllexport)
#else
#define PLUGINS_EXPORTDLL __attribute__((visibility("default")))
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* Required functions */
PLUGINS_EXPORTDLL const char* ts3plugin_name();
PLUGINS_EXPORTDLL const char* ts3plugin_version();
PLUGINS_EXPORTDLL int         ts3plugin_apiVersion();
PLUGINS_EXPORTDLL const char* ts3plugin_author();
PLUGINS_EXPORTDLL const char* ts3plugin_description();
PLUGINS_EXPORTDLL void        ts3plugin_setFunctionPointers(const struct TS3Functions funcs);
PLUGINS_EXPORTDLL int         ts3plugin_init();
PLUGINS_EXPORTDLL void        ts3plugin_shutdown();

/* Optional functions */
PLUGINS_EXPORTDLL int         ts3plugin_offersConfigure();
PLUGINS_EXPORTDLL void        ts3plugin_configure(void* handle, void* qParentWidget);
PLUGINS_EXPORTDLL void        ts3plugin_registerPluginID(const char* id);
PLUGINS_EXPORTDLL int         ts3plugin_requestAutoload();
PLUGINS_EXPORTDLL void        ts3plugin_initMenus(struct PluginMenuItem*** menuItems, char** menuIcon);
/* Required by the client for plugin menus: it releases memory the plugin
 * handed out (menu arrays, info text, ...). Must be an undecorated export,
 * i.e. declared inside this extern "C" block. */
PLUGINS_EXPORTDLL void        ts3plugin_freeMemory(void* data);

/* Clientlib */
PLUGINS_EXPORTDLL void ts3plugin_onConnectStatusChangeEvent(uint64 serverConnectionHandlerID, int newStatus, unsigned int errorNumber);
PLUGINS_EXPORTDLL void ts3plugin_onUpdateClientEvent(uint64 serverConnectionHandlerID, anyID clientID, anyID invokerID, const char* invokerName, const char* invokerUniqueIdentifier);
PLUGINS_EXPORTDLL void ts3plugin_onClientMoveEvent(uint64 serverConnectionHandlerID, anyID clientID, uint64 oldChannelID, uint64 newChannelID, int visibility, const char* moveMessage);
PLUGINS_EXPORTDLL void ts3plugin_onClientMoveTimeoutEvent(uint64 serverConnectionHandlerID, anyID clientID, uint64 oldChannelID, uint64 newChannelID, int visibility, const char* timeoutMessage);
PLUGINS_EXPORTDLL void ts3plugin_onClientMoveMovedEvent(uint64 serverConnectionHandlerID, anyID clientID, uint64 oldChannelID, uint64 newChannelID, int visibility, anyID moverID, const char* moverName, const char* moverUniqueIdentifier, const char* moveMessage);
PLUGINS_EXPORTDLL void ts3plugin_onClientKickFromChannelEvent(uint64 serverConnectionHandlerID, anyID clientID, uint64 oldChannelID, uint64 newChannelID, int visibility, anyID kickerID, const char* kickerName, const char* kickerUniqueIdentifier, const char* kickMessage);
PLUGINS_EXPORTDLL void ts3plugin_onClientKickFromServerEvent(uint64 serverConnectionHandlerID, anyID clientID, uint64 oldChannelID, uint64 newChannelID, int visibility, anyID kickerID, const char* kickerName, const char* kickerUniqueIdentifier, const char* kickMessage);
PLUGINS_EXPORTDLL void ts3plugin_onTalkStatusChangeEvent(uint64 serverConnectionHandlerID, int status, int isReceivedWhisper, anyID clientID);

/* Client UI callbacks */
PLUGINS_EXPORTDLL void        ts3plugin_onAvatarUpdated(uint64 serverConnectionHandlerID, anyID clientID, const char* avatarPath);
PLUGINS_EXPORTDLL void        ts3plugin_onMenuItemEvent(uint64 serverConnectionHandlerID, enum PluginMenuType type, int menuItemID, uint64 selectedItemID);
PLUGINS_EXPORTDLL void        ts3plugin_currentServerConnectionChanged(uint64 serverConnectionHandlerID);
PLUGINS_EXPORTDLL void        ts3plugin_onClientDisplayNameChanged(uint64 serverConnectionHandlerID, anyID clientID, const char* displayName, const char* uniqueClientIdentifier);

#ifdef __cplusplus
}
#endif

#endif /* TS3OVL_PLUGIN_H */

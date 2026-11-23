/*
 * ts3-speaking-overlay - always-on-top "who is talking" overlay (KOOK style)
 * plugin.cpp : TeamSpeak 3 client plugin (API 26, x64)
 *
 * The plugin observes talk status and channel membership, keeps only the
 * clients of the channel the local user currently sits in, and feeds that set
 * to the overlay renderer (overlay.cpp). Everything runs in-process, so no
 * helper service, port or IPC is involved.
 */
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <vector>

#include "teamspeak/public_definitions.h"
#include "teamspeak/public_errors.h"
#include "teamspeak/public_errors_rare.h"
#include "teamspeak/public_rare_definitions.h"
#include "ts3_functions.h"

#include "plugin.h"
#include "overlay.h"
#include "settings.h"

/* TS3 3.6.x (64-bit client) requires API 26. Older 32-bit clients (3.5.x and
 * earlier) require 25/24 and refuse anything newer, so the value is
 * overridable at build time (-DPLUGIN_API_VERSION=25) when producing a 32-bit
 * build. 64-bit and 32-bit clients never share a plugin directory, so the two
 * builds can coexist. */
#ifndef PLUGIN_API_VERSION
#define PLUGIN_API_VERSION 26
#endif

static struct TS3Functions ts3Functions;
static char* pluginID = NULL;

#define PATH_BUFSIZE 512

/* menu ids */
enum {
    MENU_ID_TOGGLE_VISIBLE = 1,
    MENU_ID_ONLY_IN_GAME   = 2,
    MENU_ID_TOGGLE_LOCK    = 3,
    MENU_ID_RESET_POSITION = 4,
    MENU_ID_SHOW_STATE     = 5,
};

/* ------------------------------------------------------------------ */
/* state                                                               */
/* ------------------------------------------------------------------ */
namespace {

std::mutex              g_lock;
std::set<anyID>         g_talking;          /* client ids currently talking      */
std::map<anyID, std::wstring> g_names;      /* cached display names             */
uint64                  g_channel = 0;      /* our current channel              */
uint64                  g_handler = 0;      /* our connection handler           */
bool                    g_overlayUp = false;

/* UTF-8 (TS3 wire encoding) <-> UTF-16 helpers */
std::wstring Utf8ToWide(const char* s)
{
    if (!s || !*s) return std::wstring();
    int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, NULL, 0);
    if (n <= 1) return std::wstring();
    std::wstring out((size_t)(n - 1), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s, -1, &out[0], n);
    return out;
}

/* Resolve the display name of a client, caching the result. */
std::wstring NameOf(uint64 handler, anyID clientID)
{
    std::map<anyID, std::wstring>::const_iterator it = g_names.find(clientID);
    if (it != g_names.end()) return it->second;

    char buf[TS3_MAX_SIZE_CLIENT_NICKNAME * 4] = {0};
    if (ts3Functions.getClientDisplayName &&
        ts3Functions.getClientDisplayName(handler, clientID, buf, sizeof(buf)) == ERROR_ok && buf[0]) {
        std::wstring w = Utf8ToWide(buf);
        g_names[clientID] = w;
        return w;
    }
    return L"?";
}

/* Ask the client for a client's avatar file.
 *
 * getAvatar has three distinct outcomes (see the SDK's own comments):
 *   ERROR_ok + non-empty path  -> the avatar is cached, this is the file
 *   ERROR_ok + empty path      -> the client HAS an avatar, but the image is
 *                                 not downloaded yet; the client starts the
 *                                 transfer and calls onAvatarUpdated when done
 *   ERROR_database_empty_result-> the client simply has no avatar
 *   anything else              -> hard failure
 *
 * The "not downloaded yet" case must NOT be remembered as a failure: it turns
 * into a real path a moment later. Only the definitive answers are cached. */
std::map<anyID, std::wstring> g_avatars;   /* client -> cached avatar path    */
std::set<anyID>              g_noAvatar;   /* client definitely has none      */
std::set<anyID>              g_avatarAsked;/* already requested an info update */

std::wstring AvatarOf(uint64 handler, anyID clientID)
{
    std::map<anyID, std::wstring>::const_iterator it = g_avatars.find(clientID);
    if (it != g_avatars.end()) return it->second;
    /* a definitive "no avatar" is final for this connection */
    if (g_noAvatar.count(clientID)) return std::wstring();

    char buf[2048] = {0};
    if (!ts3Functions.getAvatar) return std::wstring();
    unsigned int err = ts3Functions.getAvatar(handler, clientID, buf, sizeof(buf));

    if (err == ERROR_database_empty_result) {
        /* definitive: nothing to draw, stop asking */
        g_noAvatar.insert(clientID);
        return std::wstring();
    }
    if (err != ERROR_ok) return std::wstring();   /* transient; retry later */

    if (!buf[0]) {
        /* Avatar not cached yet: the client HAS an avatar and is fetching it;
         * it will call onAvatarUpdated once the image lands. Meanwhile ask the
         * client to refresh this item once, which is what actually kicks off
         * the download for clients whose info has not been polled yet.
         * Returning empty only costs us an initial disc for a few frames. */
        if (g_avatarAsked.insert(clientID).second && ts3Functions.requestInfoUpdate)
            ts3Functions.requestInfoUpdate(handler, PLUGIN_CLIENT, clientID);
        return std::wstring();
    }

    std::wstring w = Utf8ToWide(buf);
    if (GetFileAttributesW(w.c_str()) == INVALID_FILE_ATTRIBUTES) return std::wstring();
    g_avatars[clientID] = w;
    return w;
}

/* Rebuild the overlay content: every client in our channel, flagged with
 * whether it is currently talking. Silent members are kept so the overlay can
 * dim them instead of removing rows (KOOK behaviour). */
void Refresh()
{
    std::vector<ts3ovl::Entry> entries;
    {
        std::lock_guard<std::mutex> lk(g_lock);
        if (!g_overlayUp || g_handler == 0 || g_channel == 0) {
            ts3ovl::OverlaySetEntries(entries);
            return;
        }
    }

    uint64 handler;
    uint64 myChannel;
    std::set<anyID> talking;
    {
        std::lock_guard<std::mutex> lk(g_lock);
        handler   = g_handler;
        myChannel = g_channel;
        talking   = g_talking;
    }

    /* Who is in our channel? getChannelClientList hands back a client list the
     * plugin has to release through ts3Functions.freeMemory. */
    anyID* clients = NULL;
    if (!ts3Functions.getChannelClientList ||
        ts3Functions.getChannelClientList(handler, myChannel, &clients) != ERROR_ok) {
        clients = NULL;
    }

    std::vector<anyID> ids;
    if (clients) {
        for (anyID* p = clients; *p; ++p) ids.push_back(*p);
        if (ts3Functions.freeMemory) ts3Functions.freeMemory(clients);
    } else {
        /* Fall back to the talking set so the overlay still works on servers
         * that refuse the channel query. */
        ids.assign(talking.begin(), talking.end());
    }

    for (size_t i = 0; i < ids.size(); ++i) {
        ts3ovl::Entry e;
        e.id = ids[i];
        {
            std::lock_guard<std::mutex> lk(g_lock);
            e.name       = NameOf(handler, ids[i]);
            e.avatarPath = AvatarOf(handler, ids[i]);
        }
        e.talking = talking.count(ids[i]) != 0;
        entries.push_back(e);
    }

    /* Gentle ordering: speakers bubble to the top (so the interesting row is
     * always visible even when the channel is truncated), the rest keep a
     * stable client-id order so rows do not jump around. */
    for (size_t i = 0; i + 1 < entries.size(); ++i) {
        for (size_t j = i + 1; j < entries.size(); ++j) {
            bool swap = false;
            if (entries[j].talking != entries[i].talking) swap = entries[j].talking;
            else if (entries[j].id < entries[i].id)       swap = true;
            if (swap) {
                ts3ovl::Entry tmp = entries[i];
                entries[i] = entries[j];
                entries[j] = tmp;
            }
        }
    }

    ts3ovl::OverlaySetEntries(entries);
}

/* Track our own channel; called whenever membership may have changed. */
void RefreshOwnChannel(uint64 handler)
{
    anyID self = 0;
    uint64 channel = 0;
    if (ts3Functions.getClientID && ts3Functions.getClientID(handler, &self) == ERROR_ok &&
        ts3Functions.getChannelOfClient && ts3Functions.getChannelOfClient(handler, self, &channel) == ERROR_ok) {
        std::lock_guard<std::mutex> lk(g_lock);
        g_channel = channel;
    } else {
        std::lock_guard<std::mutex> lk(g_lock);
        g_channel = 0;
    }
}

void Log(const char* msg)
{
    if (ts3Functions.logMessage)
        ts3Functions.logMessage(msg, LogLevel_INFO, "SpeakingOverlay", 0);
    /* mirror into %TEMP%\ts3ovl_plugin.log so early-call traces are visible
     * even before the client's own log sink is wired up */
    wchar_t path[MAX_PATH] = {0};
    if (GetEnvironmentVariableW(L"TEMP", path, MAX_PATH) == 0) return;
    wcscat_s(path, MAX_PATH, L"\\ts3ovl_plugin.log");
    FILE* f = NULL;
    if (_wfopen_s(&f, path, L"a, ccs=UTF-8") != 0 || !f) return;
    SYSTEMTIME st;
    GetLocalTime(&st);
    fwprintf(f, L"%02d:%02d:%02d %hs\n", st.wHour, st.wMinute, st.wSecond, msg);
    fclose(f);
}

} // anonymous namespace

/* ------------------------------------------------------------------ */
/* required exports                                                    */
/* ------------------------------------------------------------------ */
const char* ts3plugin_name()
{
    return "Speaking Overlay";
}

const char* ts3plugin_version()
{
    return "1.0";
}

int ts3plugin_apiVersion()
{
    return PLUGIN_API_VERSION;
}

const char* ts3plugin_author()
{
    return "ts3-speaking-overlay";
}

const char* ts3plugin_description()
{
    return "Shows who is currently talking in your channel in an always-on-top overlay, like KOOK.";
}

void ts3plugin_setFunctionPointers(const struct TS3Functions funcs)
{
    ts3Functions = funcs;
    Log("setFunctionPointers called");
}

int ts3plugin_init()
{
    char configPath[PATH_BUFSIZE] = {0};
    char pluginPath[PATH_BUFSIZE] = {0};

    if (ts3Functions.getConfigPath) ts3Functions.getConfigPath(configPath, PATH_BUFSIZE);
    if (ts3Functions.getPluginPath && pluginID) ts3Functions.getPluginPath(pluginPath, PATH_BUFSIZE, pluginID);

    /* overlay.ini lives inside the plugin directory so it survives updates */
    std::wstring ini;
    if (pluginPath[0]) {
        ini = Utf8ToWide(pluginPath);
        if (!ini.empty() && ini[ini.size() - 1] != L'\\') ini += L'\\';
        ini += L"overlay.ini";
    } else if (configPath[0]) {
        ini = Utf8ToWide(configPath);
        if (!ini.empty() && ini[ini.size() - 1] != L'\\') ini += L'\\';
        ini += L"plugins\\overlay.ini";
    }

    if (!ts3ovl::OverlayStart((HINSTANCE)GetModuleHandleW(NULL), ini)) {
        return 1;
    }
    {
        std::lock_guard<std::mutex> lk(g_lock);
        g_overlayUp = true;
    }

    /* pick up an existing connection (plugin reload while connected) */
    if (ts3Functions.getCurrentServerConnectionHandlerID) {
        uint64 handler = ts3Functions.getCurrentServerConnectionHandlerID();
        if (handler) {
            std::lock_guard<std::mutex> lk(g_lock);
            g_handler = handler;
        }
        if (handler) RefreshOwnChannel(handler);
    }
    Refresh();
    Log("Speaking Overlay loaded");
    return 0;
}

void ts3plugin_shutdown()
{
    {
        std::lock_guard<std::mutex> lk(g_lock);
        if (g_overlayUp) {
            g_overlayUp = false;
            g_talking.clear();
            g_names.clear();
            g_avatars.clear();
            g_noAvatar.clear();
        g_avatarAsked.clear();
        }
    }
    ts3ovl::SettingsClose();
    ts3ovl::OverlayStop();

    if (pluginID) {
        free(pluginID);
        pluginID = NULL;
    }
}

/* ------------------------------------------------------------------ */
/* optional exports                                                    */
/* ------------------------------------------------------------------ */
int ts3plugin_offersConfigure()
{
    Log("offersConfigure called");
    /* The plugin menu cannot show live switch states (the client calls
     * ts3plugin_initMenus once per process and then frees the menu array), so
     * the switches are offered through a real settings window instead, which
     * the client shows under Options -> Addons -> Settings. */
    return PLUGIN_OFFERS_CONFIGURE_NEW_THREAD;
}

void ts3plugin_configure(void* handle, void* qParentWidget)
{
    char msg[256];
    snprintf(msg, sizeof(msg), "configure called: handle=%p qParentWidget=%p",
             handle, qParentWidget);
    Log(msg);
    ts3ovl::SettingsShow(qParentWidget);
}

void ts3plugin_registerPluginID(const char* id)
{
    if (!id) return;
    size_t sz = strlen(id) + 1;
    pluginID = (char*)malloc(sz);
    if (pluginID) memcpy(pluginID, id, sz);
    char msg[256];
    snprintf(msg, sizeof(msg), "registerPluginID: %s", id);
    Log(msg);
}

int ts3plugin_requestAutoload()
{
    /* 1 = load this plugin on client start without asking */
    return 1;
}

/* ------------------------------------------------------------------ */
/* plugin menu helpers                                                 */
/* ------------------------------------------------------------------ */

/* Menu item captions.
 *
 * IMPORTANT: these strings are only ever written while we still own the
 * PluginMenuItem structs, i.e. inside ts3plugin_initMenus. The client takes
 * ownership of that memory and releases it through ts3plugin_freeMemory once
 * the menu has been built, so writing to item->text later (after a click)
 * touches freed memory and crashes the client. That is why the captions are
 * neutral action labels and the live switch states are shown by the dedicated
 * MENU_ID_SHOW_STATE item instead. */
static const char* const kMenuTexts[5] = {
    "显示/隐藏浮窗",                                                                /* 0 */
    "仅游戏内显示：开/关",                                                          /* 1 */
    "锁定/解锁浮窗位置",                                                            /* 2 */
    "重置浮窗位置",                                                                 /* 3 */
    "打开设置（查看/切换全部开关）…",                                                /* 4 */
};

/* Filled in by ts3plugin_initMenus (defined below the menu captions) and
 * cleared again by ts3plugin_freeMemory once the client releases the memory. */
static struct PluginMenuItem** g_menuProbe = NULL;

void ts3plugin_initMenus(struct PluginMenuItem*** menuItems, char** menuIcon)
{
    Log("initMenus called");
    /* The array handed to the client must be NULL-terminated, so allocate
     * count + 1 entries and leave the last one NULL. */
    const int count = 5;
    *menuItems = (struct PluginMenuItem**)calloc((size_t)count + 1, sizeof(struct PluginMenuItem*));
    if (!*menuItems) {
        Log("initMenus: array allocation failed");
        return;
    }

    static const int kIds[5] = { MENU_ID_TOGGLE_VISIBLE, MENU_ID_ONLY_IN_GAME,
                                 MENU_ID_TOGGLE_LOCK, MENU_ID_RESET_POSITION,
                                 MENU_ID_SHOW_STATE };

    for (int i = 0; i < count; ++i) {
        struct PluginMenuItem* item =
            (struct PluginMenuItem*)calloc(1, sizeof(struct PluginMenuItem));
        if (!item) {
            Log("initMenus: item allocation failed");
            return;
        }
        item->type = PLUGIN_MENU_TYPE_GLOBAL;
        item->id   = kIds[i];
        /* text and icon must always be NUL-terminated strings; an empty string
         * (not empty memory) is what "no icon" means. These are the only writes
         * we are allowed to make to this memory - see kMenuTexts. */
        strncpy(item->text, kMenuTexts[i], PLUGIN_MENU_BUFSZ - 1);
        item->text[PLUGIN_MENU_BUFSZ - 1] = '\0';
        item->icon[0] = '\0';
        (*menuItems)[i] = item;
    }
    (*menuItems)[count] = NULL;

    /* NULL means "no plugin icon"; the sample allocates a buffer instead. */
    if (menuIcon) *menuIcon = NULL;

    g_menuProbe = *menuItems;

    Log("initMenus: 5 global items created");
}

/* The array we handed to the client. Kept for diagnostics ONLY: the client
 * takes ownership of this memory and returns it through ts3plugin_freeMemory,
 * so it must never be written after initMenus returns. */
/* The client releases buffers it got from the plugin through this callback. */
void ts3plugin_freeMemory(void* data)
{
    if (data && (data == (void*)g_menuProbe)) {
        Log("freeMemory: client released the MENU ARRAY (menu memory is gone)");
        g_menuProbe = NULL;
    }
    free(data);
}

void ts3plugin_onMenuItemEvent(uint64 serverConnectionHandlerID, enum PluginMenuType type,
                               int menuItemID, uint64 selectedItemID)
{
    (void)serverConnectionHandlerID;
    (void)selectedItemID;
    {
        char msg[128];
        snprintf(msg, sizeof(msg), "onMenuItemEvent: type=%d id=%d", (int)type, menuItemID);
        Log(msg);
    }
    if (type != PLUGIN_MENU_TYPE_GLOBAL) return;

    switch (menuItemID) {
    case MENU_ID_TOGGLE_VISIBLE:
        ts3ovl::OverlayToggleVisible();
        break;
    case MENU_ID_ONLY_IN_GAME:
        ts3ovl::OverlayToggleOnlyInGame();
        break;
    case MENU_ID_TOGGLE_LOCK:
        ts3ovl::OverlayToggleLock();
        break;
    case MENU_ID_RESET_POSITION:
        ts3ovl::OverlayResetPosition();
        break;
    case MENU_ID_SHOW_STATE: {
        /* Opens the settings window: it is the only place where the live
         * switch states can be shown, because the menu structs are already
         * freed by the client and the SDK has no checked-menu API. The window
         * runs on its own thread so this handler returns immediately. */
        ts3ovl::SettingsShowAsync();
        break;
    }
    }
    /* NOTE: never write to the menu item structs here - the client freed them
     * right after building the menu (see kMenuTexts). */
}

/* ------------------------------------------------------------------ */
/* client callbacks                                                    */
/* ------------------------------------------------------------------ */
void ts3plugin_onTalkStatusChangeEvent(uint64 serverConnectionHandlerID, int status,
                                       int isReceivedWhisper, anyID clientID)
{
    (void)isReceivedWhisper;
    {
        std::lock_guard<std::mutex> lk(g_lock);
        if (g_handler != serverConnectionHandlerID) return;
        if (status == STATUS_NOT_TALKING)
            g_talking.erase(clientID);
        else
            g_talking.insert(clientID);
    }
    Refresh();
}

void ts3plugin_onConnectStatusChangeEvent(uint64 serverConnectionHandlerID, int newStatus,
                                          unsigned int errorNumber)
{
    (void)errorNumber;
    {
        std::lock_guard<std::mutex> lk(g_lock);
        if (newStatus < STATUS_CONNECTION_ESTABLISHED) {
            if (g_handler == serverConnectionHandlerID) {
                g_talking.clear();
                g_names.clear();
                g_avatars.clear();
                g_noAvatar.clear();
        g_avatarAsked.clear();
                g_channel = 0;
                g_handler = 0;
            }
        } else {
            g_handler = serverConnectionHandlerID;
        }
    }
    if (newStatus >= STATUS_CONNECTION_ESTABLISHED)
        RefreshOwnChannel(serverConnectionHandlerID);
    Refresh();
}

void ts3plugin_onClientMoveEvent(uint64 serverConnectionHandlerID, anyID clientID,
                                 uint64 oldChannelID, uint64 newChannelID,
                                 int visibility, const char* moveMessage)
{
    (void)oldChannelID; (void)newChannelID; (void)visibility; (void)moveMessage;
    {
        std::lock_guard<std::mutex> lk(g_lock);
        g_talking.erase(clientID);
        g_names.erase(clientID);
    }
    RefreshOwnChannel(serverConnectionHandlerID);
    Refresh();
}

void ts3plugin_onClientMoveMovedEvent(uint64 serverConnectionHandlerID, anyID clientID,
                                      uint64 oldChannelID, uint64 newChannelID, int visibility,
                                      anyID moverID, const char* moverName,
                                      const char* moverUniqueIdentifier, const char* moveMessage)
{
    (void)moverID;
    (void)moverName;
    (void)moverUniqueIdentifier;
    ts3plugin_onClientMoveEvent(serverConnectionHandlerID, clientID, oldChannelID, newChannelID,
                                visibility, moveMessage);
}

void ts3plugin_onClientKickFromChannelEvent(uint64 serverConnectionHandlerID, anyID clientID,
                                            uint64 oldChannelID, uint64 newChannelID, int visibility,
                                            anyID kickerID, const char* kickerName,
                                            const char* kickerUniqueIdentifier, const char* kickMessage)
{
    (void)kickerID; (void)kickerName; (void)kickerUniqueIdentifier; (void)kickMessage;
    ts3plugin_onClientMoveEvent(serverConnectionHandlerID, clientID, oldChannelID, newChannelID,
                                visibility, NULL);
}

void ts3plugin_onClientKickFromServerEvent(uint64 serverConnectionHandlerID, anyID clientID,
                                           uint64 oldChannelID, uint64 newChannelID, int visibility,
                                           anyID kickerID, const char* kickerName,
                                           const char* kickerUniqueIdentifier, const char* kickMessage)
{
    (void)kickerID; (void)kickerName; (void)kickerUniqueIdentifier; (void)kickMessage;
    ts3plugin_onClientMoveEvent(serverConnectionHandlerID, clientID, oldChannelID, newChannelID,
                                visibility, NULL);
}

void ts3plugin_onClientMoveTimeoutEvent(uint64 serverConnectionHandlerID, anyID clientID,
                                        uint64 oldChannelID, uint64 newChannelID, int visibility,
                                        const char* timeoutMessage)
{
    (void)timeoutMessage;
    ts3plugin_onClientMoveEvent(serverConnectionHandlerID, clientID, oldChannelID, newChannelID,
                                visibility, NULL);
}

void ts3plugin_onUpdateClientEvent(uint64 serverConnectionHandlerID, anyID clientID,
                                   anyID invokerID, const char* invokerName,
                                   const char* invokerUniqueIdentifier)
{
    (void)invokerID; (void)invokerName; (void)invokerUniqueIdentifier;
    (void)serverConnectionHandlerID;
    {
        std::lock_guard<std::mutex> lk(g_lock);
        g_names.erase(clientID);   /* nickname may have changed */
    }
    Refresh();
}

void ts3plugin_onClientDisplayNameChanged(uint64 serverConnectionHandlerID, anyID clientID,
                                          const char* displayName,
                                          const char* uniqueClientIdentifier)
{
    (void)displayName; (void)uniqueClientIdentifier;
    (void)serverConnectionHandlerID;
    {
        std::lock_guard<std::mutex> lk(g_lock);
        g_names.erase(clientID);
    }
    Refresh();
}

void ts3plugin_currentServerConnectionChanged(uint64 serverConnectionHandlerID)
{
    {
        std::lock_guard<std::mutex> lk(g_lock);
        g_handler = serverConnectionHandlerID;
        g_talking.clear();
        g_names.clear();
        g_avatars.clear();
        g_noAvatar.clear();
        g_avatarAsked.clear();
        g_channel = 0;
    }
    if (serverConnectionHandlerID) RefreshOwnChannel(serverConnectionHandlerID);
    Refresh();
}

/* The client downloaded an avatar (or dropped one). Until now the overlay only
 * had the initial-disc placeholder for that client, because getAvatar returned
 * an empty path. Remember the real file and repaint.
 * avatarPath == NULL means the avatar was deleted. */
void ts3plugin_onAvatarUpdated(uint64 serverConnectionHandlerID, anyID clientID, const char* avatarPath)
{
    (void)serverConnectionHandlerID;
    {
        std::lock_guard<std::mutex> lk(g_lock);
        g_avatars.erase(clientID);
        g_noAvatar.erase(clientID);
        if (avatarPath && *avatarPath) {
            /* cache straight away so the next Refresh picks it up without
             * another round-trip through getAvatar */
            std::wstring w = Utf8ToWide(avatarPath);
            if (!w.empty() && GetFileAttributesW(w.c_str()) != INVALID_FILE_ATTRIBUTES)
                g_avatars[clientID] = w;
        }
    }
    char msg[512];
    snprintf(msg, sizeof(msg), "onAvatarUpdated: client=%u path=%s",
             (unsigned)clientID, (avatarPath && *avatarPath) ? avatarPath : "(none)");
    Log(msg);
    Refresh();
}

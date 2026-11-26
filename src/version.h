/*
 * ts3-speaking-overlay - always-on-top "who is talking" overlay (KOOK style)
 * version.h : the single place the version string lives
 *
 * The value can be overridden at build time, e.g. -DTS3OVL_VERSION='"1.2.0"',
 * which is what the release script does for pre-releases.
 */
#ifndef TS3OVL_VERSION_H
#define TS3OVL_VERSION_H

#ifndef TS3OVL_VERSION
#define TS3OVL_VERSION "1.2.0"
#endif

/* Same string as a wide literal, for the Win32 APIs. */
#define TS3OVL_VERSION_W L"" TS3OVL_VERSION

#endif /* TS3OVL_VERSION_H */

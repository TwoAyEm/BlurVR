// Module location, log file and logging.
#pragma once
#include "common.h"

// Version of the mod, shown in the log.
#define BLURVR_VERSION "1.7.1"

// Handle of this DLL (d3d9.dll proxy).
extern HMODULE g_selfModule;
// Folder of the game executable (where the proxy lives).
extern char g_gameDir[MAX_PATH];
// The BlurVR\ folder next to it: BlurVR.ini, BlurVR.log, hidden-area meshes.
extern char g_modDir[MAX_PATH];
// BlurVR.log, or NULL when logging is off.
extern bool g_logEnabled;
extern char g_logPath[MAX_PATH];
// printf-style line to BlurVR.log (no-op while logging is off).
void Log(const char* fmt, ...);

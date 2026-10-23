// Where the display mode lives.  In VR the picture size is the VR size, kept in BlurVR.ini ([VR] Resolution); the game's own settings.xml
// keeps the display mode of the flat game, so switching to flat (Stereo=0 or Enabled=0) gives back the old size.
//
// The game keeps its mode (width, height, windowed, x, y) in one element of settings.xml and reads it right after the proxy is loaded:
//  * VR start: the flat mode is copied to BlurVR\display_backup.ini and settings.xml is seeded with the VR size in a window, so the game
//    starts exactly as if that had been picked in its options;
//  * VR end (the process exits): those five values go back into settings.xml, whatever the game wrote there meanwhile (its other
//    settings, V-Sync and so on, stay as the game wrote them);
//  * a start that finds the last VR run still marked open (the game was killed, there was no exit) puts the flat mode back first, VR or not;
//  * a size picked in the game's video options while VR runs is written to BlurVR.ini as the new Resolution.
#pragma once
#include "common.h"

// DllMain, after LoadConfig and the log are up and before the game reads its settings.
void VideoSettingsStart();
// DLL_PROCESS_DETACH of a process that exits.
void VideoSettingsEnd();
// The game is about to switch to this mode (CreateDevice / Reset), one of the modes it offers: in VR it becomes the Resolution.
void VideoSettingsNoteMode(UINT width, UINT height);

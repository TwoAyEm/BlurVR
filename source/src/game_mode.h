// Menu / race detection: the game opening files below "levels\" tells us which scene is loading.
#pragma once
#include "common.h"

extern volatile bool g_levelFilesSeen;
// 0 = detect automatically, 1 = force menu, 2 = force race (start mode from BlurVR.ini).
extern int g_modeOverride;
extern volatile bool g_raceStartPhase;
extern unsigned long long g_raceStartTime;
extern unsigned long long g_levelLoadStartTime; // when the game last started opening a different level
extern volatile bool g_levelLoading;
extern unsigned g_sceneProbeDraws;
extern int g_sceneProbeFrames;
// True while a 3D scene (a race or the showroom) is on screen; false during loading screens.
bool IsRaceMode();
// True while the front-end menus are up: no level has been opened yet, or the last one is the showroom.  Works with or without VR (it only needs the
// file-open hooks).  A pause menu inside a race is not recognised.  g_modeOverride (F8, StartMode) forces it either way.
bool InMenuScene();
// Name of the level most recently opened by the game ("showroom" is the menu backdrop).
extern char g_currentLevel[64];
// Movies (the tutorial videos and cutscenes, UI\movies\*.bik): the game opens the file, then stops drawing its 3D scene
// and plays the video full screen.  Called once per presented frame (3D scene on screen or not).
void UpdateMovieState(bool sceneDrawn);
// True while such a movie fills the screen: the UI the game keeps drawing under it must stay hidden.
bool IsMovieFrame();
// When a photo from the game's photo folder was last opened (the main menu's photo viewer loads it before it draws it); 0 = never.
extern volatile unsigned long long g_photoOpenTime;
// Hook CreateFileA/W to learn which level the game is loading.
void InstallFileOpenHooks();

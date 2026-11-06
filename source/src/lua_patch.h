// A patch of the game's fan screen script for levels past 25 (NOTES.md section 31), made as the game's Lua loads it.
#pragma once
#include "common.h"

// Hook the game's Lua loader (found by its bytes).  False when the loader is not found (yet); true when it is hooked (also when it already was).
bool InstallLuaChunkPatch();

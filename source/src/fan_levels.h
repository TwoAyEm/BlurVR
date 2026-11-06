// Fan levels past the game's 25 ([Fans] MaxLevel, NOTES.md section 31).
#pragma once

// Start the background thread that finds the game's fan-level table in memory once it is loaded and replaces it with a longer one.  Does nothing when
// [Fans] MaxLevel is 25 or less.  Safe to call more than once.
void StartFanLevels();

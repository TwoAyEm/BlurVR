// The car the game's chase camera follows, for the cockpit view (NOTES.md section 29).  The chase camera object keeps the transform of the car it follows (a
// position and a rotation quaternion); a hook on its per-frame update reads it, and the cockpit view puts the head at a seat in that car's frame instead of at
// the chase camera, which lags, swings and pulls back with the speed.
#pragma once
#include "common.h"
#include "matrix.h"

// Called once per presented frame.  With the cockpit view wanted (and VR running) it looks for the chase camera's update in Blur.exe by its bytes and hooks it.
void ChaseCameraPoll();
// The speed of the car the chase camera follows in metres a second (0 while the chase camera is not the one updating); the cockpit seat is also taken once the car moves.
double ChaseCameraSpeed();
// The speed of the car the chase camera follows in metres a second (0 while the chase camera is not the one updating); the cockpit seat is also taken once the car moves.
double ChaseCameraSpeed();
// True once the update is hooked: the cockpit seat is then placed in the car's frame (CarSeatDelta); where it is not (another build) the cockpit falls back to a
// fixed offset from the chase camera.
bool ChaseCameraHooked();
// The game has just given the main view its camera (game_camera.cpp): read the car's pose again and rebuild g_vr.eyeCameraDelta (VrComposeSeat) from the two.
void ChaseCameraRefresh();
// True while the cockpit seat is the view of this frame's main camera (the draws near the car are then held to its body, see below).
extern bool g_cockpitSeatOn;
// The car's own draws held to the body (always on in the cockpit view): the game draws the chassis a little off the physics body the seat is placed on, springing back after a bump, so the
// cabin moved against the view on rough ground.  A draw of the car near its middle line is given the place it has against the body when the car rides level, carried to
// the body of this frame.  The first takes the draw's world-view matrix, the second its world matrix; each gives the matrix to use in its place and returns true, or
// returns false (not the car, or the cockpit seat is not on) and the draw is left as it is.
bool ChaseCameraPinWorldView(IDirect3DDevice9* device, int shader, const Matrix4& worldView, Matrix4& out);
// (`skinned`: the draw's shader has a bone palette, as the driver's has; with CockpitDriver=0 such a draw is shrunk to nothing, as its animation moves against the car.)
bool ChaseCameraPinWorld(IDirect3DDevice9* device, int shader, const Matrix4& world, bool skinned, Matrix4& out);
// The seat is kept for the car now driven (BlurVR_seats.ini, found by the sizes of its mesh buffers); False when the car is not known yet.
bool ChaseCameraSaveSeat();
// The transform from the game camera's space to the seat's space (row vectors, like g_vr.eyeCameraDelta; the head's tracking goes after it).  `seat` is the
// seat's place in the car's frame in metres: x right, y up, z forward.  False when the chase camera has not been updated lately (another camera is in use,
// or no race) or the car's axes could not be matched to the camera's.
bool CarSeatDelta(const double seat[3], Matrix4& cameraToSeat);

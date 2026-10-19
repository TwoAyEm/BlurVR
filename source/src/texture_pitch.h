// Lock pitch of the textures that SharedTransport moved out of the managed pool.
//
// An Ex device has no managed pool, so the game's managed textures become default-pool dynamic ones (device_hooks.cpp).  Their small
// mip levels of uncompressed formats are locked with a row pitch of at least 128 bytes, where a managed texture gave the tight pitch
// (width * bytes per pixel).  A texture loader that fills a level assuming the tight pitch then writes its rows in the wrong places,
// and the small mips - the ones seen from far away - turn into noise.  The lock of such a level is answered with a staging buffer
// that has the tight pitch; on unlock it is copied into the real surface.
#pragma once
#include "common.h"

// Called for every texture the game creates through the hooked device: remapped = it was a managed texture turned into a default-pool one.
void NoteTextureCreated(IDirect3DTexture9* texture, UINT width, UINT height, D3DFORMAT format, bool remapped);
void NoteCubeTextureCreated(IDirect3DCubeTexture9* texture, UINT edge, D3DFORMAT format, bool remapped);
void NoteVolumeTextureCreated(IDirect3DVolumeTexture9* texture, UINT width, UINT height, UINT depth, D3DFORMAT format,
                              bool remapped);
// Frees the staging buffers of locks that were never unlocked (device Reset).
void ReleaseTexturePitchState();

// Luanti "fx" fork: deterministic test mode
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Used by util/fx/test/run.py. Off unless fx_test_capture is set.
//
//   fx_test_capture = <path.png>   capture one frame there, then quit
//   fx_test_fixed_dtime = true     every frame advances by exactly 1/60 s
//   fx_test_wait_ready = true      wait until a mod sets fx_test_ready = true
//                                  (the test scene mods do, once built)
//   fx_test_settle_frames = 30     ...and mesh generation has been idle that long
//   fx_test_max_wait_frames = 240  ...or this many frames after the flag
//   fx_test_min_frames = 60        never capture before this frame
//   fx_test_set = <frame>:<key>=<value>;...   change settings at those frames
//                                  (counted from the "ready" frame)
//   fx_test_rejoin = 1             after the capture, leave and join again this
//                                  many times; the last join captures
//
// Next to the PNG, <path>.json gets frame count, wall time and CPU frame-time
// percentiles over the frames after the scene settled.

#pragma once

#include "irrlichttypes.h"

namespace video { class IVideoDriver; }

namespace fx::test
{

bool active();

/// Game loop, right after the frame limiter: pin dtime in fixed-step mode.
void beginFrame(f32 &dtime);

/// Mesh generation (any thread): note that the map is still changing.
void noteMeshGenerated();

/// Just before endScene(). Hides HUD/chat in test mode, counts frames,
/// captures and ends the session when the scene is ready.
void endFrame(video::IVideoDriver *driver, bool &show_hud, bool &show_chat);

/// Client launcher: start the game again instead of quitting (rejoin test).
bool wantsRejoin();

/// fx::init (each Game): a new join starts counting from zero.
void onGameStart();

/// Frames since the scene settled (meshing idle, light volume current), or
/// -1 before that / outside test mode. GPU particles prewarm then.
int framesSinceReady();

} // namespace fx::test

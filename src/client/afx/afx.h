// Luanti "afx" fork: environmental audio, main-thread side
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Copies the map around the listener into an acoustic VoxelSnapshot at
// afx_probe_interval and hands it to the sound thread, which does the rest
// (see client/sound/sound_manager_afx.cpp). Client-only: no protocol, map or
// node-definition changes.

#pragma once

#include "client/sound.h"
#include "afx_types.h"
#include <memory>
#include <string>

class Client;
namespace video { class IVideoDriver; }
class ISoundManager;
class NodeDefManager;
struct ContentFeatures;

namespace afx
{

/// Register settings callbacks; call once at client start.
void init();
void deinit();

const Params &params();

/// Acoustic material of a node, from its groups, drawtype and name.
/// Works for Mineclonia/VoxeLibre groups (pickaxey, axey, ...) and Minetest
/// Game ones (cracky, choppy, ...).
Material classifyNode(const ContentFeatures &f);

/// Call every frame with the listener (camera) position in node space.
void update(Client *client, ISoundManager *sound, v3f ear, float dtime);

struct Mood;
/// Smoothed tension and cozy (see afx_mood.h); for other client systems
/// such as the fx renderer.
const Mood &mood();

/// One line for the F5 debug text, or "" when afx_debug is off.
std::string debugLine(ISoundManager *sound);

/// Weather from the server (TOCLIENT_AFX_WORLD): "key=value ..."
void setWorldState(const std::string &state);

/// Knock to find caves: the extra hollow layer for punching `node` from the
/// side of `normal` (pointing out of the struck face). Empty spec when the
/// wall is solid or afx is off. Cached per node and face.
SoundSpec knockSound(Client *client, v3s16 node, v3s16 normal);

/// The tuning view (afx_debug_view): the probe's rays, every playing sound
/// with its occlusion and mix, the walls and the way out. 3D lines go in
/// the scene pass, labels in the HUD pass.
void drawDebugView3D(Client *client, video::IVideoDriver *driver);
void drawDebugView2D(Client *client, video::IVideoDriver *driver);
/// The latest snapshot and where it was taken from
std::shared_ptr<const VoxelSnapshot> debugSnapshot(v3f *ear);

/// A moving object's velocity for its sounds (node space): from how its
/// drawn position actually moves, smoothed, rather than the velocity the
/// server last sent, which steps (and the Doppler shift with it). Call once
/// per frame per object with its current position.
v3f soundVelocity(u16 object_id, v3f pos_nodes, float dtime);

/// Build a snapshot (exposed for tools/tests that have a Client).
std::shared_ptr<VoxelSnapshot> buildSnapshot(Client *client, v3s16 center, s16 radius);

} // namespace afx

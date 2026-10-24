// Luanti "fx" fork: capabilities, decided once per world
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// The single answer to "is feature X on?". Worked out at world start from the
// settings and what the driver and the render pipeline can do, then frozen:
// RESTART settings changed in-game apply to the next world. Shader defines,
// pipeline steps, jitter and per-frame work all read this and nothing else.

#pragma once

#include "params.h"

namespace video { class IVideoDriver; }

namespace fx
{

struct Caps {
	bool enabled = false;       // fx at all (needs the opengl3 driver)
	bool post = false;          // enable_post_processing: render targets exist
	bool hdr = false;           // float render targets + fx_hdr_composite
	bool msaa = false, ssaa = false;
	bool taa = false;           // its resolve step is in the pipeline
	bool ao = false;
	bool surfaces = false;      // glass/water variants + scene copy
	bool colored_light = false;
	bool light_volume = false;
	bool volumetrics = false;   // the volumetric light / height fog pass
	bool gi = false;            // the screen-space bounce light pass
	bool clouds = false;        // volumetric clouds replace the engine's
	bool materials = false;     // needs array textures
	bool array_textures = false;
	bool gpu_particles = false; // particles.cpp: float targets, light volume

	std::string why_disabled;   // for the log and the tuner
};

Caps computeCaps(const Params &p, video::IVideoDriver *driver);

} // namespace fx

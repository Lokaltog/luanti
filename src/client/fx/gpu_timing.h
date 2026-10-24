// Luanti "fx" fork: GPU time per render step, shown on a profiler page (F5)
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Timestamp queries (glQueryCounter), so scopes nest freely: every pipeline
// step is one scope, and the scene draw has scopes inside it (map passes,
// shadows, particles). Results are read a few frames late, never stalling
// the GPU, and smoothed. Costs nothing while the page is not shown.

#pragma once

#include <string>

namespace fx::gpu
{

// on while the GPU profiler page is shown
void setEnabled(bool on);
bool enabled();

// Bracket one frame (RenderingCore::draw)
void frameBegin();
void frameEnd();

// A named span; nests. Names must be string literals or otherwise outlive
// the frame (step names are interned).
class Scope
{
public:
	explicit Scope(const char *name);
	explicit Scope(const std::string &name);
	~Scope();
	Scope(const Scope &) = delete;
	Scope &operator=(const Scope &) = delete;
private:
	int m_index = -1;
};

// The table for the profiler page
std::string report();

// a stable copy of a name for Scope (interned for the program's lifetime)
const char *intern(const std::string &name);

} // namespace fx::gpu

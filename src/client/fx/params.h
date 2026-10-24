// Luanti "fx" fork: parameters, generated from params.def
// SPDX-License-Identifier: LGPL-2.1-or-later

#pragma once

#include "irrlichttypes.h"
#include <string>
#include <vector>

class Settings;

namespace fx
{

enum class Apply : u8 { LIVE, SHADERS, RESTART };

/// Every fx_* tunable, by value. Members are named like the settings
/// without the fx_ prefix.
struct Params {
#define FX_BOOL(name, def, apply) bool name = def;
#define FX_INT(name, def, lo, hi, apply) int name = def;
#define FX_FLOAT(name, def, lo, hi, apply, tune, label) float name = def;
#include "params.def"
#undef FX_BOOL
#undef FX_INT
#undef FX_FLOAT
};

enum class ParamType : u8 { Bool, Int, Float };

/// Registry entry, one per params.def line.
struct ParamInfo {
	const char *name;       // without the fx_ prefix
	std::string setting;    // fx_<name>
	std::string glsl;       // fx<CamelName>, floats only
	ParamType type;
	Apply apply;
	bool tune;              // tuner slider and mood key
	float def, min, max;
	const char *label;      // floats only
	size_t offset;          // into Params
	int slot = -1;          // index into the FxParams block (floats only)
};

const std::vector<ParamInfo> &paramInfo();
const ParamInfo *findParam(const std::string &setting);

/// Number of vec4s in the FxParams uniform block.
size_t paramBlockVec4s();
/// Pack the float params into the FxParams block layout (std140 vec4 array).
void packParamBlock(const Params &p, std::vector<float> &out);
/// GLSL for fx_generated/params.glsl: the block and one accessor per float.
std::string paramBlockGlsl();

/// Read every fx_* setting.
Params readParams(const Settings &s);

float getFloat(const Params &p, const ParamInfo &info);
void setFloat(Params &p, const ParamInfo &info, float v);

} // namespace fx

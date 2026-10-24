// Luanti "fx" fork: parameters, generated from params.def
// SPDX-License-Identifier: LGPL-2.1-or-later

#include "params.h"

#include "settings.h"
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <sstream>

namespace fx
{

namespace
{

std::string camel(const char *snake)
{
	std::string out = "fx";
	bool up = true;
	for (const char *c = snake; *c; c++) {
		if (*c == '_') {
			up = true;
			continue;
		}
		out += up ? (char)std::toupper((unsigned char)*c) : *c;
		up = false;
	}
	return out;
}

constexpr bool TUNE = true, NOTUNE = false;

std::vector<ParamInfo> build()
{
	std::vector<ParamInfo> v;
	int slot = 0;
#define FX_BOOL(name, def, apply) \
	v.push_back({#name, "fx_" #name, "", ParamType::Bool, Apply::apply, false, \
			(def) ? 1.f : 0.f, 0.f, 1.f, "", offsetof(Params, name)});
#define FX_INT(name, def, lo, hi, apply) \
	v.push_back({#name, "fx_" #name, "", ParamType::Int, Apply::apply, false, \
			(float)(def), (float)(lo), (float)(hi), "", offsetof(Params, name)});
#define FX_FLOAT(name, def, lo, hi, apply, tune, label) \
	v.push_back({#name, "fx_" #name, camel(#name), ParamType::Float, Apply::apply, tune, \
			def, lo, hi, label, offsetof(Params, name), slot++});
#include "params.def"
#undef FX_BOOL
#undef FX_INT
#undef FX_FLOAT
	return v;
}

} // namespace

const std::vector<ParamInfo> &paramInfo()
{
	static const std::vector<ParamInfo> v = build();
	return v;
}

const ParamInfo *findParam(const std::string &setting)
{
	for (const auto &i : paramInfo())
		if (i.setting == setting)
			return &i;
	return nullptr;
}

size_t paramBlockVec4s()
{
	size_t floats = 0;
	for (const auto &i : paramInfo())
		floats += i.type == ParamType::Float;
	return (floats + 3) / 4;
}

float getFloat(const Params &p, const ParamInfo &info)
{
	const char *base = reinterpret_cast<const char *>(&p) + info.offset;
	switch (info.type) {
	case ParamType::Bool: return *reinterpret_cast<const bool *>(base) ? 1.f : 0.f;
	case ParamType::Int: return (float)*reinterpret_cast<const int *>(base);
	case ParamType::Float: return *reinterpret_cast<const float *>(base);
	}
	return 0.f;
}

void setFloat(Params &p, const ParamInfo &info, float v)
{
	char *base = reinterpret_cast<char *>(&p) + info.offset;
	switch (info.type) {
	case ParamType::Bool: *reinterpret_cast<bool *>(base) = v != 0.f; break;
	case ParamType::Int: *reinterpret_cast<int *>(base) = (int)std::lround(v); break;
	case ParamType::Float: *reinterpret_cast<float *>(base) = v; break;
	}
}

void packParamBlock(const Params &p, std::vector<float> &out)
{
	out.assign(paramBlockVec4s() * 4, 0.f);
	for (const auto &i : paramInfo())
		if (i.slot >= 0)
			out[i.slot] = getFloat(p, i);
}

std::string paramBlockGlsl()
{
	std::ostringstream os;
	os << "// generated from src/client/fx/params.def\n"
	   << "layout(std140) uniform FxParams {\n\tvec4 fxP[" << paramBlockVec4s() << "];\n};\n";
	static const char comp[] = "xyzw";
	for (const auto &i : paramInfo())
		if (i.slot >= 0)
			os << "#define " << i.glsl << " fxP[" << i.slot / 4 << "]." << comp[i.slot % 4] << "\n";
	return os.str();
}

Params readParams(const Settings &s)
{
	Params p;
	for (const auto &i : paramInfo()) {
		switch (i.type) {
		case ParamType::Bool: setFloat(p, i, s.getBool(i.setting) ? 1.f : 0.f); break;
		case ParamType::Int:
		case ParamType::Float: {
			// settings files and mods can hold anything: clamp to the
			// registry's range (a huge gather radius would stall every frame)
			float v = i.type == ParamType::Int ? (float)s.getS32(i.setting) : s.getFloat(i.setting);
			if (!std::isfinite(v))
				v = i.def;
			setFloat(p, i, std::clamp(v, i.min, i.max));
			break;
		}
		}
	}
	return p;
}

} // namespace fx

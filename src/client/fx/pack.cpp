// Luanti "fx" fork: texture-pack data (companions, light colours, surfaces)
// SPDX-License-Identifier: LGPL-2.1-or-later

#include "renderer.h"

#include "client/texturepaths.h"
#include "client/texturesource.h"
#include "filesys.h"
#include "log.h"
#include "nodedef.h"
#include "util/string.h"
#include <IImage.h>
#include <algorithm>
#include <fstream>
#include <regex>
#include <sstream>

namespace fx
{

/*
	Companion textures
*/

// A texture atom: a plain file name, without escapes or modifiers.
static const std::regex s_atom_re(R"(([A-Za-z0-9_.\-]+)\.png)");

static float autoEmissiveThreshold()
{
	return params().auto_emissive_threshold;
}

std::string companionName(ITextureSource *tsrc, const std::string &image,
		bool auto_emissive, core::dimension2du size)
{
	// split off a trailing mesh filter, re-applied at the end
	std::string base = image;
	std::string suffix;
	const std::string &filter = ITextureSource::FILTER_FOR_MESH;
	if (!filter.empty() && str_ends_with(base, filter)) {
		base.resize(base.size() - filter.size());
		suffix = filter;
	}

	bool any_authored = false;
	std::string out;
	out.reserve(base.size() * 2);
	size_t last = 0;
	for (auto it = std::sregex_iterator(base.begin(), base.end(), s_atom_re);
			it != std::sregex_iterator(); ++it) {
		const auto &m = *it;
		out.append(base, last, m.position() - last);
		const std::string authored = m[1].str() + "_e.png";
		// atoms inside [combine etc. are escaped: only rewrite top-level ones
		const bool top_level = m.position() == 0 || base[m.position() - 1] == '^' ||
				base[m.position() - 1] == '(';
		if (top_level && tsrc->isKnownSourceImage(authored)) {
			out += authored;
			any_authored = true;
		} else if (top_level) {
			out += "(" + m.str() + "^[multiply:#000000)"; // keep alpha, drop colour
		} else {
			out += m.str();
		}
		last = m.position() + m.length();
	}
	out.append(base, last, std::string::npos);

	std::string result;
	if (any_authored) {
		result = out;
	} else if (auto_emissive) {
		std::ostringstream os;
		os << "(" << base << ")^[fx_autoemissive:" << autoEmissiveThreshold();
		result = os.str();
	} else {
		result = "(" + base + ")^[multiply:#000000";
	}

	if (size.Width && size.Height) {
		auto csize = tsrc->getTextureDimensions(result);
		if (csize != size) {
			std::ostringstream os;
			os << "(" << result << ")^[resize:" << size.Width << "x" << size.Height;
			result = os.str();
		}
	}
	verbosestream << "fx: companion of \"" << image << "\" = \"" << result + suffix << "\"" << std::endl;
	return result + suffix;
}

video::ITexture *entityEmissive(ITextureSource *tsrc, const std::string &image)
{
	if (!caps().enabled || image.empty())
		return nullptr;
	bool any = false;
	for (auto it = std::sregex_iterator(image.begin(), image.end(), s_atom_re);
			it != std::sregex_iterator() && !any; ++it)
		any = tsrc->isKnownSourceImage((*it)[1].str() + "_e.png");
	if (!any)
		return nullptr;
	return tsrc->getTextureForMesh(companionName(tsrc, image, false,
			tsrc->getTextureDimensions(image)));
}

bool materialLayers(ITextureSource *tsrc, const std::string &image,
		core::dimension2du size, std::string &normal, std::string &specular)
{
	// base atom: the first plain file name at the start of the string
	std::smatch m;
	std::string stem;
	if (std::regex_search(image, m, s_atom_re) && m.position() == 0)
		stem = m[1].str();
	auto make = [&] (const char *suffix, const char *neutral, std::string &out) {
		const std::string authored = stem + suffix;
		std::ostringstream os;
		if (!stem.empty() && tsrc->isKnownSourceImage(authored)) {
			os << "(" << authored << ")^[resize:" << size.Width << "x" << size.Height;
			out = os.str();
			return true;
		}
		os << "[fill:" << size.Width << "x" << size.Height << ":" << neutral;
		out = os.str();
		return false;
	};
	// LabPBR neutrals: flat normal, no AO, top height / rough, dielectric, no emission
	const bool n = make("_n.png", "#8080ffff", normal);
	const bool s = make("_s.png", "#000000ff", specular);
	return n || s;
}

video::ITexture *neutralMaterialTexture(ITextureSource *tsrc)
{
	Renderer *r = renderer();
	if (!r)
		return nullptr;
	if (!r->neutral_material)
		r->neutral_material = tsrc->addArrayTexture({"[fill:1x1:#8080ffff", "[fill:1x1:#000000ff"});
	return r->neutral_material;
}

void applyAutoEmissive(video::IImage *image, float threshold)
{
	auto dim = image->getDimension();
	const float t0 = std::clamp(threshold, 0.f, 0.99f);
	for (u32 y = 0; y < dim.Height; y++)
	for (u32 x = 0; x < dim.Width; x++) {
		video::SColor c = image->getPixel(x, y);
		float r = c.getRed() / 255.f, g = c.getGreen() / 255.f, b = c.getBlue() / 255.f;
		// max channel, not luma: saturated flames are exactly what should glow
		float v = std::max({r, g, b});
		float k = std::clamp((v - t0) / (1.f - t0), 0.f, 1.f);
		k = k * k * (3.f - 2.f * k);
		c.setRed(core::round32(r * k * 255.f));
		c.setGreen(core::round32(g * k * 255.f));
		c.setBlue(core::round32(b * k * 255.f));
		image->setPixel(x, y, c);
	}
}

/*
	Tables: fx_pack.txt, fx_light_colors.txt and fx_materials.txt in the
	texture dirs (spec: util/fx/PACK_FORMAT.md)
*/

static constexpr int PACK_FORMAT_MAX = 2;

static std::vector<std::string> tokens(const std::string &line)
{
	std::vector<std::string> out;
	std::istringstream is(line);
	std::string t;
	while (is >> t)
		out.push_back(t);
	return out;
}

// fx_pack.txt: `key = value`; only `format` changes behaviour
static int readPackFormat(const std::string &dir)
{
	const std::string path = dir + DIR_DELIM + "fx_pack.txt";
	std::ifstream is(path);
	if (!is.good())
		return 1;
	int format = 1;
	std::string line;
	int lineno = 0;
	while (std::getline(is, line)) {
		lineno++;
		const std::string t(trim(line));
		if (t.empty() || t[0] == '#')
			continue;
		const size_t eq = t.find('=');
		if (eq == std::string::npos) {
			warningstream << path << ":" << lineno << ": expected <key> = <value>" << std::endl;
			continue;
		}
		const std::string key(trim(t.substr(0, eq))), value(trim(t.substr(eq + 1)));
		if (key == "format") {
			format = mystoi(value);
			if (format < 1 || format > PACK_FORMAT_MAX) {
				warningstream << path << ":" << lineno << ": format " << value
					<< " is not supported (1.." << PACK_FORMAT_MAX << "), reading as "
					<< std::clamp(format, 1, PACK_FORMAT_MAX) << std::endl;
				format = std::clamp(format, 1, PACK_FORMAT_MAX);
			}
		} else if (key != "producer") {
			infostream << path << ":" << lineno << ": ignoring key \"" << key << "\"" << std::endl;
		}
	}
	return format;
}

// `*` any run, `?` one character; the whole name must match
bool packGlobMatch(const std::string &pat, const std::string &s)
{
	size_t p = 0, i = 0, star = std::string::npos, mark = 0;
	while (i < s.size()) {
		if (p < pat.size() && (pat[p] == '?' || pat[p] == s[i])) {
			p++;
			i++;
		} else if (p < pat.size() && pat[p] == '*') {
			star = p++;
			mark = i;
		} else if (star != std::string::npos) {
			p = star + 1;
			i = ++mark;
		} else {
			return false;
		}
	}
	while (p < pat.size() && pat[p] == '*')
		p++;
	return p == pat.size();
}

// First dir wins for a key (same order as texture lookups)
template <typename T>
static bool addEntry(PackTable<T> &table, const std::string &key, PackEntry<T> e)
{
	if (key.size() > 1 && key[0] == '@')
		return table.textures.emplace(key.substr(1), std::move(e)).second;
	if (key.find_first_of("*?") != std::string::npos) {
		for (const auto &g : table.globs)
			if (g.first == key)
				return false;
		table.globs.emplace_back(key, std::move(e));
		return true;
	}
	return table.exact.emplace(key, std::move(e)).second;
}

static void loadLightColors(const std::string &path, int format, PackTables &out)
{
	std::ifstream is(path);
	std::string line;
	size_t n = 0;
	int lineno = 0;
	while (std::getline(is, line)) {
		lineno++;
		const std::string t(trim(line));
		if (t.empty() || t[0] == '#')
			continue;
		auto parts = tokens(t);
		PackEntry<LightStyle> e;
		e.where = path + ":" + std::to_string(lineno);
		if (parts.size() < 2 || !parseColorString(parts[1], e.value.color, true)) {
			warningstream << e.where << ": expected <key> <#rrggbb> [flicker=<0..4>]" << std::endl;
			continue;
		}
		bool flicker_given = false;
		for (size_t i = 2; i < parts.size(); i++) {
			if (str_starts_with(parts[i], "flicker=")) {
				e.value.flicker = std::clamp(mystof(parts[i].substr(8)), 0.f, 4.f);
				flicker_given = true;
			} else {
				warningstream << e.where << ": unknown option \"" << parts[i] << "\"" << std::endl;
			}
		}
		// v1: an entry replaces the whole style (no flicker= means none);
		// v2: no flicker= keeps the name heuristic
		e.heuristic_flicker = format >= 2 && !flicker_given;
		if (addEntry(out.lights, parts[0], std::move(e)))
			n++;
	}
	infostream << "fx: loaded " << n << " light colours from " << path << std::endl;
}

static void loadSurfaces(const std::string &path, PackTables &out)
{
	std::ifstream is(path);
	std::string line;
	size_t n = 0;
	int lineno = 0;
	while (std::getline(is, line)) {
		lineno++;
		const std::string t(trim(line));
		if (t.empty() || t[0] == '#')
			continue;
		auto parts = tokens(t);
		PackEntry<Surface> e;
		e.where = path + ":" + std::to_string(lineno);
		Surface &s = e.value;
		if (parts.size() < 2) {
			warningstream << e.where << ": expected <key> glass|water|none [pattern=...]" << std::endl;
			continue;
		}
		if (parts[1] == "glass")
			s.kind = SurfaceKind::Glass;
		else if (parts[1] == "water")
			s.kind = SurfaceKind::Water;
		else if (parts[1] != "none") {
			warningstream << e.where << ": unknown surface \"" << parts[1] << "\"" << std::endl;
			continue;
		}
		for (size_t i = 2; i < parts.size(); i++) {
			if (parts[i] == "pattern=fluted")
				s.pattern = SurfacePattern::Fluted;
			else if (parts[i] == "pattern=frosted")
				s.pattern = SurfacePattern::Frosted;
			else if (parts[i] == "pattern=rippled")
				s.pattern = SurfacePattern::Rippled;
			else
				warningstream << e.where << ": unknown option \"" << parts[i] << "\"" << std::endl;
		}
		if (addEntry(out.surfaces, parts[0], std::move(e)))
			n++;
	}
	infostream << "fx: loaded " << n << " surfaces from " << path << std::endl;
}

template <typename T>
static void sortGlobs(PackTable<T> &table)
{
	// longest pattern wins; stable, so the first dir wins among equals
	std::stable_sort(table.globs.begin(), table.globs.end(),
			[] (const auto &a, const auto &b) { return a.first.size() > b.first.size(); });
}

void loadPackTables(PackTables &out)
{
	loadPackTables(out, getTextureDirs());
}

void loadPackTables(PackTables &out, const std::vector<std::string> &dirs)
{
	out = PackTables();
	for (const auto &dir : dirs) {
		const std::string lights = dir + DIR_DELIM + "fx_light_colors.txt";
		const std::string surfaces = dir + DIR_DELIM + "fx_materials.txt";
		const bool has_lights = fs::PathExists(lights), has_surfaces = fs::PathExists(surfaces);
		if (!has_lights && !has_surfaces)
			continue;
		const int format = readPackFormat(dir);
		if (has_lights)
			loadLightColors(lights, format, out);
		if (has_surfaces)
			loadSurfaces(surfaces, out);
	}
	sortGlobs(out.lights);
	sortGlobs(out.surfaces);
}

// "foo.png" of the first top-level atom of the first non-empty tile
static std::string baseAtom(const ContentFeatures &f)
{
	for (const auto &tile : f.tiledef) {
		if (tile.name.empty())
			continue;
		std::smatch m;
		if (std::regex_search(tile.name, m, s_atom_re))
			return m.str();
		return "";
	}
	return "";
}

// exact name, then glob (longest), then @texture
template <typename T>
static const PackEntry<T> *lookup(const PackTable<T> &table, const ContentFeatures &f)
{
	const PackEntry<T> *e = nullptr;
	auto it = table.exact.find(f.name);
	if (it != table.exact.end())
		e = &it->second;
	for (size_t i = 0; !e && i < table.globs.size(); i++)
		if (packGlobMatch(table.globs[i].first, f.name))
			e = &table.globs[i].second;
	if (!e && !table.textures.empty()) {
		auto tt = table.textures.find(baseAtom(f));
		if (tt != table.textures.end())
			e = &tt->second;
	}
	if (e)
		e->used = true;
	return e;
}

const LightStyle *packFindLight(const PackTables &t, const ContentFeatures &f, bool *heuristic_flicker)
{
	const auto *e = lookup(t.lights, f);
	if (e && heuristic_flicker)
		*heuristic_flicker = e->heuristic_flicker;
	return e ? &e->value : nullptr;
}

const Surface *packFindSurface(const PackTables &t, const ContentFeatures &f)
{
	const auto *e = lookup(t.surfaces, f);
	return e ? &e->value : nullptr;
}

template <typename T>
static void reportUnused(const PackTable<T> &table, const char *what)
{
	auto report = [&] (const std::string &key, const PackEntry<T> &e) {
		if (!e.used)
			warningstream << e.where << ": " << what << " key \"" << key
				<< "\" matched no node" << std::endl;
	};
	for (const auto &it : table.exact)
		report(it.first, it.second);
	for (const auto &it : table.globs)
		report(it.first, it.second);
	for (const auto &it : table.textures)
		report("@" + it.first, it.second);
}

void reportUnusedPackKeys()
{
	Renderer *r = renderer();
	if (!r)
		return;
	// light keys only resolve for light sources, surfaces only with caps.surfaces
	reportUnused(r->pack.lights, "light (light sources only)");
	if (r->caps.surfaces)
		reportUnused(r->pack.surfaces, "surface");
}

static video::SColor normalizeChroma(video::SColor c)
{
	// brightest channel to 255: only the hue matters, the intensity comes from
	// the engine's occlusion-correct scalar light
	u32 m = std::max({c.getRed(), c.getGreen(), c.getBlue()});
	if (m == 0)
		return video::SColor(255, 255, 255, 255);
	return video::SColor(255, c.getRed() * 255 / m, c.getGreen() * 255 / m, c.getBlue() * 255 / m);
}

static float heuristicFlicker(const std::string &n)
{
	// fire-like nodes flicker, solid glow blocks don't
	for (const char *w : {"torch", "fire", "flame", "candle"})
		if (n.find(w) != std::string::npos)
			return 1.f;
	if (n.find("lava") != std::string::npos)
		return 0.35f;
	return 0.f;
}

LightStyle resolveLightStyle(ITextureSource *tsrc, const ContentFeatures &f,
		const std::unordered_set<std::string> &glowing)
{
	Renderer *r = renderer();
	if (r) {
		if (const auto *e = lookup(r->pack.lights, f)) {
			LightStyle st = e->value;
			st.color = normalizeChroma(st.color);
			if (e->heuristic_flicker)
				st.flicker = heuristicFlicker(f.name);
			return st;
		}
	}
	LightStyle st;
	st.flicker = heuristicFlicker(f.name);

	// the tile that glows: the first one unique to light sources (a jack
	// o'lantern's face, not the pumpkin side it shares), else the first
	const std::string *pick = nullptr;
	for (const auto *defs : {&f.tiledef, &f.tiledef_overlay})
		for (const auto &tile : *defs)
			if (!pick && !tile.name.empty() && glowing.count(tile.name))
				pick = &tile.name;
	for (const auto &tile : f.tiledef)
		if (!pick && !tile.name.empty())
			pick = &tile.name;
	if (!pick)
		return st;
	// average of the glowing pixels: the authored companion when there is
	// one (premultiplied, black drops out), else [fx_autoemissive
	std::string glow = companionName(tsrc, *pick, false);
	video::SColor avg = tsrc->getTextureAverageColor(glow);
	if (avg.getRed() + avg.getGreen() + avg.getBlue() == 0) {
		std::ostringstream os;
		os << "(" << *pick << ")^[fx_autoemissive:" << autoEmissiveThreshold();
		avg = tsrc->getTextureAverageColor(os.str());
	}
	if (avg.getRed() + avg.getGreen() + avg.getBlue() > 0)
		st.color = normalizeChroma(avg);
	return st;
}

Surface resolveSurface(const ContentFeatures &f)
{
	Renderer *r = renderer();
	if (!r || !r->caps.surfaces)
		return {};
	if (const auto *e = lookup(r->pack.surfaces, f))
		return e->value;
	Surface s;
	const bool liquid_draw = f.drawtype == NDT_LIQUID || f.drawtype == NDT_FLOWINGLIQUID;
	const bool translucent = f.alpha != ALPHAMODE_OPAQUE;
	if (liquid_draw && f.isLiquid() && f.light_source == 0 && translucent) {
		s.kind = SurfaceKind::Water;
	} else if (liquid_draw && !f.isLiquid() && f.light_source == 0 && translucent) {
		// liquid drawtype on a solid (Mineclonia's ice): see-through, not water
		s.kind = SurfaceKind::Glass;
	} else if (f.drawtype == NDT_GLASSLIKE || f.drawtype == NDT_GLASSLIKE_FRAMED ||
			f.drawtype == NDT_GLASSLIKE_FRAMED_OPTIONAL ||
			(f.name.find("glass") != std::string::npos && f.light_source == 0 &&
			 f.drawtype != NDT_NORMAL)) {
		s.kind = SurfaceKind::Glass;
	}
	if (s.kind == SurfaceKind::Glass) {
		if (f.name.find("fluted") != std::string::npos || f.name.find("reeded") != std::string::npos)
			s.pattern = SurfacePattern::Fluted;
		else if (f.name.find("frosted") != std::string::npos)
			s.pattern = SurfacePattern::Frosted;
		else if (f.name.find("ripple") != std::string::npos)
			s.pattern = SurfacePattern::Rippled;
	}
	return s;
}

} // namespace fx

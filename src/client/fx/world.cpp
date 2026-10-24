// Luanti "fx" fork: the world around the camera as the server describes it
// (fx_hints mod), and camera events (lightning, damage, low health, portals)
// SPDX-License-Identifier: LGPL-2.1-or-later

#include "renderer.h"
#include "light_volume.h"
#include "fx_test.h"

#include "client/client.h"
#include "client/clientenvironment.h"
#include "client/content_cao.h"
#include "client/localplayer.h"
#include "hud_element.h"
#include "log.h"
#include "settings.h"
#include "util/string.h"
#include <algorithm>
#include <cmath>

namespace fx
{

// "fx1;dim=nether;biome=CrimsonForest;weather=none;flash=3;portal=none"
static void parseHints(const std::string &text, Hints &h)
{
	Hints out;
	out.present = true;
	out.raw = text;
	const auto parts = str_split(text, ';');
	if (parts.empty() || parts[0] != "fx1") {
		warningstream << "fx: unknown hint format \"" << text << "\"" << std::endl;
		h = out;
		return;
	}
	for (size_t i = 1; i < parts.size(); i++) {
		const auto eq = parts[i].find('=');
		if (eq == std::string::npos)
			continue;
		const std::string k = parts[i].substr(0, eq), v = parts[i].substr(eq + 1);
		if (k == "dim")
			out.dim = v;
		else if (k == "biome")
			out.biome = v;
		else if (k == "weather")
			out.weather = v;
		else if (k == "flash")
			out.flash = mystoi(v);
		else if (k == "portal")
			out.portal = v;
	}
	h = out;
}

void Renderer::updateWorld(Client *client, float dtime)
{
	LocalPlayer *player = client->getEnv().getLocalPlayer();
	if (!player)
		return;

	// 1. hints: the hidden "fx_hints" HUD text the server mod keeps current
	bool found = false;
	// test mode: hints from a setting instead of the server mod
	if (const std::string t = g_settings->get("fx_test_hints"); !t.empty()) {
		found = true;
		if (t != hints.raw)
			parseHints(t, hints);
	}
	for (const auto &e : player->hud.getElements()) {
		if (found)
			break;
		if (!e || e->name != "fx_hints")
			continue;
		found = true;
		if (e->text != hints.raw) {
			parseHints(e->text, hints);
			verbosestream << "fx: hints " << e->text << std::endl;
		}
		break;
	}
	if (!found && hints.present)
		hints = Hints(); // the mod went away

	// test mode: everything settles within a few frames
	const float time_scale = test::active() ? 0.01f : 1.f;
	auto approach = [&] (float &v, float target, float up_s, float down_s) {
		const float s = (target > v ? up_s : down_s) * time_scale;
		v += (target - v) * (1.f - std::exp(-dtime / std::max(s, 1e-3f)));
	};
	approach(events.nether, hints.dim == "nether" ? 1.f : 0.f, 1.5f, 1.5f);
	approach(events.end, hints.dim == "end" ? 1.f : 0.f, 1.5f, 1.5f);
	const bool wet = hints.weather == "rain" || hints.weather == "thunder";
	approach(events.wet, wet ? 1.f : 0.f, 20.f, 40.f); // surfaces wet up, then dry
	approach(events.storm, hints.weather == "thunder" ? 1.f : 0.f, 3.f, 6.f);
	// snow settles over a minute and melts over two; rain right now (puddle
	// ripples); wind grows with the weather and slowly turns
	approach(events.snow_cover, hints.weather == "snow" ? 1.f : 0.f, 60.f, 120.f);
	approach(events.raining, wet ? 1.f : 0.f, 1.f, 1.f);
	const float wind = hints.weather == "thunder" ? 1.8f : wet ? 1.1f :
			hints.weather == "snow" ? 0.8f : 0.5f;
	approach(events.wind, wind, 6.f, 10.f);
	events.wind_angle = std::fmod(events.wind_angle + dtime * 0.004f, 6.2831853f);
	// a portal pulls in over the ~4 s it takes, and lets go quickly
	approach(events.portal, hints.portal != "none" ? 1.f : 0.f, 2.5f, 0.4f);

	// Nether heat: how much lava light reaches the camera (red-heavy
	// artificial light in the light volume)
	float heat = 0.f;
	if (volume && hints.dim == "nether") {
		u8 l[3];
		if (volume->sampleLevels(floatToInt(player->getEyePosition(), BS), l))
			heat = std::clamp((l[0] - l[2] - 2.f) / 8.f, 0.f, 1.f);
	}
	approach(events.heat, heat, 1.0f, 2.5f);

	// Biome moods for water and sky (Mineclonia biome names; anything else
	// gets the temperate default)
	auto has = [&] (std::initializer_list<const char *> words) {
		for (const char *w : words)
			if (hints.biome.find(w) != std::string::npos)
				return true;
		return false;
	};
	const bool cold = has({"Ice", "Frozen", "Snow", "Cold", "Taiga", "Grove", "Peaks", "Slopes"});
	const bool warm = has({"Desert", "Jungle", "Savanna", "Mesa", "Badlands", "Warm", "Lukewarm", "Beach", "Bamboo"});
	const bool swamp = has({"Swamp", "Mangrove"});
	float tint[3] = {0.10f, 0.45f, 0.55f}, chop = 0.5f; // temperate: blue-green
	if (warm) { tint[0] = 0.12f; tint[1] = 0.75f; tint[2] = 0.70f; chop = 0.35f; } // turquoise
	if (cold) { tint[0] = 0.04f; tint[1] = 0.18f; tint[2] = 0.40f; chop = 0.8f; }  // dark, choppy
	if (swamp) { tint[0] = 0.22f; tint[1] = 0.26f; tint[2] = 0.10f; chop = 0.2f; } // murky
	chop += events.storm * 0.8f + (wet ? 0.2f : 0.f);
	for (int i = 0; i < 3; i++)
		approach(events.water[i], tint[i], 3.f, 3.f);
	approach(events.water[3], chop, 3.f, 3.f);
	approach(events.cold, cold ? 1.f : 0.f, 4.f, 4.f);
	approach(events.overworld, hints.dim == "overworld" || !hints.present ? 1.f : 0.f, 1.f, 1.f);
	// surfacing: water runs down the lens for a few seconds
	if (events.was_underwater && cam.underwater < 0.5f)
		events.droplets = 1.f;
	events.was_underwater = cam.underwater > 0.5f;
	events.droplets = std::max(0.f, events.droplets - dtime / 3.f);

	// 2. lightning: each new strike flashes twice, quickly
	if (hints.present && events.last_flash >= 0 && hints.flash > events.last_flash) {
		events.flash_age = 0.f;
		// where it strikes: somewhere around, rarely close (nodes)
		static u32 rng = 0x9e3779b9u;
		auto rnd = [&] { rng = rng * 1664525u + 1013904223u; return (rng >> 8) / 16777216.f; };
		const float a = rnd() * 6.2831853f, d = 50.f + rnd() * rnd() * 220.f;
		const v3f eye = player->getEyePosition() / BS;
		events.strike[0] = eye.X + std::cos(a) * d;
		events.strike[1] = eye.Z + std::sin(a) * d;
		events.strike[2] = rnd();
	}
	events.last_flash = hints.present ? hints.flash : -1;
	events.flash_age += dtime;
	// test mode: a strike pinned at "dx,dz,age" from the eye (nodes, s)
	if (test::active()) {
		const std::string ts = g_settings->get("fx_test_strike");
		const auto v = str_split(ts, ',');
		if (v.size() == 3) {
			const v3f eye = player->getEyePosition() / BS;
			events.strike[0] = eye.X + stof(v[0]);
			events.strike[1] = eye.Z + stof(v[1]);
			events.strike[2] = 0.37f;
			events.flash_age = stof(v[2]);
		}
	}
	{
		const float t = events.flash_age;
		const float a = std::exp(-t / 0.08f), b = t > 0.15f ? std::exp(-(t - 0.15f) / 0.2f) : 0.f;
		events.flash = std::clamp(a + 0.7f * b, 0.f, 1.f);
	}

	// 3. damage and low health, from the player's own hp
	const s32 hp = player->hp;
	s32 hp_max = 20;
	if (GenericCAO *cao = player->getCAO())
		hp_max = std::max<s32>(1, cao->getProperties().hp_max);
	if (events.last_hp >= 0 && hp < events.last_hp)
		events.damage = 1.f;
	events.last_hp = hp;
	events.damage *= std::exp(-dtime / 0.35f);
	const float frac = (float)hp / hp_max;
	approach(events.low_health, hp > 0 ? std::clamp((0.35f - frac) / 0.3f, 0.f, 1.f) : 0.f, 0.5f, 1.f);
}

} // namespace fx

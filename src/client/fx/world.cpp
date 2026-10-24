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

	// 2. lightning: each new strike flashes twice, quickly
	if (hints.present && events.last_flash >= 0 && hints.flash > events.last_flash)
		events.flash_age = 0.f;
	events.last_flash = hints.present ? hints.flash : -1;
	events.flash_age += dtime;
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

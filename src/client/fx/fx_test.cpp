// Luanti "fx" fork: deterministic test mode
// SPDX-License-Identifier: LGPL-2.1-or-later

#include "fx_test.h"

#include "gui/mainmenumanager.h"
#include "log.h"
#include "porting.h"
#include "settings.h"
#include "util/string.h"

#include <IVideoDriver.h>
#include <IImage.h>
#include <algorithm>
#include <atomic>
#include <fstream>
#include <vector>

namespace fx::test
{

namespace
{

struct SetAt {
	u32 frame;
	std::string key, value;
};

struct State {
	bool loaded = false;
	bool active = false;
	std::string capture;
	bool fixed_dtime = true;
	bool wait_ready = true;
	u32 settle_frames = 30;
	u32 min_frames = 60;
	u32 after_frames = 30;
	u32 max_wait_frames = 240;
	std::vector<SetAt> sets;
	int joins_left = 0;

	// per join
	u32 frame = 0;
	u32 idle = 0;
	u32 last_mesh = 0;
	s64 ready_frame = -1;
	s64 flag_frame = -1;
	u64 last_us = 0;
	u64 start_ms = 0;
	std::vector<float> frame_ms;
	bool done = false;
	bool rejoin_pending = false;
};

State s;
std::atomic<u32> s_mesh_counter{0};

void load()
{
	if (s.loaded)
		return;
	s.loaded = true;
	s.capture = g_settings->get("fx_test_capture");
	s.active = !s.capture.empty();
	if (!s.active)
		return;
	s.fixed_dtime = g_settings->getBool("fx_test_fixed_dtime");
	s.wait_ready = g_settings->getBool("fx_test_wait_ready");
	s.settle_frames = g_settings->getU32("fx_test_settle_frames");
	s.min_frames = g_settings->getU32("fx_test_min_frames");
	s.after_frames = g_settings->getU32("fx_test_after_frames");
	s.max_wait_frames = g_settings->getU32("fx_test_max_wait_frames");
	s.joins_left = g_settings->getS32("fx_test_rejoin");
	for (const auto &item : str_split(g_settings->get("fx_test_set"), ';')) {
		auto colon = item.find(':'), eq = item.find('=');
		if (colon == std::string::npos || eq == std::string::npos || eq < colon) {
			if (!trim(item).empty())
				errorstream << "fx test: bad fx_test_set entry \"" << item << "\"" << std::endl;
			continue;
		}
		s.sets.push_back({(u32)mystoi(item.substr(0, colon)),
				std::string(trim(item.substr(colon + 1, eq - colon - 1))),
				std::string(trim(item.substr(eq + 1)))});
	}
	u32 last = 0;
	for (auto &e : s.sets)
		last = std::max(last, e.frame);
	s.after_frames = std::max(s.after_frames, last + s.settle_frames);
	infostream << "fx test: capture to " << s.capture << ", joins after this: "
			<< s.joins_left << std::endl;
}

void resetJoin()
{
	s.frame = s.idle = 0;
	s.last_mesh = s_mesh_counter.load();
	s.ready_frame = -1;
	s.flag_frame = -1;
	s.last_us = 0;
	s.start_ms = porting::getTimeMs();
	s.frame_ms.clear();
	s.done = false;
}

float percentile(std::vector<float> v, float p)
{
	if (v.empty())
		return 0.f;
	std::sort(v.begin(), v.end());
	return v[std::min(v.size() - 1, (size_t)(p * (v.size() - 1) + 0.5f))];
}

void writeStats()
{
	std::ofstream f(s.capture + ".json");
	f << "{\n"
	  << "  \"frames\": " << s.frame << ",\n"
	  << "  \"ready_frame\": " << s.ready_frame << ",\n"
	  << "  \"wall_ms\": " << (porting::getTimeMs() - s.start_ms) << ",\n"
	  << "  \"frame_ms_p50\": " << percentile(s.frame_ms, 0.5f) << ",\n"
	  << "  \"frame_ms_p95\": " << percentile(s.frame_ms, 0.95f) << ",\n"
	  << "  \"frame_ms_max\": " << percentile(s.frame_ms, 1.f) << ",\n"
	  << "  \"measured_frames\": " << s.frame_ms.size() << "\n"
	  << "}\n";
}

} // namespace

bool active()
{
	load();
	return s.active;
}

void beginFrame(f32 &dtime)
{
	if (active() && s.fixed_dtime)
		dtime = 1.f / 60.f;
}

void noteMeshGenerated()
{
	s_mesh_counter.fetch_add(1, std::memory_order_relaxed);
}

void endFrame(video::IVideoDriver *driver, bool &show_hud, bool &show_chat)
{
	if (!active() || s.done)
		return;
	if (s.frame == 0)
		resetJoin();
	show_hud = false;
	show_chat = false;

	const u64 now = porting::getTimeUs();
	if (s.ready_frame >= 0 && s.last_us)
		s.frame_ms.push_back((now - s.last_us) / 1000.f);
	s.last_us = now;
	s.frame++;

	const u32 meshes = s_mesh_counter.load();
	s.idle = meshes == s.last_mesh ? s.idle + 1 : 0;
	s.last_mesh = meshes;

	if (s.ready_frame < 0) {
		const bool ready = !s.wait_ready || g_settings->getBool("fx_test_ready");
		if (s.frame % 60 == 0)
			infostream << "fx test: frame " << s.frame << ", scene ready " << ready
					<< ", meshes " << meshes << ", idle for " << s.idle << std::endl;
		if (ready && s.flag_frame < 0)
			s.flag_frame = s.frame;
		// settled: meshing idle for a while, or (liquids still flowing far
		// away, light updates) a fixed number of frames after the flag
		const bool settled = s.idle >= s.settle_frames ||
				(s.flag_frame >= 0 && s.frame - s.flag_frame >= (s64)s.max_wait_frames);
		if (ready && s.frame >= s.min_frames && settled) {
			s.ready_frame = s.frame;
			infostream << "fx test: scene ready at frame " << s.frame
					<< (s.idle >= s.settle_frames ? " (meshing idle)" : " (max wait)") << std::endl;
		}
		return;
	}

	const u32 since = s.frame - (u32)s.ready_frame;
	for (const auto &e : s.sets) {
		if (e.frame == since) {
			actionstream << "fx test: frame " << since << ": " << e.key << " = " << e.value << std::endl;
			g_settings->set(e.key, e.value);
		}
	}
	if (since < s.after_frames)
		return;

	s.done = true;
	if (s.joins_left > 0) {
		s.joins_left--;
		s.rejoin_pending = true;
		actionstream << "fx test: leaving to join again" << std::endl;
		g_gamecallback->disconnect();
		return;
	}
	if (video::IImage *img = driver->createScreenShot()) {
		if (driver->writeImageToFile(img, s.capture.c_str()))
			actionstream << "fx test: capture written to " << s.capture << std::endl;
		else
			errorstream << "fx test: could not write " << s.capture << std::endl;
		img->drop();
	}
	writeStats();
	g_gamecallback->exitToOS();
}

bool wantsRejoin()
{
	return active() && s.rejoin_pending;
}

void onGameStart()
{
	if (!active())
		return;
	// the new server sets it again once its scene is built (Game() runs
	// before the server starts, so this cannot race the scene mod)
	if (s.rejoin_pending)
		g_settings->setBool("fx_test_ready", false);
	s.rejoin_pending = false;
	s.frame = 0;
	s.done = false;
}

} // namespace fx::test

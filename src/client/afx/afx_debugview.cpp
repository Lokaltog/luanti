// Luanti "afx" fork: the tuning view (afx_debug_view)
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// What the acoustics see, drawn over the world:
//   rays     the probe's rays from the ear, coloured by what they hit
//            (grey stone, brown wood, green leaves, blue water, cyan glass,
//            tan earth, white snow); escaped ones fade out, yellow where
//            they passed through open sky
//   the way out   a cyan arrow toward where the outdoors comes in, as long
//            as how much of it does
//   walls    magenta to the wall beside you and the one across
//   sources  every playing sound: a cross where it is, a line from you
//            (green clear, orange muffled, red cut off; dashed to the
//            opening when it's heard around a corner) and a label with
//            its bus and mix
// and a summary of the room in the corner.

#include "afx.h"
#include "afx_mix.h"
#include "afx_types.h"
#include "client/camera.h"
#include "client/client.h"
#include "client/fontengine.h"
#include "client/sound.h"
#include "constants.h"
#include "util/string.h"
#include <IGUIFont.h>
#include <IVideoDriver.h>
#include <ICameraSceneNode.h>
#include <cstdio>

namespace afx
{

static video::SColor materialColor(Material m, u32 alpha)
{
	switch (m) {
	case Material::Stone: return video::SColor(alpha, 150, 150, 160);
	case Material::Wood: return video::SColor(alpha, 170, 110, 60);
	case Material::Foliage: return video::SColor(alpha, 60, 180, 60);
	case Material::Water: return video::SColor(alpha, 60, 110, 230);
	case Material::Glass: return video::SColor(alpha, 120, 230, 240);
	case Material::Earth: return video::SColor(alpha, 190, 160, 110);
	case Material::Snow: return video::SColor(alpha, 245, 245, 255);
	case Material::Soft: return video::SColor(alpha, 220, 120, 200);
	case Material::Lava: return video::SColor(alpha, 255, 90, 20);
	case Material::Leaky: return video::SColor(alpha, 200, 200, 90);
	default: return video::SColor(alpha, 255, 255, 255);
	}
}

static std::vector<DebugRay> s_rays;
static const VoxelSnapshot *s_rays_of = nullptr;
static v3f s_rays_ear, s_chest;

static Report latestReport(Client *client)
{
	Report r;
	ISoundManager *sound = client->getSoundManager();
	if (sound)
		sound->getAcousticReport(r);
	return r;
}

void drawDebugView3D(Client *client, video::IVideoDriver *driver)
{
	if (!params().debug_view)
		return;
	Camera *camera = client->getCamera();
	if (!camera)
		return;
	const v3f off = intToFloat(camera->getOffset(), BS);
	auto at = [&](v3f node_pos) { return node_pos * BS - off; };

	video::SMaterial mat;
	mat.MaterialType = video::EMT_TRANSPARENT_VERTEX_ALPHA;
	mat.ZBuffer = video::ECFN_DISABLED; // through walls: that's the point
	mat.ZWriteEnable = video::EZW_OFF;
	mat.Thickness = 1.f;
	driver->setMaterial(mat);
	driver->setTransform(video::ETS_WORLD, core::IdentityMatrix);

	// The rays, recast from the latest snapshot
	v3f ear;
	auto snap = debugSnapshot(&ear);
	if (snap && (snap.get() != s_rays_of || (ear - s_rays_ear).getLengthSQ() > 0.01f)) {
		s_rays = probeRays(*snap, ear, params().rays);
		s_rays_of = snap.get();
		s_rays_ear = ear;
	}
	// (lines from the eye itself would be seen end-on: they start from the
	// chest, which reads as the same fan of directions)
	Camera *cam = camera;
	const v3f chest = s_rays_ear + v3f(0, -0.7f, 0) + cam->getDirection() * 0.6f;
	s_chest = chest;
	for (const DebugRay &r : s_rays) {
		if (r.hit) {
			driver->draw3DLine(at(chest), at(r.end), materialColor(r.mat, 90));
			// a mark at the hit, big enough to see from here
			const v3f e = at(r.end);
			const float t = std::max(0.12f, 0.025f * (r.end - s_rays_ear).getLength()) * BS;
			driver->draw3DLine(e - v3f(t, 0, 0), e + v3f(t, 0, 0), materialColor(r.mat, 255));
			driver->draw3DLine(e - v3f(0, t, 0), e + v3f(0, t, 0), materialColor(r.mat, 255));
			driver->draw3DLine(e - v3f(0, 0, t), e + v3f(0, 0, t), materialColor(r.mat, 255));
		} else {
			const v3f mid = chest + (r.end - chest) * 0.25f;
			driver->draw3DLine(at(chest), at(mid),
					r.sky ? video::SColor(70, 255, 230, 80) : video::SColor(40, 255, 255, 255));
		}
	}

	Report rep = latestReport(client);
	if (!rep.valid)
		return;
	const Acoustics &a = rep.now;
	const v3f e = at(chest);

	// the way out
	if (a.outdoor_dir.getLengthSQ() > 0.5f) {
		const float len = 1.5f + 4.f * std::max(a.outdoor_gain, a.outdoor_wall);
		const v3f tip = e + a.outdoor_dir * (len * BS);
		const video::SColor c(220, 60, 230, 255);
		driver->draw3DLine(e, tip, c);
		v3f side = a.outdoor_dir.crossProduct(v3f(0, 1, 0));
		if (side.getLengthSQ() < 0.01f)
			side = v3f(1, 0, 0);
		side.normalize();
		driver->draw3DLine(tip, tip - a.outdoor_dir * (0.4f * BS) + side * (0.25f * BS), c);
		driver->draw3DLine(tip, tip - a.outdoor_dir * (0.4f * BS) - side * (0.25f * BS), c);
	}
	// where it gets out: a box on the doorway, sized by how wide it is
	if (a.has_opening > 0.5f) {
		const v3f o = at(a.opening_inner);
		const float h = (0.25f + 0.04f * a.opening_size) * BS;
		const video::SColor c(230, 60, 230, 255);
		driver->draw3DBox(core::aabbox3d<f32>(o - v3f(h, h, h), o + v3f(h, h, h)), c);
		driver->draw3DLine(o, at(a.opening), c);
	}
	// the space next door
	if (a.coupling > 0.05f && a.neighbor_dir.getLengthSQ() > 0.5f)
		driver->draw3DLine(e, e + a.neighbor_dir * ((1.f + 3.f * a.coupling) * BS),
				video::SColor(200, 255, 220, 40));
	// walls
	if (a.wall_near > 0.05f)
		driver->draw3DLine(e, e + a.wall_near_dir * (a.wall_near_dist * BS),
				video::SColor((u32)(80 + 175 * std::min(1.f, a.wall_near)), 255, 60, 220));
	if (a.wall_far > 0.05f)
		driver->draw3DLine(e, e + a.wall_far_dir * (a.wall_far_dist * BS),
				video::SColor((u32)(60 + 140 * std::min(1.f, a.wall_far)), 200, 60, 255));

	// the sounds
	for (const DebugSource &s : rep.debug_sources) {
		if (!s.positional)
			continue;
		const v3f p = at(s.pos);
		const float o = std::clamp(s.occlusion, 0.f, 1.f);
		const video::SColor c = o < 0.1f ? video::SColor(230, 80, 240, 80) :
				o < 0.7f ? video::SColor(230, 255, 160, 40) : video::SColor(230, 255, 50, 50);
		const float k = 0.25f * BS;
		driver->draw3DLine(p - v3f(k, 0, 0), p + v3f(k, 0, 0), c);
		driver->draw3DLine(p - v3f(0, k, 0), p + v3f(0, k, 0), c);
		driver->draw3DLine(p - v3f(0, 0, k), p + v3f(0, 0, k), c);
		driver->draw3DLine(e, p, video::SColor(90, c.getRed(), c.getGreen(), c.getBlue()));
		if (s.path > 0.5f) {
			// heard from the opening: dashed to it
			const v3f h = at(s.heard_from);
			for (int i = 0; i < 10; i += 2)
				driver->draw3DLine(e + (h - e) * (i / 10.f), e + (h - e) * ((i + 1) / 10.f),
						video::SColor(230, 255, 255, 255));
		}
	}
}

static std::string s_script_text; // the ambience's own lines (afx_debug_text)

void setScriptDebugText(const std::string &text)
{
	s_script_text = text.substr(0, 2048);
}

void drawDebugView2D(Client *client, video::IVideoDriver *driver)
{
	if (!params().debug_view)
		return;
	Camera *camera = client->getCamera();
	if (!camera)
		return;
	gui::IGUIFont *font = g_fontengine->getFont();
	if (!font)
		return;
	Report rep = latestReport(client);
	if (!rep.valid)
		return;
	const v3f off = intToFloat(camera->getOffset(), BS);
	scene::ICameraSceneNode *cam = camera->getCameraNode();
	core::matrix4 trans = cam->getProjectionMatrix();
	trans *= cam->getViewMatrix();
	const v2u32 screen = driver->getScreenSize();
	const v3f ear = s_rays_ear;

	auto label = [&](const std::string &text, v2s32 pos, video::SColor color) {
		const std::wstring w = utf8_to_wide(text);
		core::dimension2du d = font->getDimension(w.c_str());
		core::rect<s32> r(pos.X, pos.Y, pos.X + d.Width, pos.Y + d.Height);
		driver->draw2DRectangle(video::SColor(150, 0, 0, 0),
				core::rect<s32>(r.UpperLeftCorner.X - 2, r.UpperLeftCorner.Y,
						r.LowerRightCorner.X + 2, r.LowerRightCorner.Y));
		font->draw(w.c_str(), r, color);
		return (s32)d.Height;
	};

	// each positional sound, where it is; the others listed in the corner
	char buf[256];
	std::vector<std::string> flat;
	for (const DebugSource &s : rep.debug_sources) {
		if (s.gain < 0.005f)
			continue;
		std::string name = s.name;
		if (name.rfind("afx_", 0) == 0)
			name = name.substr(4);
		snprintf(buf, sizeof(buf), "%s %s g%.2f d%.2f hf%.2f m%.2f r%.2f%s%s",
				name.c_str(), busName((Bus)s.bus), s.gain, s.direct, s.direct_hf, s.muffle,
				s.reverb, s.remote > 0.01f ? (" far" + std::to_string(s.remote).substr(0, 4)).c_str() : "",
				s.path > 0.5f ? " around" : (s.occlusion > 0.1f ?
						(" occ" + std::to_string(s.occlusion).substr(0, 4)).c_str() : ""));
		if (!s.positional || (s.pos - ear).getLength() > 48.f) {
			flat.emplace_back(buf);
			continue;
		}
		const v3f p = s.pos * BS - off + v3f(0, 0.4f * BS, 0);
		f32 tp[4] = {p.X, p.Y, p.Z, 1.f};
		trans.multiplyWith1x4Matrix(tp);
		if (tp[3] <= 0)
			continue;
		const f32 z = 1.f / tp[3];
		v2s32 sp(screen.X * (0.5f + tp[0] * z * 0.5f), screen.Y * (0.5f - tp[1] * z * 0.5f));
		const float o = s.occlusion;
		label(buf, sp, o < 0.1f ? video::SColor(255, 160, 255, 160) :
				o < 0.7f ? video::SColor(255, 255, 200, 120) : video::SColor(255, 255, 130, 130));
	}

	// the room
	const Acoustics &a = rep.now;
	std::vector<std::string> lines;
	snprintf(buf, sizeof(buf), "afx view | encl %.2f  mfp %.1f  natural %.2f  hf %.2f",
			a.enclosure, a.mean_free_path, a.natural, a.hf);
	lines.emplace_back(buf);
	snprintf(buf, sizeof(buf), "reverb wet %.2f  decay %.2fs  refl %.2f  late %.2f",
			a.wet, a.decay, a.reflections_gain, a.late_gain);
	lines.emplace_back(buf);
	snprintf(buf, sizeof(buf), "recorded %s  share %.2f",
			rep.recorded >= 0 && rep.recorded < RECORDED_SPACE_COUNT ?
					RECORDED_SPACES[rep.recorded].sound : "none", rep.recorded_share);
	lines.emplace_back(buf);
	snprintf(buf, sizeof(buf), "outdoors open %.2f (hf %.2f, path %.0f, focus %.2f)  wall %.3f",
			a.outdoor_gain, a.outdoor_hf, a.outdoor_path, a.outdoor_focus, a.outdoor_wall);
	lines.emplace_back(buf);
	if (a.has_opening > 0.5f) {
		snprintf(buf, sizeof(buf), "way out at %.0f,%.0f,%.0f, %.0f open cells across",
				a.opening_inner.X, a.opening_inner.Y, a.opening_inner.Z, a.opening_size);
		lines.emplace_back(buf);
	}
	snprintf(buf, sizeof(buf), "walls near %.2f @%.1f  far %.2f @%.1f  next door %.2f (%.1fs)",
			a.wall_near, a.wall_near_dist, a.wall_far, a.wall_far_dist, a.coupling, a.neighbor_decay);
	lines.emplace_back(buf);
	snprintf(buf, sizeof(buf), "far space %.2f (%.1fs)  hollow %.2f  submerged %.2f  sources %u, %u occluded, %u around",
			rep.remote_level, rep.remote_decay, a.hollow, a.submerged, rep.sources, rep.occluded,
			rep.routed);
	lines.emplace_back(buf);
	for (const std::string &l : str_split(s_script_text, '\n'))
		if (!l.empty())
			lines.push_back(l);
	for (const std::string &f : flat)
		lines.push_back("  " + f);
	u32 width = 0;
	for (const std::string &l : lines)
		width = std::max(width, font->getDimension(utf8_to_wide(l).c_str()).Width);
	const s32 x = std::max<s32>(8, (s32)screen.X - (s32)width - 10);
	s32 y = std::max<s32>(8, screen.Y / 4); // under the chat
	for (const std::string &l : lines)
		y += label(l, v2s32(x, y), video::SColor(255, 230, 240, 255)) + 1;
}

} // namespace afx

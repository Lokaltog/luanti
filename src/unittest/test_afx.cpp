// Luanti "afx" fork: environmental audio
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// The acoustic probe on synthetic worlds: an open field, a stone cave room,
// a wooden hut, a tunnel with one mouth, a cliff that echoes, occluders.

#include "test.h"
#include "client/afx/afx_types.h"
#include "client/afx/afx_smoother.h"
#include "client/afx/afx_mood.h"
#include "client/afx/afx_mix.h"
#include "afx_worlds.h"

#include <cmath>

using namespace afx;
using afx_test::World;
using afx_test::openField;
using afx_test::stoneCave;

class TestAfx : public TestBase
{
public:
	TestAfx() { TestManager::registerTestModule(this); }
	const char *getName() { return "TestAfx"; }

	void runTests(IGameDef *gamedef);

	void testOpenField();
	void testStoneCave();
	void testWoodHut();
	void testTunnelMouth();
	void testCliffEcho();
	void testOcclusion();
	void testDoor();
	void testUnderwater();
	void testSmoother();
	void testMood();
	void testVariation();
};

static TestAfx g_test_instance;

void TestAfx::runTests(IGameDef *gamedef)
{
	TEST(testOpenField);
	TEST(testStoneCave);
	TEST(testWoodHut);
	TEST(testTunnelMouth);
	TEST(testCliffEcho);
	TEST(testOcclusion);
	TEST(testDoor);
	TEST(testUnderwater);
	TEST(testSmoother);
	TEST(testMood);
	TEST(testVariation);
}

void TestAfx::testOpenField()
{
	World w = openField();
	Acoustics a = w.probe(v3f(0.3f, 1.6f, 0.2f));
	UASSERT(a.enclosure < 0.15f);
	UASSERT(a.wet < 0.25f); // faint, long outdoor tail
	UASSERT(a.outdoor_gain == 1.f);
	UASSERT(a.outdoor_hf == 1.f);
	UASSERT(a.echo < 0.05f);
	UASSERT(!a.underwater);
}

void TestAfx::testStoneCave()
{
	World w = stoneCave();
	Acoustics a = w.probe(v3f(0, 0, 0));
	UASSERT(a.enclosure > 0.95f);
	UASSERT(a.decay > 1.5f);
	UASSERT(a.wet > 0.6f);
	UASSERT(a.outdoor_gain < 0.02f);
	UASSERT(a.decay_hf_ratio > 1.0f); // stone keeps its highs
	UASSERT(a.echo < 0.05f); // enclosed: the walls are reverb, not echo
}

void TestAfx::testDoor()
{
	auto hutWith = [](int door) { // 0 gap, 1 closed door, 2 door swung open
		World w = openField();
		w.fill({-4, 0, -4}, {4, 5, 4}, Material::Wood);
		w.fill({-3, 0, -3}, {3, 4, 3}, Material::Air);
		if (door == 0)
			w.fill({0, 0, -4}, {0, 1, -4}, Material::Air);
		else
			w.fillThin({0, 0, -4}, {0, 1, -4}, Material::Wood, door == 1 ? 2 : 0);
		w.markSky();
		return w.probe(v3f(0, 1.6f, 0));
	};
	Acoustics gap = hutWith(0), closed = hutWith(1), swung = hutWith(2);
	UASSERT(gap.outdoor_gain > 0.2f);
	UASSERT(gap.outdoor_dir.Z < -0.7f);
	UASSERT(swung.outdoor_gain > 0.2f);
	UASSERT(closed.outdoor_gain < 0.01f);
	// a door is the thin spot of the wall
	UASSERT(closed.outdoor_wall > 0.04f);
}

void TestAfx::testWoodHut()
{
	// 7x4x7 interior, 1-node plank walls and roof, standing in an open field.
	World w = openField();
	w.fill({-4, 0, -4}, {4, 5, 4}, Material::Wood);
	w.fill({-3, 0, -3}, {3, 4, 3}, Material::Air);
	w.markSky();
	Acoustics hut = w.probe(v3f(0, 1.6f, 0));
	Acoustics cave = stoneCave().probe(v3f(0, 0, 0));

	UASSERT(hut.enclosure > 0.9f);
	UASSERT(hut.decay < cave.decay * 0.5f);
	UASSERT(hut.wet < cave.wet * 0.6f);
	// No opening: outdoors only comes through the planks, on the muffled path
	UASSERT(hut.outdoor_gain < 0.01f);
	UASSERT(hut.outdoor_wall > 0.02f && hut.outdoor_wall < 0.3f);
	UASSERT(hut.decay_hf_ratio < cave.decay_hf_ratio);
}

void TestAfx::testTunnelMouth()
{
	// A hill of stone with a 3x3 tunnel running +X out to open air at x=12.
	World w = openField();
	w.fill({-32, 0, -32}, {11, 20, 31}, Material::Stone);
	w.fill({-20, 0, -1}, {11, 2, 1}, Material::Air);
	w.markSky();

	Acoustics deep = w.probe(v3f(-12, 1, 0));
	Acoustics near = w.probe(v3f(6, 1, 0));

	UASSERT(deep.outdoor_dir.X > 0.8f);
	UASSERT(near.outdoor_dir.X > 0.8f);
	UASSERT(deep.outdoor_gain < near.outdoor_gain);
	UASSERT(deep.outdoor_hf < near.outdoor_hf);
	UASSERT(deep.outdoor_gain > 0.01f && deep.outdoor_gain < 0.15f);
	UASSERT(deep.outdoor_wall < 0.01f); // a hill of stone transmits nothing
	UASSERT(deep.outdoor_focus > 0.5f);
	UASSERT(std::fabs(deep.outdoor_path - 24.f) < 4.f);
}

void TestAfx::testCliffEcho()
{
	// A cliff 20 nodes north (+Z), 30 tall.
	World w = openField();
	w.fill({-32, 0, 20}, {31, 30, 31}, Material::Stone);
	w.markSky();
	Acoustics a = w.probe(v3f(0, 1.6f, 0));

	UASSERT(a.echo > 0.3f);
	UASSERT(a.echo_dir.Z > 0.8f);
	UASSERT(std::fabs(a.echo_delay - 2.f * 20.f / 343.f) < 0.03f);
	UASSERT(a.outdoor_gain == 1.f);
	// carried on the early reflections, from the cliff's direction
	UASSERT(std::fabs(a.reflections_delay - 2.f * 20.f / 343.f) < 0.03f);
	UASSERT(a.reflections_pan.Z > 0.6f);

	// No cliff, no echo
	UASSERT(openField().probe(v3f(0, 1.6f, 0)).echo < 0.05f);
}

void TestAfx::testOcclusion()
{
	World w = openField();
	v3f ear(0, 1.6f, 0), src(0, 1, 10);

	UASSERT(occlusion(w.snap, ear, src).amount < 0.01f);

	// A 3-wide stone wall: blocked, but rays around its edges carry a little
	w.fill({-1, 0, 5}, {1, 4, 5}, Material::Stone);
	Occlusion pillar = occlusion(w.snap, ear, src);

	World wall = openField();
	wall.fill({-30, 0, 5}, {30, 20, 6}, Material::Stone);
	Occlusion full = occlusion(wall.snap, ear, src);
	UASSERT(full.amount > 0.99f);
	UASSERT(full.hf < 0.01f);
	UASSERT(full.wall < 0.001f);
	UASSERT(pillar.amount > 0.3f);

	World hedge = openField();
	hedge.fill({-30, 0, 5}, {30, 4, 5}, Material::Foliage);
	Occlusion leaves = occlusion(hedge.snap, ear, src);
	UASSERT(leaves.amount > 0.1f && leaves.amount < 0.5f);
	UASSERT(leaves.hf > 0.5f);
}

void TestAfx::testUnderwater()
{
	World w = openField();
	w.fill({-32, -10, -32}, {31, 3, 31}, Material::Water);
	w.markSky();
	Acoustics a = w.probe(v3f(0, 0, 0));
	UASSERT(a.underwater);
	UASSERT(a.outdoor_hf < 0.1f);
}

void TestAfx::testSmoother()
{
	Smoother s;
	Acoustics open, cave;
	cave.enclosure = 1.f;
	cave.wet = 1.f;
	cave.outdoor_gain = 0.f;
	cave.outdoor_dir = v3f(1, 0, 0);
	s.reset(open);
	s.setTarget(cave);

	// No jumps: after one 16 ms step we're barely moving
	s.step(0.016f, 1.f);
	UASSERT(s.current().wet < 0.1f);

	// Dread creeps in slower than relief arrives (asymmetric)
	for (int i = 0; i < 60; i++)
		s.step(0.016f, 1.f);
	float in_after_1s = s.current().wet;
	UASSERT(in_after_1s > 0.4f && in_after_1s < 1.f);

	for (int i = 0; i < 600; i++)
		s.step(0.016f, 1.f);
	UASSERT(std::fabs(s.current().wet - 1.f) < 0.01f);
	UASSERT(s.current().outdoor_gain < 0.01f);

	// Direction rotates rather than snapping through zero
	s.setTarget(open);
	Acoustics turn = cave;
	turn.outdoor_dir = v3f(0, 0, 1);
	s.setTarget(turn);
	s.step(0.1f, 1.f);
	float len = s.current().outdoor_dir.getLength();
	UASSERT(len > 0.9f && len < 1.1f);
	UASSERT(s.current().outdoor_dir.X > 0.f && s.current().outdoor_dir.Z > 0.f);
}

void TestAfx::testMood()
{
	// Noon in a field: neither
	MoodInputs field;
	field.env = openField().probe(v3f(0, 1.6f, 0));
	Mood f = moodTarget(field);
	UASSERT(f.tension < 0.1f && f.cozy < 0.1f);

	// Night in the same field: a little uneasy, not scary
	MoodInputs night = field;
	night.daylight = 0.1f;
	night.light = 0.2f;
	Mood n = moodTarget(night);
	UASSERT(n.tension > 0.2f && n.tension < 0.7f);

	// Dark stone cave far from the sky: dread
	MoodInputs cave;
	cave.env = stoneCave().probe(v3f(0, 0, 0));
	cave.light = 0.05f;
	Mood c = moodTarget(cave);
	UASSERT(c.tension > 0.75f);

	// Same cave with a bed, a chest, a furnace and lamps lit: home
	MoodInputs home = cave;
	home.light = 0.9f;
	home.homey = 6;
	Mood h = moodTarget(home);
	UASSERT(h.tension < 0.2f);
	UASSERT(h.cozy > 0.8f);

	// Dread creeps in, relief arrives fast
	MoodSmoother sm;
	for (int i = 0; i < 60 * 3; i++)
		sm.step(1.f / 60, c);
	UASSERT(sm.current().tension < 0.2f); // 3 s in: barely
	for (int i = 0; i < 60 * 40; i++)
		sm.step(1.f / 60, c);
	UASSERT(sm.current().tension > 0.7f);
	for (int i = 0; i < 60 * 3; i++)
		sm.step(1.f / 60, f);
	UASSERT(sm.current().tension < 0.35f); // 3 s back out: mostly gone
}

void TestAfx::testVariation()
{
	// Off is exactly neutral
	Variation off = vary(0.9f, 0.9f, 0.1f, 0.1f, 0.f);
	UASSERT(off.pitch == 1.f && off.gain == 1.f);

	// Within ±7 % pitch and ±3 dB at any amount; mostly small changes
	float lo = 1, hi = 1, sum_abs = 0;
	int n = 0;
	for (int a = 0; a <= 10; a++)
	for (int b = 0; b <= 10; b++) {
		Variation v = vary(a / 10.f, b / 10.f, b / 10.f, a / 10.f, 2.f);
		UASSERT(v.pitch >= std::exp(-0.0701f) && v.pitch <= std::exp(0.0701f));
		UASSERT(v.gain >= 0.70f && v.gain <= 1.42f);
		Variation w = vary(a / 10.f, b / 10.f, 0.5f, 0.5f, 1.f);
		lo = std::min(lo, w.pitch);
		hi = std::max(hi, w.pitch);
		sum_abs += std::fabs(std::log(w.pitch));
		n++;
	}
	UASSERT(hi > 1.05f && lo < 0.95f);  // audible spread at the extremes
	UASSERT(sum_abs / n < 0.03f);       // but mostly subtle
}

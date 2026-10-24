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
#include "client/afx/afx_score.h"
#include <random>
#include <cstring>
#include "afx_worlds.h"
#include "client/afx/afx_knock.h"

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
	void testAroundCorner();
	void testNeighborRoom();
	void testHollowFloor();
	void testScore();
	void testDeafen();
	void testRoomTone();
	void testSourceSpace();
	void testBuriedSilent();
	void testKnock();
	void testWeather();
	void testSkybridge();
	void testShortTunnel();
	void testWalls();
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
	TEST(testAroundCorner);
	TEST(testNeighborRoom);
	TEST(testHollowFloor);
	TEST(testScore);
	TEST(testDeafen);
	TEST(testRoomTone);
	TEST(testSourceSpace);
	TEST(testBuriedSilent);
	TEST(testKnock);
	TEST(testWeather);
	TEST(testSkybridge);
	TEST(testShortTunnel);
	TEST(testWalls);
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
	// a doorway is a small window on the outdoors: clearly there, well down
	UASSERT(gap.outdoor_gain > 0.05f && gap.outdoor_gain < 0.3f);
	UASSERT(gap.outdoor_dir.Z < -0.7f);
	UASSERT(swung.outdoor_gain > 0.05f);
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

// An L-shaped corridor in stone: the ear at one end, a source around the
// corner. The sound should come from the corner, not through the rock.
void TestAfx::testAroundCorner()
{
	World w;
	w.fill({-32, -32, -32}, {31, 31, 31}, Material::Stone);
	w.fill({-10, 0, -1}, {3, 2, 1}, Material::Air);   // east-west leg
	w.fill({1, 0, 1}, {3, 2, 14}, Material::Air);     // north leg from x=1..3
	w.markSky();
	const v3f ear(-8, 1, 0), src(2, 1, 12);

	Occlusion occ = occlusion(w.snap, ear, src);
	UASSERT(occ.amount > 0.9f); // through the rock: blocked
	AirField f;
	buildAirField(w.snap, ear, 20, f);
	routeAround(w.snap, f, ear, src, occ);
	UASSERT(occ.path > 0.5f);
	// heard from the corner's direction (east), at about the walked distance
	v3f d = occ.virt_pos - ear;
	UASSERT(d.normalize().X > 0.9f);
	const float walked = (occ.virt_pos - ear).getLength();
	UASSERT(walked > 19.f && walked < 26.f);
	// a right-angle bend costs some level and most of the highs
	UASSERT(occ.path_gain < 0.7f && occ.path_gain > 0.2f);
	UASSERT(occ.path_hf < 0.4f);

	// In plain sight: no routing
	Occlusion clear = occlusion(w.snap, ear, v3f(0, 1, 0));
	routeAround(w.snap, f, ear, v3f(0, 1, 0), clear);
	UASSERT(clear.path == 0.f);
}

// A small wooden hut whose door opens into a big stone cavern: the cavern's
// reverb comes in through the door.
void TestAfx::testNeighborRoom()
{
	World w;
	w.fill({-32, -32, -32}, {31, 31, 31}, Material::Stone);
	w.fill({-12, -6, 2}, {12, 8, 26}, Material::Air);    // cavern to the north
	w.fill({-3, -1, -6}, {3, 3, 1}, Material::Wood);     // the hut, inside the rock
	w.fill({-2, 0, -5}, {2, 2, 0}, Material::Air);
	w.fill({0, 0, 1}, {0, 1, 1}, Material::Air);         // its door, north
	w.markSky();
	Acoustics a = w.probe(v3f(0, 1, -3));
	UASSERT(a.coupling > 0.15f);
	UASSERT(a.neighbor_decay > a.decay * 1.5f);
	UASSERT(a.neighbor_dir.Z > 0.6f);

	// The same hut with the door closed: nothing next door
	w.fill({0, 0, 1}, {0, 1, 1}, Material::Wood);
	Acoustics shut = w.probe(v3f(0, 1, -3));
	UASSERT(shut.coupling < 0.02f);
}

void TestAfx::testHollowFloor()
{
	// planks over a void
	World w = openField();
	w.fill({-6, -6, -6}, {6, -1, 6}, Material::Air);
	w.fill({-6, -1, -6}, {6, -1, 6}, Material::Wood);
	w.markSky();
	UASSERT(w.probe(v3f(0, 1.6f, 0)).hollow > 0.6f);
	// solid ground
	UASSERT(openField().probe(v3f(0, 1.6f, 0)).hollow < 0.05f);
}

void TestAfx::testScore()
{
	std::mt19937 rng(3);
	auto r = [&] { return std::uniform_real_distribution<float>(0.f, 1.f)(rng); };
	auto run = [&](Mood m, float tod, float day, float seconds, Score &sc) {
		std::vector<Note> out, all;
		for (float t = 0; t < seconds; t += 0.1f) {
			out.clear();
			sc.step(0.1f, m, tod, day, r, out);
			all.insert(all.end(), out.begin(), out.end());
		}
		return all;
	};

	// Silent at first, then a session plays
	Score sc;
	Mood calm;
	auto first = run(calm, 0.5f, 1.f, 15.f, sc);
	UASSERT(first.empty());
	auto day = run(calm, 0.5f, 1.f, 120.f, sc);
	UASSERT(day.size() > 15);
	int piano = 0;
	for (const auto &n : day) {
		UASSERT(n.pitch > 0.24f && n.pitch < 4.1f);
		UASSERT(n.gain > 0.f && n.gain < 0.8f);
		piano += std::strcmp(n.group, "afx_dry_score_piano") == 0;
	}
	UASSERT(piano > (int)day.size() / 2);

	// Dread: no melody, only sparse low bowl strikes
	Score dread_sc;
	dread_sc.startSession();
	Mood dread;
	dread.tension = 0.9f;
	auto d = run(dread, 0.0f, 0.f, 120.f, dread_sc);
	UASSERT(!d.empty() && d.size() < 20);
	for (const auto &n : d) {
		UASSERT(std::strcmp(n.group, "afx_dry_score_bowl") == 0);
		UASSERT(n.pitch < 0.75f);
	}

	// Dawn: one rising phrase, once
	Score dawn_sc;
	auto dw = run(calm, 0.25f, 0.5f, 8.f, dawn_sc);
	UASSERT(dw.size() >= 7);
	auto again = run(calm, 0.26f, 0.5f, 5.f, dawn_sc);
	UASSERT(again.empty());
}

void TestAfx::testDeafen()
{
	Params p;
	Acoustics env;
	Mix calm = mixFor(Bus::World, env, {}, p);
	env.deafen = 1.f;
	Mix blast = mixFor(Bus::World, env, {}, p);
	UASSERT(blast.direct_hf < calm.direct_hf * 0.1f);
	UASSERT(blast.direct_gain < calm.direct_gain * 0.6f);
	// the ear ring (dry) is untouched
	Mix ring = mixFor(Bus::Dry, env, {}, p);
	UASSERT(ring.direct_hf == 1.f && ring.direct_gain == 1.f);
	// ducking only dips the ambience
	Acoustics loud;
	loud.duck = 1.f;
	UASSERT(mixFor(Bus::Outdoor, loud, {}, p).direct_gain < 0.5f);
	UASSERT(mixFor(Bus::World, loud, {}, p).direct_gain == 1.f);
}

void TestAfx::testRoomTone()
{
	Params p;
	// silent under the sky
	const RoomTone open = roomTone(openField().probe(v3f(0, 1.6f, 0)), p);
	UASSERT(open.gain < 0.002f);

	// a stone cavern rumbles: louder and lower than a wooden hut
	const RoomTone cave = roomTone(stoneCave().probe(v3f(0, 0, 0)), p);
	World hut = openField();
	hut.fill({-4, 0, -4}, {4, 5, 4}, Material::Wood);
	hut.fill({-3, 0, -3}, {3, 4, 3}, Material::Air);
	hut.markSky();
	const RoomTone wood = roomTone(hut.probe(v3f(0, 1.6f, 0)), p);
	UASSERT(wood.gain > 0.01f);
	UASSERT(cave.gain > wood.gain);
	UASSERT(cave.pitch < wood.pitch);
	// mostly the room ringing, not a noise in your ear
	UASSERT(wood.mix.reverb_send > wood.mix.direct_gain);
	UASSERT(wood.mix.direct_hf < 0.5f);

	// a wool room is near silent; the setting scales it, 0 turns it off
	World wool = openField();
	wool.fill({-4, 0, -4}, {4, 5, 4}, Material::Soft);
	wool.fill({-3, 0, -3}, {3, 4, 3}, Material::Air);
	wool.markSky();
	UASSERT(roomTone(wool.probe(v3f(0, 1.6f, 0)), p).gain < wood.gain * 0.6f);
	p.roomtone = 0.f;
	UASSERT(roomTone(hut.probe(v3f(0, 1.6f, 0)), p).gain == 0.f);
}

void TestAfx::testSourceSpace()
{
	// The listener in a dead little wool closet, a cavern beyond its door,
	// a zombie in the cavern
	World w;
	w.fill({-32, -32, -32}, {31, 31, 31}, Material::Stone);
	w.fill({-12, -6, 4}, {12, 8, 28}, Material::Air);
	w.fill({-3, -1, -4}, {3, 3, 3}, Material::Soft);
	w.fill({-2, 0, -3}, {2, 2, 2}, Material::Air);
	w.fill({0, 0, 3}, {0, 1, 3}, Material::Air);
	const Params p;
	const v3f ear(0, 1.6f, -1);
	const Acoustics here = w.probe(ear);
	const SourceSpace far = analyzeSpace(w.snap, v3f(2, 0.5f, 18), p);
	UASSERT(far.valid);
	UASSERT(far.decay > here.decay * 2.f);
	UASSERT(spaceContrast(far, here) > 0.5f);

	// It ends up in the remote slot and feeds it
	RemoteSpace r;
	float bw = 0.f;
	const SourceSpace near = analyzeSpace(w.snap, v3f(1, 0.5f, 0), p);
	considerRemote(near, 1.f, v3f(1, 0, 0), here, r, bw);
	UASSERT(r.level == 0.f); // the closet itself has nothing to carry
	considerRemote(far, 0.5f, v3f(0, 0, 1), here, r, bw);
	UASSERT(r.level > 0.3f);
	UASSERT(r.decay == far.decay);
	Occlusion o;
	Mix m = mixFor(Bus::World, here, o, p);
	carrySpace(m, far, r, here, o, p);
	UASSERT(m.remote_send > 0.4f);
	Mix mn = mixFor(Bus::World, here, o, p);
	carrySpace(mn, near, r, here, o, p);
	UASSERT(mn.remote_send < 0.05f);
	Params off = p;
	off.source_space = 0.f;
	Mix mo = mixFor(Bus::World, here, o, off);
	carrySpace(mo, far, r, here, o, off);
	UASSERT(mo.remote_send == 0.f);

	// Something under water sounds dull from up here
	World pond = openField();
	pond.fill({-6, -4, 4}, {6, -1, 14}, Material::Water);
	pond.markSky();
	const SourceSpace fish = analyzeSpace(pond.snap, v3f(0, -2.5f, 9), p);
	UASSERT(fish.in_water);
	const Acoustics bank = pond.probe(v3f(0, 1.6f, 0));
	Mix mf = mixFor(Bus::World, bank, o, p);
	carrySpace(mf, fish, RemoteSpace(), bank, o, p);
	UASSERT(mf.direct_hf < 0.3f);
}

void TestAfx::testBuriedSilent()
{
	// A bird on the surface, the listener in a sealed pocket 8 nodes under
	// it: nothing of it may reach the ear, not even through the room's reverb
	World w = openField();
	w.fill({-32, -32, -32}, {31, -1, 31}, Material::Stone);
	w.fill({-1, -10, -1}, {1, -8, 1}, Material::Air);
	w.markSky();
	const v3f ear(0, -8.4f, 0), bird(6, 3, 4);
	const Params p;
	const Acoustics env = w.probe(ear);
	const Occlusion o = occlusion(w.snap, ear, bird);
	const Mix m = mixFor(Bus::World, env, o, p);
	UASSERT(m.direct_gain < 0.01f);
	UASSERT(m.reverb_send < 0.01f);
	UASSERT(m.muffle_send < 0.01f);
	UASSERT(m.echo_send < 0.01f);
	// and the outdoor beds are gone too
	const Mix bed = mixFor(Bus::Outdoor, env, Occlusion(), p);
	UASSERT(bed.direct_gain < 0.01f && bed.muffle_send < 0.01f);

	// Behind one plank wall: only the muffled path, a little
	World hut = openField();
	hut.fill({-4, 0, -4}, {4, 5, 4}, Material::Wood);
	hut.fill({-3, 0, -3}, {3, 4, 3}, Material::Air);
	hut.markSky();
	const v3f in(0, 1.6f, 0), out(10, 1.5f, 0);
	const Mix mh = mixFor(Bus::World, hut.probe(in), occlusion(hut.snap, in, out), p);
	UASSERT(mh.direct_gain < 0.05f);
	UASSERT(mh.muffle_send > 0.005f && mh.muffle_send < 0.2f);
}

void TestAfx::testKnock()
{
	// The player in a tunnel along x, facing a stone wall to the north (+z).
	// Behind it, two nodes in, a cave.
	World w;
	w.fill({-32, -32, -32}, {31, 31, 31}, Material::Stone);
	w.fill({-6, 0, -1}, {6, 2, 1}, Material::Air);        // the tunnel
	auto open = [&](v3s16 p) { return cellMaterial(w.snap.at(p)) == Material::Air &&
			!(w.snap.at(p) & CELL_UNLOADED); };
	const v3s16 wall(0, 1, 2), into(0, 0, 1);
	Knock solid = knockProbe(open, wall, into);
	UASSERT(solid.depth == 0 && solid.hollow == 0.f);

	w.fill({-4, -2, 4}, {4, 4, 10}, Material::Air);       // a cave behind
	Knock cave = knockProbe(open, wall, into);
	UASSERT(cave.depth == 2);
	UASSERT(cave.hollow > 0.6f);
	UASSERT(cave.pitch < 1.f);

	// Further in, fainter; five nodes of rock and it's lost
	World deep = w;
	deep.fill({-4, -2, 3}, {4, 4, 5}, Material::Stone);
	Knock far = knockProbe([&](v3s16 p) { return cellMaterial(deep.snap.at(p)) == Material::Air &&
			!(deep.snap.at(p) & CELL_UNLOADED); }, wall, into);
	UASSERT(far.depth == 4);
	UASSERT(far.hollow < cave.hollow * 0.6f);

	// A pillar in the room: the air behind it is the room itself
	World room;
	room.fill({-32, -32, -32}, {31, 31, 31}, Material::Stone);
	room.fill({-5, 0, -5}, {5, 3, 5}, Material::Air);
	room.fill({0, 0, 0}, {0, 3, 0}, Material::Stone);
	Knock pillar = knockProbe([&](v3s16 p) { return cellMaterial(room.snap.at(p)) == Material::Air &&
			!(room.snap.at(p) & CELL_UNLOADED); }, v3s16(0, 1, 0), v3s16(0, 0, 1));
	UASSERT(pillar.depth == 1);
	UASSERT(pillar.hollow < 0.35f);
}

void TestAfx::testWeather()
{
	Params p;
	p.wind = 1.4f;
	p.wind_dir = v3f(1, 0, 0); // blowing east
	Mix up, down, near, calm;
	// a source 40 nodes east: its sound travels west, into the wind
	carryWeather(up, v3f(40, 0, 0), p);
	// a source 40 nodes west: the wind carries it to us
	carryWeather(down, v3f(-40, 0, 0), p);
	carryWeather(near, v3f(4, 0, 0), p);
	UASSERT(up.direct_gain < 0.6f && up.direct_hf < 0.6f);
	UASSERT(down.direct_gain == 1.f);
	UASSERT(down.air_absorption < up.air_absorption);
	UASSERT(near.direct_gain == 1.f); // close by, the wind doesn't matter
	// still nights carry, rain dulls
	Params night;
	night.night = 1.f;
	carryWeather(calm, v3f(30, 0, 0), night);
	UASSERT(calm.air_absorption < 3.f);
	Params rain;
	rain.rain = 1.f;
	Mix wet;
	carryWeather(wet, v3f(30, 0, 0), rain);
	UASSERT(wet.air_absorption > 3.f && wet.direct_gain < 1.f);
}

void TestAfx::testShortTunnel()
{
	// Two nodes into rock, a 1x2 mouth to open air: near, but a small window
	World w = openField();
	w.fill({-32, 0, -32}, {31, 20, 31}, Material::Stone);
	w.fill({0, 0, 0}, {0, 1, 3}, Material::Air);      // tunnel along +z, out at z=3
	w.fill({-32, 0, 4}, {31, 20, 31}, Material::Air);  // open land beyond
	w.markSky();
	const Acoustics a = w.probe(v3f(0, 1.5f, 1));
	UASSERT(a.outdoor_path < 4.f);
	UASSERT(a.outdoor_gain < 0.35f);
	UASSERT(a.outdoor_gain > 0.03f);
	UASSERT(a.outdoor_hf < 0.8f);
}

void TestAfx::testSkybridge()
{
	// A 3-wide plank bridge high in the air, nothing else around: no reverb
	World w;
	w.fill({-32, 0, -1}, {31, 0, 1}, Material::Wood);
	w.markSky();
	const Acoustics a = w.probe(v3f(0, 1.6f, 0));
	UASSERT(a.wet < 0.04f);
	UASSERT(a.decay < 0.6f);
	// while a meadow keeps its faint long tail
	const Acoustics m = openField().probe(v3f(0, 1.6f, 0));
	UASSERT(m.wet > 0.1f);
	UASSERT(m.decay > 1.2f);
}

void TestAfx::testWalls()
{
	// Open meadow: the ground isn't a wall
	const Acoustics open = openField().probe(v3f(0, 1.6f, 0));
	UASSERT(open.wall_near < 0.05f);
	// Beside a stone cliff 2 nodes east: a slap from the east
	World cliff = openField();
	cliff.fill({2, 0, -32}, {31, 20, 31}, Material::Stone);
	cliff.markSky();
	const Acoustics c = cliff.probe(v3f(0, 1.6f, 0));
	UASSERT(c.wall_near > 0.5f);
	UASSERT(c.wall_near_dir.X > 0.7f);
	UASSERT(std::fabs(c.wall_near_dist - 1.5f) < 1.f);
	UASSERT(c.wall_far < 0.05f);
	// A 3-wide stone corridor: both sides answer
	World cor = openField();
	cor.fill({-32, 0, -32}, {31, 4, 31}, Material::Stone);
	cor.fill({-32, 0, -1}, {31, 3, 1}, Material::Air);
	cor.markSky();
	const Acoustics k = cor.probe(v3f(0, 1.6f, 0));
	UASSERT(k.wall_near > 0.5f && k.wall_far > 0.5f);
	UASSERT(k.wall_near_dir.dotProduct(k.wall_far_dir) < -0.6f);
	// Wool walls barely answer
	World wool = openField();
	wool.fill({2, 0, -32}, {31, 20, 31}, Material::Soft);
	wool.markSky();
	UASSERT(wool.probe(v3f(0, 1.6f, 0)).wall_near < 0.4f);
}

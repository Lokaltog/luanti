// Luanti "fx" fork: unit tests for the renderer's pure logic.
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Run: bin/luanti --run-unittests --test-module TestFx

#include "test.h"

#include "client/fx/light_volume.h"
#include "client/fx/params.h"
#include "client/fx/renderer.h"
#include "filesys.h"
#include "nodedef.h"
#include "client/node_visuals.h"
#include "porting.h"
#include "util/string.h"

#include <fstream>
#include <map>
#include <cmath>
#include <sstream>
#include <vector>

class TestFx : public TestBase
{
public:
	TestFx() { TestManager::registerTestModule(this); }
	const char *getName() { return "TestFx"; }

	void runTests(IGameDef *gamedef);

	void testFloodFalloff();
	void testFloodOpaqueStops();
	void testFloodGlassTints();
	void testFloodColorsMax();
	void testFloodUnknownIsOpaque();
	void testParamRegistry();
	void testSettingtypesInSync();
	void testPackGlob();
	void testPackTables();
};

static TestFx g_test_instance;

void TestFx::runTests(IGameDef *gamedef)
{
	TEST(testFloodFalloff);
	TEST(testPackGlob);
	TEST(testPackTables);
	TEST(testFloodOpaqueStops);
	TEST(testFloodGlassTints);
	TEST(testFloodColorsMax);
	TEST(testFloodUnknownIsOpaque);
	TEST(testParamRegistry);
	TEST(testSettingtypesInSync);
}

namespace
{

// content ids for the tests
enum : u16 { AIR, WALL, WHITE, RED_GLASS, BLUE, COUNT };

std::vector<fx::LightCell> cells()
{
	std::vector<fx::LightCell> c(COUNT);
	c[AIR].opaque = false;
	c[WALL].opaque = true;
	c[WHITE].opaque = false;
	c[WHITE].src[0] = c[WHITE].src[1] = c[WHITE].src[2] = 14;
	c[RED_GLASS].opaque = false;
	c[RED_GLASS].tint[1] = c[RED_GLASS].tint[2] = 0;
	c[BLUE].opaque = false;
	c[BLUE].src[2] = 14;
	return c;
}

struct Grid {
	s16 sx, sy, sz;
	std::vector<u16> content;
	std::vector<u8> out;
	Grid(s16 x, s16 y, s16 z) : sx(x), sy(y), sz(z), content((size_t)x * y * z, AIR) {}
	u16 &at(s16 x, s16 y, s16 z) { return content[((size_t)z * sy + y) * sx + x]; }
	u8 lvl(s16 x, s16 y, s16 z, int c) const { return out[(((size_t)z * sy + y) * sx + x) * 3 + c]; }
	void fill() { fx::floodFillRGB(sx, sy, sz, content.data(), cells(), out); }
};

} // namespace

void TestFx::testFloodFalloff()
{
	Grid g(32, 1, 1);
	g.at(0, 0, 0) = WHITE;
	g.fill();
	UASSERTEQ(int, g.lvl(0, 0, 0, 0), 14);
	UASSERTEQ(int, g.lvl(1, 0, 0, 0), 13);
	UASSERTEQ(int, g.lvl(13, 0, 0, 1), 1);
	UASSERTEQ(int, g.lvl(14, 0, 0, 2), 0); // light reaches 13 nodes, like the engine
}

void TestFx::testFloodOpaqueStops()
{
	// the leak test: light on one side of a wall must not reach the other
	Grid g(9, 3, 3);
	for (s16 y = 0; y < 3; y++)
	for (s16 z = 0; z < 3; z++)
		g.at(4, y, z) = WALL;
	g.at(1, 1, 1) = WHITE;
	g.fill();
	UASSERTEQ(int, g.lvl(3, 1, 1, 0), 12);
	UASSERTEQ(int, g.lvl(4, 1, 1, 0), 0);
	UASSERTEQ(int, g.lvl(5, 1, 1, 0), 0);
	UASSERTEQ(int, g.lvl(8, 1, 1, 0), 0);
}

void TestFx::testFloodGlassTints()
{
	Grid g(6, 1, 1);
	g.at(0, 0, 0) = WHITE;
	g.at(2, 0, 0) = RED_GLASS;
	g.fill();
	UASSERTEQ(int, g.lvl(1, 0, 0, 1), 13); // white before the glass
	UASSERTEQ(int, g.lvl(2, 0, 0, 0), 12); // red passes
	UASSERTEQ(int, g.lvl(2, 0, 0, 1), 0);  // green and blue are filtered
	UASSERTEQ(int, g.lvl(3, 0, 0, 0), 11);
	UASSERTEQ(int, g.lvl(3, 0, 0, 2), 0);
}

void TestFx::testFloodColorsMax()
{
	// two sources: each channel keeps the brighter of the two
	Grid g(9, 1, 1);
	g.at(0, 0, 0) = WHITE;
	g.at(8, 0, 0) = BLUE;
	g.fill();
	UASSERTEQ(int, g.lvl(7, 0, 0, 2), 13); // blue from its own source
	UASSERTEQ(int, g.lvl(7, 0, 0, 0), 7);  // red only from the white one
	UASSERTEQ(int, g.lvl(4, 0, 0, 2), 10);
}

void TestFx::testFloodUnknownIsOpaque()
{
	Grid g(5, 1, 1);
	g.at(0, 0, 0) = WHITE;
	g.at(2, 0, 0) = 999; // not in the table
	g.fill();
	UASSERTEQ(int, g.lvl(1, 0, 0, 0), 13);
	UASSERTEQ(int, g.lvl(2, 0, 0, 0), 0);
	UASSERTEQ(int, g.lvl(3, 0, 0, 0), 0);
}

void TestFx::testParamRegistry()
{
	// every float gets a unique block slot and a GLSL accessor
	fx::Params p;
	std::vector<float> block;
	fx::packParamBlock(p, block);
	UASSERTEQ(size_t, block.size(), fx::paramBlockVec4s() * 4);
	const std::string glsl = fx::paramBlockGlsl();
	for (const auto &i : fx::paramInfo()) {
		if (i.type != fx::ParamType::Float)
			continue;
		UASSERT(i.slot >= 0);
		UASSERT(block[i.slot] == i.def);
		UASSERT(glsl.find("#define " + i.glsl + " ") != std::string::npos);
		UASSERT(i.min <= i.def && i.def <= i.max);
	}
	const auto *e = fx::findParam("fx_exposure");
	UASSERT(e && e->glsl == "fxExposure");
	fx::setFloat(p, *e, 2.5f);
	UASSERT(p.exposure == 2.5f);
}

void TestFx::testSettingtypesInSync()
{
	// builtin/settingtypes.txt documents every registry entry, with the
	// registry's type, default and range
	std::ifstream is(porting::path_share + DIR_DELIM "builtin" DIR_DELIM "settingtypes.txt");
	UASSERT(is.good());
	std::map<std::string, std::vector<std::string>> doc;
	std::string line;
	while (std::getline(is, line)) {
		if (!str_starts_with(line, "fx_"))
			continue;
		// name (Label) type default [min max]
		const auto open = line.find('('), close = line.find(')');
		if (open == std::string::npos || close == std::string::npos)
			continue;
		std::istringstream rest(line.substr(close + 1));
		std::vector<std::string> f;
		for (std::string t; rest >> t;)
			f.push_back(t);
		doc[std::string(trim(line.substr(0, open)))] = f;
	}
	for (const auto &i : fx::paramInfo()) {
		auto it = doc.find(i.setting);
		if (it == doc.end())
			throw TestFailedException("settingtypes.txt is missing " + i.setting, __FILE__, __LINE__);
		const auto &f = it->second;
		const char *type = i.type == fx::ParamType::Bool ? "bool" :
				i.type == fx::ParamType::Int ? "int" : "float";
		if (f.empty() || f[0] != type)
			throw TestFailedException(i.setting + ": type should be " + type, __FILE__, __LINE__);
		if (i.type == fx::ParamType::Bool) {
			if (f.size() < 2 || is_yes(f[1]) != (i.def != 0.f))
				throw TestFailedException(i.setting + ": default differs from params.def", __FILE__, __LINE__);
			continue;
		}
		if (f.size() < 4 || std::fabs(stof(f[1]) - i.def) > 1e-4f ||
				std::fabs(stof(f[2]) - i.min) > 1e-4f || std::fabs(stof(f[3]) - i.max) > 1e-4f)
			throw TestFailedException(i.setting + ": default or range differs from params.def", __FILE__, __LINE__);
	}
}

void TestFx::testPackGlob()
{
	using fx::packGlobMatch;
	UASSERT(packGlobMatch("mcl_core:glass_*", "mcl_core:glass_red"));
	UASSERT(packGlobMatch("mcl_core:glass_*", "mcl_core:glass_"));
	UASSERT(!packGlobMatch("mcl_core:glass_*", "mcl_core:glass"));
	UASSERT(!packGlobMatch("mcl_core:glass_*", "xmcl_core:glass_red"));
	UASSERT(packGlobMatch("*:torch*", "mcl_torches:torch_wall"));
	UASSERT(packGlobMatch("a?c", "abc"));
	UASSERT(!packGlobMatch("a?c", "ac"));
	UASSERT(packGlobMatch("*", ""));
	UASSERT(packGlobMatch("*a*b", "xxaxxab"));
	UASSERT(!packGlobMatch("*a*b", "xxaxxa"));
}

static ContentFeatures packNode(const std::string &name, const std::string &tile0)
{
	ContentFeatures f;
	f.name = name;
	f.tiledef[0].name = tile0;
	return f;
}

void TestFx::testPackTables()
{
	// two texture dirs: the first wins per key; format is per dir
	const std::string root = getTestTempDirectory();
	const std::string a = root + DIR_DELIM + "fxpack_a", b = root + DIR_DELIM + "fxpack_b";
	fs::CreateAllDirs(a);
	fs::CreateAllDirs(b);
	auto write = [] (const std::string &path, const char *text) {
		std::ofstream(path) << text;
	};
	write(a + DIR_DELIM + "fx_pack.txt", "format = 2\nproducer = test\n");
	write(a + DIR_DELIM + "fx_light_colors.txt",
		"mod:torch #ff8000\n"            // v2: no flicker= -> name heuristic
		"mod:lamp_* #00ff00 flicker=0\n"
		"mod:lamp_r* #ff0000 flicker=3\n" // longer glob wins
		"@glow.png #0000ff flicker=9\n"   // clamped to 4
		"bad line\n");
	write(a + DIR_DELIM + "fx_materials.txt", "mod:glass_* glass pattern=frosted\n@ice.png glass\n");
	write(b + DIR_DELIM + "fx_light_colors.txt", "mod:torch #ffffff\nmod:candle #ff0000\n");
	write(b + DIR_DELIM + "fx_materials.txt", "mod:glass_red none\n");

	fx::PackTables t;
	fx::loadPackTables(t, {a, b});
	bool heur = false;

	const fx::LightStyle *l = fx::packFindLight(t, packNode("mod:torch", "x.png"), &heur);
	UASSERT(l && l->color == video::SColor(255, 255, 128, 0) && heur); // dir a wins
	l = fx::packFindLight(t, packNode("mod:candle", "x.png"), &heur);
	UASSERT(l && !heur && l->flicker == 0.f); // dir b has no fx_pack.txt: v1
	l = fx::packFindLight(t, packNode("mod:lamp_red", "x.png"));
	UASSERT(l && l->color.getRed() == 255 && l->flicker == 3.f);
	l = fx::packFindLight(t, packNode("mod:lamp_blue", "x.png"));
	UASSERT(l && l->color.getGreen() == 255);
	// @texture: base atom of the first non-empty tile, modifiers stripped
	l = fx::packFindLight(t, packNode("other:thing", "glow.png^[colorize:#fff:80"));
	UASSERT(l && l->color.getBlue() == 255 && l->flicker == 4.f);
	l = fx::packFindLight(t, packNode("other:thing", "(glow.png^x.png)"));
	UASSERT(l);
	UASSERT(!fx::packFindLight(t, packNode("other:thing", "noglow.png")));
	// exact beats glob beats texture
	l = fx::packFindLight(t, packNode("mod:lamp_x", "glow.png"));
	UASSERT(l && l->color.getGreen() == 255);

	// glob in dir a matches glass_red, but b's exact key is still more specific
	const fx::Surface *s = fx::packFindSurface(t, packNode("mod:glass_red", "g.png"));
	UASSERT(s && s->kind == fx::SurfaceKind::None);
	s = fx::packFindSurface(t, packNode("mod:glass_blue", "g.png"));
	UASSERT(s && s->kind == fx::SurfaceKind::Glass && s->pattern == fx::SurfacePattern::Frosted);
	s = fx::packFindSurface(t, packNode("mod:ice", "ice.png"));
	UASSERT(s && s->kind == fx::SurfaceKind::Glass && s->pattern == fx::SurfacePattern::None);

	fs::RecursiveDelete(a);
	fs::RecursiveDelete(b);
}

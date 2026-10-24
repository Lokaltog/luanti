// Luanti "afx" fork: environmental audio
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Listening renders. Not a test: does nothing unless AFX_RENDER_DIR is set.
//
//   AFX_RENDER_DIR=/tmp/afx bin/luanti --run-unittests --test-module AfxRender
//
// Plays synthesized placeholder sounds through the real EfxRig on an OpenAL
// Soft loopback device, with the acoustics coming from the real probe on
// synthetic worlds, the real smoother and the real per-bus mix policy, at the
// engine's rates (probe 2.5 Hz, glide 60 Hz, EFX push 30 Hz, occlusion 7 Hz).
// Writes 16-bit stereo WAVs.

#include "test.h"
#include "config.h"

#if USE_SOUND

#include "afx_worlds.h"
#include "client/afx/afx_smoother.h"
#include "client/sound/efx.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <optional>
#include <random>
#include <string>
#include <vector>

using namespace afx;
using namespace afx_test;
using sound::EfxRig;
using sound::ListenerFrame;
using sound::swap_handedness;

namespace {

constexpr int RATE = 48000;
constexpr float STEP = 1.f / 60.f;

/*
	Placeholder sources
*/

using Pcm = std::vector<float>; // mono, or interleaved stereo

struct Synth {
	std::mt19937 rng{42};
	float noise() { return std::uniform_real_distribution<float>(-1.f, 1.f)(rng); }

	Pcm click()
	{
		Pcm p((size_t)(0.012f * RATE));
		for (size_t i = 0; i < p.size(); i++)
			p[i] = noise() * std::exp(-(float)i / (0.003f * RATE));
		return p;
	}

	Pcm footstep(float bright)
	{
		Pcm p((size_t)(0.12f * RATE));
		float lp = 0.f;
		const float a = 1.f - std::exp(-2.f * (float)M_PI * (600.f + 1400.f * bright) / RATE);
		for (size_t i = 0; i < p.size(); i++) {
			float t = (float)i / RATE;
			lp += (noise() - lp) * a;
			float scuff = lp * std::exp(-t / 0.035f) * 2.2f;
			float thump = std::sin(2.f * (float)M_PI * 85.f * t) * std::exp(-t / 0.025f) * 0.8f;
			p[i] = scuff + thump;
		}
		return p;
	}

	// A vowel-ish "hey!": glottal harmonics weighted by two formants.
	Pcm shout()
	{
		const float dur = 0.5f;
		Pcm p((size_t)(dur * RATE));
		float phase = 0.f;
		for (size_t i = 0; i < p.size(); i++) {
			float t = (float)i / RATE;
			float f0 = 270.f - 70.f * t / dur;
			phase += 2.f * (float)M_PI * f0 / RATE;
			float s = 0.f;
			for (int h = 1; h <= 24; h++) {
				float f = f0 * h;
				float w = std::exp(-std::pow((f - 650.f) / 220.f, 2.f)) +
						0.7f * std::exp(-std::pow((f - 1700.f) / 300.f, 2.f)) +
						0.25f * std::exp(-std::pow((f - 2600.f) / 400.f, 2.f));
				s += w * std::sin(phase * h) / std::sqrt((float)h);
			}
			float env = std::min(1.f, t / 0.03f) * std::min(1.f, (dur - t) / 0.15f);
			p[i] = s * env * 0.5f;
		}
		return p;
	}

	Pcm chirp()
	{
		Pcm p;
		float phase = 0.f;
		for (int k = 0; k < 3; k++) {
			size_t n = (size_t)(0.055f * RATE);
			for (size_t i = 0; i < n; i++) {
				float t = (float)i / n;
				float f = 3200.f + 1600.f * t + 300.f * std::sin(t * 40.f);
				phase += 2.f * (float)M_PI * f / RATE;
				p.push_back(std::sin(phase) * std::sin((float)M_PI * t) * 0.8f);
			}
			p.insert(p.end(), (size_t)(0.03f * RATE), 0.f);
		}
		return p;
	}

	Pcm drip(float pitch)
	{
		Pcm p((size_t)(0.25f * RATE));
		float phase = 0.f;
		for (size_t i = 0; i < p.size(); i++) {
			float t = (float)i / RATE;
			float f = pitch * (0.55f + 0.45f * std::exp(-t / 0.02f));
			phase += 2.f * (float)M_PI * f / RATE;
			p[i] = std::sin(phase) * std::exp(-t / 0.05f) * 0.9f + noise() * 0.3f * std::exp(-t / 0.002f);
		}
		return p;
	}

	// Stereo, loopable: filtered noise with slow gusts, decorrelated channels.
	Pcm wind(float seconds)
	{
		size_t n = (size_t)(seconds * RATE);
		Pcm p(n * 2);
		for (int ch = 0; ch < 2; ch++) {
			float lp1 = 0, lp2 = 0, bp = 0;
			for (size_t i = 0; i < n; i++) {
				float t = (float)i / n * 2.f * (float)M_PI; // one loop period
				float gust = 0.55f + 0.25f * std::sin(t * 2 + ch) + 0.2f * std::sin(t * 5 + 1.3f * ch);
				float cut = 250.f + 900.f * gust;
				float a = 1.f - std::exp(-2.f * (float)M_PI * cut / RATE);
				lp1 += (noise() - lp1) * a;
				lp2 += (lp1 - lp2) * a;
				bp += (noise() - bp) * 0.02f;
				p[i * 2 + ch] = (lp2 * 2.5f + bp * 0.6f) * gust;
			}
		}
		// Crossfade the seam so the loop is seamless
		size_t xf = RATE / 2;
		for (size_t i = 0; i < xf; i++) {
			float w = (float)i / xf;
			for (int ch = 0; ch < 2; ch++) {
				float &head = p[i * 2 + ch];
				float tail = p[(n - xf + i) * 2 + ch];
				head = head * w + tail * (1.f - w);
			}
		}
		p.resize((n - xf) * 2);
		return p;
	}

	// Stereo rain: hiss plus random drops.
	Pcm rain(float seconds)
	{
		size_t n = (size_t)(seconds * RATE);
		Pcm p(n * 2, 0.f);
		for (int ch = 0; ch < 2; ch++) {
			float hp = 0, prev = 0;
			for (size_t i = 0; i < n; i++) {
				float x = noise();
				hp = 0.97f * (hp + x - prev);
				prev = x;
				p[i * 2 + ch] = hp * 0.12f;
			}
		}
		std::uniform_real_distribution<float> u(0.f, 1.f);
		for (int d = 0; d < (int)(seconds * 140); d++) {
			size_t at = (size_t)(u(rng) * (n - RATE / 20));
			int ch = u(rng) < 0.5f ? 0 : 1;
			float amp = 0.15f + 0.4f * u(rng);
			float f = 2000.f + 4000.f * u(rng);
			for (size_t i = 0; i < (size_t)(RATE / 50); i++) {
				float t = (float)i / RATE;
				p[(at + i) * 2 + ch] += amp * std::sin(2.f * (float)M_PI * f * t) * std::exp(-t / 0.003f);
			}
		}
		return p;
	}
};

ALuint upload(const Pcm &pcm, int channels)
{
	std::vector<int16_t> s(pcm.size());
	for (size_t i = 0; i < pcm.size(); i++)
		s[i] = (int16_t)std::lround(std::clamp(pcm[i], -1.f, 1.f) * 32000.f);
	ALuint buf;
	alGenBuffers(1, &buf);
	alBufferData(buf, channels == 2 ? AL_FORMAT_STEREO16 : AL_FORMAT_MONO16, s.data(),
			(ALsizei)(s.size() * 2), RATE);
	return buf;
}

/*
	A tiny stand-in for OpenALSoundManager's afx path
*/

struct Voice {
	ALuint src;
	Bus bus;
	bool positional, stereo;
	v3f pos;
	Occlusion occ, occ_target;
};

class Session
{
public:
	ALCdevice *dev = nullptr;
	ALCcontext *ctx = nullptr;
	LPALCRENDERSAMPLESSOFT render = nullptr;
	int sends = 0;
	std::unique_ptr<EfxRig> rig;
	Params params;
	Smoother smoother;
	ListenerFrame frame;
	v3f ear;
	const World *world = nullptr;
	std::vector<Voice> voices;
	std::vector<float> out; // interleaved stereo
	float probe_timer = 0, apply_timer = 0, occl_timer = 0;
	float t = 0;
	bool enabled = true;

	bool open()
	{
		auto openfn = reinterpret_cast<LPALCLOOPBACKOPENDEVICESOFT>(
				alcGetProcAddress(nullptr, "alcLoopbackOpenDeviceSOFT"));
		if (!openfn || !(dev = openfn(nullptr)))
			return false;
		render = reinterpret_cast<LPALCRENDERSAMPLESSOFT>(
				alcGetProcAddress(dev, "alcRenderSamplesSOFT"));
		const ALCint attrs[] = {ALC_FORMAT_CHANNELS_SOFT, ALC_STEREO_SOFT,
			ALC_FORMAT_TYPE_SOFT, ALC_FLOAT_SOFT, ALC_FREQUENCY, RATE,
			ALC_HRTF_SOFT, ALC_FALSE, ALC_MAX_AUXILIARY_SENDS, 4, 0};
		ctx = alcCreateContext(dev, attrs);
		if (!ctx || !alcMakeContextCurrent(ctx))
			return false;
		alcGetIntegerv(dev, ALC_MAX_AUXILIARY_SENDS, 1, &sends);
		alDistanceModel(AL_INVERSE_DISTANCE_CLAMPED);
		rig = std::make_unique<EfxRig>(dev, sends);
		// Facing north (+Z): east is on the right
		frame.at = swap_handedness(v3f(0, 0, 1));
		frame.up = v3f(0, 1, 0);
		ALfloat ori[6] = {frame.at.X, frame.at.Y, frame.at.Z, 0, 1, 0};
		alListenerfv(AL_ORIENTATION, ori);
		return rig->available();
	}

	~Session()
	{
		stopAll();
		rig.reset();
		alcMakeContextCurrent(nullptr);
		if (ctx)
			alcDestroyContext(ctx);
		if (dev)
			alcCloseDevice(dev);
	}

	/// Teleport: the acoustics snap instead of gliding.
	void place(const World &w, v3f e, bool afx_on = true)
	{
		world = &w;
		enabled = afx_on;
		moveTo(e);
		Acoustics a = afx_on ? analyze(w.snap, ear, params) : Acoustics();
		if (!afx_on) {
			a.wet = 0;
			a.echo = 0;
		}
		smoother.reset(a);
		for (auto &v : voices)
			if (v.positional)
				v.occ = v.occ_target = occlusion(w.snap, ear, v.pos);
		applyAll();
	}

	void moveTo(v3f e)
	{
		ear = e;
		v3f s = swap_handedness(e);
		alListener3f(AL_POSITION, s.X, s.Y, s.Z);
	}

	Voice &play(ALuint buf, Bus bus, std::optional<v3f> pos, float gain, bool loop = false,
			bool stereo = false)
	{
		Voice v{};
		alGenSources(1, &v.src);
		v.bus = bus;
		v.positional = pos.has_value();
		v.stereo = stereo;
		alSourcei(v.src, AL_BUFFER, buf);
		alSourcei(v.src, AL_LOOPING, loop);
		if (v.positional) {
			v.pos = *pos;
			v3f s = swap_handedness(v.pos);
			alSource3f(v.src, AL_POSITION, s.X, s.Y, s.Z);
			alSourcef(v.src, AL_REFERENCE_DISTANCE, 1.f);
			alSourcef(v.src, AL_GAIN, gain * 3.f); // as PlayingSound::setGain
			if (world && enabled)
				v.occ = v.occ_target = occlusion(world->snap, ear, v.pos);
		} else {
			alSourcei(v.src, AL_SOURCE_RELATIVE, AL_TRUE);
			alSource3f(v.src, AL_POSITION, 0, 0, 0);
			alSourcef(v.src, AL_GAIN, gain);
		}
		voices.push_back(v);
		mix(voices.back());
		alSourcePlay(v.src);
		return voices.back();
	}

	void mix(Voice &v)
	{
		Params p = params;
		p.enabled = enabled;
		Mix m = mixFor(v.bus, smoother.current(), v.occ, p);
		if (!enabled || v.bus == Bus::Dry)
			rig->clearSource(v.src);
		else
			rig->applySource(v.src, m, v.positional, v.stereo, smoother.current(), frame);
	}

	void applyAll()
	{
		rig->applyEnvironment(smoother.current(), frame);
		for (auto &v : voices)
			mix(v);
	}

	/// Advance like the sound thread does, rendering as we go.
	void run(float seconds)
	{
		for (float left = seconds; left > 1e-6f; left -= STEP) {
			if (enabled && world) {
				if ((probe_timer -= STEP) <= 0) {
					probe_timer = params.probe_interval;
					smoother.setTarget(analyze(world->snap, ear, params));
				}
				smoother.step(STEP, params.smoothing);
				const bool recast = (occl_timer -= STEP) <= 0;
				if (recast)
					occl_timer = 0.15f;
				for (auto &v : voices) {
					if (!v.positional)
						continue;
					if (recast)
						v.occ_target = occlusion(world->snap, ear, v.pos);
					float k = 1.f - std::exp(-STEP / 0.25f);
					v.occ.amount += (v.occ_target.amount - v.occ.amount) * k;
					v.occ.hf += (v.occ_target.hf - v.occ.hf) * k;
				}
				if ((apply_timer -= STEP) <= 0) {
					apply_timer = 1.f / 30.f;
					applyAll();
				}
			}
			size_t frames = (size_t)std::lround(STEP * RATE);
			size_t at = out.size();
			out.resize(at + frames * 2);
			render(dev, out.data() + at, (ALCsizei)frames);
			t += STEP;
			reap();
		}
	}

	/// Walk from the current position to `to` at walking speed, stepping.
	void walk(v3f to, const std::vector<ALuint> &steps, float speed = 3.5f)
	{
		v3f from = ear;
		float len = (to - from).getLength();
		float dur = len / speed;
		int n = 0;
		for (float tt = 0; tt < dur; tt += 0.45f, n++) {
			if (!steps.empty())
				play(steps[n % steps.size()], Bus::Local, std::nullopt, 0.5f);
			float seg = std::min(0.45f, dur - tt);
			int sub = std::max(1, (int)(seg / STEP));
			for (int i = 0; i < sub; i++) {
				float k = std::min(1.f, (tt + (i + 1) * seg / sub) / dur);
				moveTo(from + (to - from) * k);
				run(seg / sub);
			}
		}
		moveTo(to);
	}

	void reap()
	{
		for (size_t i = 0; i < voices.size();) {
			ALint st;
			alGetSourcei(voices[i].src, AL_SOURCE_STATE, &st);
			if (st == AL_STOPPED) {
				alDeleteSources(1, &voices[i].src);
				voices[i] = voices.back();
				voices.pop_back();
			} else {
				i++;
			}
		}
	}

	void stopAll()
	{
		for (auto &v : voices) {
			alSourceStop(v.src);
			alDeleteSources(1, &v.src);
		}
		voices.clear();
	}

	void silence(float seconds) { out.insert(out.end(), (size_t)(seconds * RATE) * 2, 0.f); }

	bool write(const std::string &path)
	{
		float peak = 1e-6f;
		for (float s : out)
			peak = std::max(peak, std::fabs(s));
		const float g = 0.89f / peak; // -1 dBFS, one gain for the whole clip
		std::ofstream f(path, std::ios::binary);
		auto u32 = [&](uint32_t v) { f.write((const char *)&v, 4); };
		auto u16 = [&](uint16_t v) { f.write((const char *)&v, 2); };
		uint32_t bytes = (uint32_t)out.size() * 2;
		f.write("RIFF", 4); u32(36 + bytes); f.write("WAVEfmt ", 8);
		u32(16); u16(1); u16(2); u32(RATE); u32(RATE * 4); u16(4); u16(16);
		f.write("data", 4); u32(bytes);
		for (float s : out) {
			int16_t v = (int16_t)std::lround(std::clamp(s * g, -1.f, 1.f) * 32767.f);
			f.write((const char *)&v, 2);
		}
		out.clear();
		t = 0;
		return (bool)f;
	}
};

/*
	Worlds
*/

World tunnelHill()
{
	World w = openField();
	w.fill({-32, 0, -32}, {11, 20, 31}, Material::Stone);
	w.fill({-20, 0, -1}, {11, 2, 1}, Material::Air);
	w.markSky();
	return w;
}

World hut(bool door_open)
{
	World w = openField();
	w.fill({-4, 0, -4}, {4, 5, 4}, Material::Wood);
	w.fill({-3, 0, -3}, {3, 4, 3}, Material::Air);
	if (door_open)
		w.fill({0, 0, -4}, {0, 1, -4}, Material::Air);
	else
		w.fill({0, 0, -4}, {0, 1, -4}, Material::Leaky); // a closed door
	w.markSky();
	return w;
}

World cliff()
{
	World w = openField();
	w.fill({-32, 0, 20}, {31, 30, 31}, Material::Stone);
	w.markSky();
	return w;
}

World pool()
{
	World w = openField();
	w.fill({-32, -10, -32}, {31, 3, 31}, Material::Water);
	w.markSky();
	return w;
}

World cavern()
{
	// A big cavern deep in stone: 27 wide, 13 tall, 27 deep.
	World w;
	w.fill({-32, -32, -32}, {31, 24, 31}, Material::Stone);
	w.fill({-13, -6, -13}, {13, 6, 13}, Material::Air);
	w.markSky();
	return w;
}

} // namespace

class AfxRender : public TestBase
{
public:
	AfxRender() { TestManager::registerTestModule(this); }
	const char *getName() { return "AfxRender"; }

	void runTests(IGameDef *gamedef)
	{
		const char *dir = std::getenv("AFX_RENDER_DIR");
		if (!dir)
			return; // not a test; see the header comment
		TEST(renderAll, std::string(dir));
	}

	void renderAll(const std::string &dir)
	{
		Session s;
		UASSERT(s.open());
		Synth syn;

		ALuint click = upload(syn.click(), 1);
		std::vector<ALuint> steps;
		for (int i = 0; i < 4; i++)
			steps.push_back(upload(syn.footstep(0.3f + 0.15f * i), 1));
		ALuint shout = upload(syn.shout(), 1);
		ALuint chirp = upload(syn.chirp(), 1);
		std::vector<ALuint> drips;
		for (float f : {1400.f, 1800.f, 2300.f, 1100.f})
			drips.push_back(upload(syn.drip(f), 1));
		ALuint wind = upload(syn.wind(12.f), 2);
		ALuint rain = upload(syn.rain(10.f), 2);

		// 1. One-shots in each place
		{
			World field = openField(), cave = stoneCave(), tun = tunnelHill(), h = hut(false),
					cl = cliff(), pl = pool();
			struct Scene { const char *name; const World *w; v3f ear; bool afx; };
			const Scene scenes[] = {
				{"dry reference", &field, {0, 1.6f, 0}, false},
				{"open field", &field, {0, 1.6f, 0}, true},
				{"stone cave", &cave, {0, 0, 0}, true},
				{"tunnel, 20 deep", &tun, {-12, 1, 0}, true},
				{"wooden hut", &h, {0, 1.6f, 0}, true},
				{"cliff 20 north", &cl, {0, 1.6f, 0}, true},
				{"under water", &pl, {0, 0, 0}, true},
			};
			FILE *idx = std::fopen((dir + "/afx_oneshots.txt").c_str(), "w");
			for (const auto &sc : scenes) {
				if (idx)
					std::fprintf(idx, "%5.1fs  %s\n", s.t, sc.name);
				s.place(*sc.w, sc.ear, sc.afx);
				s.run(0.3f);
				s.play(click, Bus::Local, std::nullopt, 0.7f);
				s.run(1.3f);
				for (int i = 0; i < 4; i++) {
					s.play(steps[i], Bus::Local, std::nullopt, 0.6f);
					s.run(0.45f);
				}
				s.run(0.6f);
				s.play(shout, Bus::Local, std::nullopt, 0.8f);
				s.run(4.5f);
				s.stopAll();
				s.silence(0.6f);
				s.t += 0.6f;
			}
			if (idx)
				std::fclose(idx);
			UASSERT(s.write(dir + "/afx_oneshots.wav"));
		}

		// 2. Wind, walking into a tunnel (mouth to the east = your right) and out
		{
			World w = tunnelHill();
			s.place(w, {22, 1, 0});
			s.play(wind, Bus::Outdoor, std::nullopt, 0.7f, true, true);
			s.run(3.f);
			s.walk({12, 1, 0}, steps);   // to the mouth
			s.walk({-16, 1, 0}, steps);  // deep in
			s.run(5.f);
			s.play(shout, Bus::Local, std::nullopt, 0.6f);
			s.run(3.f);
			s.walk({22, 1, 0}, steps, 5.f); // back out
			s.run(3.f);
			s.stopAll();
			UASSERT(s.write(dir + "/afx_ambience_tunnel_wind.wav"));
		}

		// 3. Rain, into a wooden hut, door closes, opens, out
		{
			World open_door = hut(true), closed = hut(false);
			s.place(open_door, {0, 1.6f, -14});
			s.play(rain, Bus::Outdoor, std::nullopt, 0.7f, true, true);
			s.run(3.f);
			s.walk({0, 1.6f, -5}, steps);
			s.walk({0, 1.6f, 1}, steps);
			s.run(4.f);
			s.world = &closed; // the door shuts
			s.run(6.f);
			s.world = &open_door;
			s.run(3.f);
			s.walk({0, 1.6f, -14}, steps, 4.f);
			s.run(3.f);
			s.stopAll();
			UASSERT(s.write(dir + "/afx_ambience_hut_rain.wav"));
		}

		// 4. A bird 8 nodes north; things grow between you and it
		{
			World clear = openField(), hedge = openField(), wall = openField(), pillar = openField();
			hedge.fill({-30, 0, 5}, {30, 4, 5}, Material::Foliage);
			wall.fill({-30, 0, 5}, {30, 20, 6}, Material::Stone);
			pillar.fill({-1, 0, 5}, {1, 6, 5}, Material::Stone);
			const World *phases[] = {&clear, &hedge, &wall, &pillar, &clear};
			FILE *idx = std::fopen((dir + "/afx_occlusion.txt").c_str(), "w");
			const char *names[] = {"clear", "hedge of leaves", "stone wall", "3-wide stone pillar", "clear"};
			s.place(clear, {0, 1.6f, 0});
			s.play(wind, Bus::Outdoor, std::nullopt, 0.25f, true, true);
			for (int p = 0; p < 5; p++) {
				if (idx)
					std::fprintf(idx, "%5.1fs  %s\n", s.t, names[p]);
				s.world = phases[p];
				for (int k = 0; k < 5; k++) {
					s.play(chirp, Bus::World, v3f(0, 2, 8), 0.5f);
					s.run(1.f);
				}
			}
			if (idx)
				std::fclose(idx);
			s.stopAll();
			UASSERT(s.write(dir + "/afx_occlusion_bird.wav"));
		}

		// 5. A big cavern: drips from the ceiling, slow steps, a shout
		{
			World w = cavern();
			std::mt19937 rng(7);
			std::uniform_real_distribution<float> u(-12.f, 12.f), gap(0.4f, 1.8f);
			s.place(w, {-8, -4.4f, -8});
			s.play(wind, Bus::Outdoor, std::nullopt, 0.7f, true, true); // fully muffled here
			auto drip_for = [&](float seconds) {
				for (float tt = 0; tt < seconds;) {
					float g = gap(rng);
					s.play(drips[rng() % drips.size()], Bus::World, v3f(u(rng), 6, u(rng)), 0.35f);
					s.run(g);
					tt += g;
				}
			};
			drip_for(6.f);
			s.walk({6, -4.4f, 6}, steps, 1.6f);
			drip_for(3.f);
			s.play(shout, Bus::Local, std::nullopt, 0.8f);
			drip_for(8.f);
			s.stopAll();
			UASSERT(s.write(dir + "/afx_ambience_cavern.wav"));
		}

		for (ALuint b : {click, shout, chirp, wind, rain})
			alDeleteBuffers(1, &b);
		for (ALuint b : steps)
			alDeleteBuffers(1, &b);
		for (ALuint b : drips)
			alDeleteBuffers(1, &b);
	}
};

static AfxRender g_render_instance;

#endif // USE_SOUND

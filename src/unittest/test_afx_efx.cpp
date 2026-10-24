// Luanti "afx" fork: environmental audio
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Renders through a real OpenAL Soft loopback device (no sound card) and
// measures the output: reverb tails, muffling, and that echoes and outdoor
// beds land on the side they should.

#include "test.h"
#include "config.h"

#if USE_SOUND

#include "client/sound/efx.h"
#include "client/afx/afx_mix.h"

#include <cmath>
#include <random>
#include <vector>

using namespace sound;

namespace {

constexpr int RATE = 48000;

struct Loopback {
	ALCdevice *dev = nullptr;
	ALCcontext *ctx = nullptr;
	LPALCRENDERSAMPLESSOFT render = nullptr;
	int sends = 0;

	bool open()
	{
		if (!alcIsExtensionPresent(nullptr, "ALC_SOFT_loopback"))
			return false;
		auto openfn = reinterpret_cast<LPALCLOOPBACKOPENDEVICESOFT>(
				alcGetProcAddress(nullptr, "alcLoopbackOpenDeviceSOFT"));
		if (!openfn)
			return false;
		dev = openfn(nullptr);
		if (!dev)
			return false;
		render = reinterpret_cast<LPALCRENDERSAMPLESSOFT>(
				alcGetProcAddress(dev, "alcRenderSamplesSOFT"));
		const ALCint attrs[] = {
			ALC_FORMAT_CHANNELS_SOFT, ALC_STEREO_SOFT,
			ALC_FORMAT_TYPE_SOFT, ALC_FLOAT_SOFT,
			ALC_FREQUENCY, RATE,
			ALC_HRTF_SOFT, ALC_FALSE,
			ALC_MAX_AUXILIARY_SENDS, 6,
			0};
		ctx = alcCreateContext(dev, attrs);
		if (!ctx || !alcMakeContextCurrent(ctx))
			return false;
		alcGetIntegerv(dev, ALC_MAX_AUXILIARY_SENDS, 1, &sends);
		alDistanceModel(AL_INVERSE_DISTANCE_CLAMPED);
		return render != nullptr;
	}

	~Loopback()
	{
		alcMakeContextCurrent(nullptr);
		if (ctx)
			alcDestroyContext(ctx);
		if (dev)
			alcCloseDevice(dev);
	}

	/// Interleaved stereo float.
	std::vector<float> renderSeconds(float s)
	{
		std::vector<float> out((size_t)(s * RATE) * 2);
		render(dev, out.data(), (ALCsizei)(out.size() / 2));
		return out;
	}
};

ALuint makeBuffer(int channels, float seconds, bool burst)
{
	std::mt19937 rng(1234);
	std::uniform_real_distribution<float> d(-1.f, 1.f);
	size_t frames = (size_t)(seconds * RATE);
	std::vector<int16_t> pcm(frames * channels, 0);
	size_t active = burst ? (size_t)(0.01f * RATE) : frames;
	for (size_t i = 0; i < active * channels; i++)
		pcm[i] = (int16_t)(d(rng) * 20000.f);
	ALuint buf;
	alGenBuffers(1, &buf);
	alBufferData(buf, channels == 2 ? AL_FORMAT_STEREO16 : AL_FORMAT_MONO16, pcm.data(),
			(ALsizei)(pcm.size() * sizeof(int16_t)), RATE);
	return buf;
}

ALuint makeSource(ALuint buf, bool relative)
{
	ALuint src;
	alGenSources(1, &src);
	alSourcei(src, AL_BUFFER, buf);
	if (relative) {
		alSourcei(src, AL_SOURCE_RELATIVE, AL_TRUE);
		alSource3f(src, AL_POSITION, 0, 0, 0);
	}
	return src;
}

struct Energy { double l = 0, r = 0, hf = 0; double total() const { return l + r; } };

Energy measure(const std::vector<float> &s, float from, float to)
{
	Energy e;
	size_t a = (size_t)(from * RATE), b = std::min(s.size() / 2, (size_t)(to * RATE));
	for (size_t i = std::max<size_t>(a, 1); i < b; i++) {
		float l = s[2 * i], r = s[2 * i + 1];
		e.l += l * l;
		e.r += r * r;
		float dl = l - s[2 * (i - 1)];
		e.hf += dl * dl; // first difference: emphasises highs
	}
	return e;
}

afx::Acoustics cave()
{
	afx::Acoustics a;
	a.enclosure = 1.f;
	a.wet = 1.f;
	a.decay = 3.f;
	a.reflections_gain = 1.f;
	a.late_gain = 2.f;
	a.reflections_delay = 0.02f;
	a.late_delay = 0.03f;
	return a;
}

} // namespace

class TestAfxEfx : public TestBase
{
public:
	TestAfxEfx() { TestManager::registerTestModule(this); }
	const char *getName() { return "TestAfxEfx"; }

	void runTests(IGameDef *gamedef);

	void testReverbTail(Loopback &lb);
	void testOutdoorMuffle(Loopback &lb);
	void testEchoSide(Loopback &lb);
	void testBedSteering(Loopback &lb);
	void testNeighborTail(Loopback &lb);
	void testHollowSteps(Loopback &lb);
};

static TestAfxEfx g_test_instance;

void TestAfxEfx::runTests(IGameDef *gamedef)
{
	Loopback lb;
	if (!lb.open()) {
		rawstream << "TestAfxEfx: no OpenAL Soft loopback device, skipping" << std::endl;
		return;
	}
	rawstream << "TestAfxEfx: " << lb.sends << " aux sends" << std::endl;
	EfxRig probe(lb.dev, lb.sends);
	if (!probe.available()) {
		rawstream << "TestAfxEfx: no EFX, skipping" << std::endl;
		return;
	}
	TEST(testReverbTail, lb);
	TEST(testOutdoorMuffle, lb);
	TEST(testEchoSide, lb);
	TEST(testBedSteering, lb);
	TEST(testNeighborTail, lb);
	TEST(testHollowSteps, lb);
}

// The same click, dry vs in a stone cave: the cave keeps ringing.
void TestAfxEfx::testReverbTail(Loopback &lb)
{
	ListenerFrame frame;
	afx::Params params;
	ALuint buf = makeBuffer(1, 0.05f, true);

	auto run = [&](const afx::Acoustics &env) {
		EfxRig rig(lb.dev, lb.sends);
		rig.applyEnvironment(env, frame);
		ALuint src = makeSource(buf, true);
		rig.applySource(src, afx::mixFor(afx::Bus::Local, env, {}, params), false, false,
				env, frame);
		alSourcePlay(src);
		auto out = lb.renderSeconds(1.5f);
		alDeleteSources(1, &src);
		return out;
	};

	afx::Acoustics open; // wet 0
	auto dry = run(open);
	auto wet = run(cave());
	double dry_tail = measure(dry, 0.3f, 1.2f).total();
	double wet_tail = measure(wet, 0.3f, 1.2f).total();
	UASSERT(wet_tail > 1e-4);
	UASSERT(wet_tail > dry_tail * 100.0 + 1e-6);
	alDeleteBuffers(1, &buf);
}

// Outdoor noise through a tunnel is quieter and darker than in the open.
void TestAfxEfx::testOutdoorMuffle(Loopback &lb)
{
	ListenerFrame frame;
	afx::Params params;
	ALuint buf = makeBuffer(2, 1.f, false);

	auto run = [&](float gain, float hf) {
		EfxRig rig(lb.dev, lb.sends);
		afx::Acoustics env;
		env.outdoor_gain = gain;
		env.outdoor_hf = hf;
		rig.applyEnvironment(env, frame);
		ALuint src = makeSource(buf, true);
		rig.applySource(src, afx::mixFor(afx::Bus::Outdoor, env, {}, params), false, true,
				env, frame);
		alSourcePlay(src);
		auto out = lb.renderSeconds(0.5f);
		alDeleteSources(1, &src);
		return measure(out, 0.1f, 0.5f);
	};

	Energy outside = run(1.f, 1.f);
	Energy tunnel = run(0.4f, 0.1f);
	UASSERT(tunnel.total() < outside.total() * 0.5);
	// Brightness = HF energy relative to total; the low-pass must darken it
	UASSERT(tunnel.hf / tunnel.total() < 0.5 * outside.hf / outside.total());
	alDeleteBuffers(1, &buf);
}

// A cliff to the listener's right: its echo (carried on the reverb's early
// reflections) is louder on the right.
void TestAfxEfx::testEchoSide(Loopback &lb)
{
	ListenerFrame frame; // facing -Z in sound space; right is +X sound = -X node
	afx::Params params;
	ALuint buf = makeBuffer(1, 0.05f, true);

	auto run = [&](v3f node_dir) {
		EfxRig rig(lb.dev, lb.sends);
		afx::Acoustics env;
		env.wet = 1.f;
		env.reflections_gain = 3.f;
		env.reflections_delay = 0.15f;
		env.reflections_pan = node_dir * 0.9f;
		env.late_gain = 0.f;
		env.decay = 0.3f;
		rig.applyEnvironment(env, frame);
		ALuint src = makeSource(buf, true);
		afx::Mix mix = afx::mixFor(afx::Bus::Local, env, {}, params);
		mix.direct_gain = 0.f; // isolate the reflection
		rig.applySource(src, mix, false, false, env, frame);
		alSourcePlay(src);
		auto out = lb.renderSeconds(0.5f);
		alDeleteSources(1, &src);
		return measure(out, 0.12f, 0.24f);
	};

	UASSERT(frame.toLocal(v3f(-1, 0, 0)).X > 0.99f);
	Energy right = run(v3f(-1, 0, 0));
	Energy left = run(v3f(1, 0, 0));
	UASSERT(right.total() > 1e-6);
	UASSERT(right.r > right.l * 1.5);
	UASSERT(left.l > left.r * 1.5);
	alDeleteBuffers(1, &buf);
}

// Deep in a tunnel whose mouth is to the right, the outdoor bed comes from the right.
void TestAfxEfx::testBedSteering(Loopback &lb)
{
	ListenerFrame frame;
	afx::Params params;

	auto run = [&](int channels, v3f node_dir, float focus) {
		ALuint buf = makeBuffer(channels, 1.f, false);
		EfxRig rig(lb.dev, lb.sends);
		afx::Acoustics env;
		env.outdoor_gain = 1.f;
		env.outdoor_dir = node_dir;
		env.outdoor_focus = focus;
		rig.applyEnvironment(env, frame);
		ALuint src = makeSource(buf, true);
		rig.applySource(src, afx::mixFor(afx::Bus::Outdoor, env, {}, params), false,
				channels == 2, env, frame);
		alSourcePlay(src);
		auto out = lb.renderSeconds(0.5f);
		alDeleteSources(1, &src);
		alDeleteBuffers(1, &buf);
		return measure(out, 0.1f, 0.5f);
	};

	for (int ch : {1, 2}) {
		Energy focused = run(ch, v3f(-1, 0, 0), 1.f);
		Energy wide = run(ch, v3f(-1, 0, 0), 0.f);
		UASSERT(focused.r > focused.l * 2.0);
		// Unfocused (standing outside) stays roughly centred
		UASSERT(wide.r < wide.l * 1.6 && wide.l < wide.r * 1.6);
	}
}

// A dead hut with a cave through the door on the right: a click's long tail
// comes back from the right.
void TestAfxEfx::testNeighborTail(Loopback &lb)
{
	ListenerFrame frame;
	afx::Params params;
	ALuint buf = makeBuffer(1, 0.05f, true);
	auto run = [&](float coupling) {
		EfxRig rig(lb.dev, lb.sends);
		afx::Acoustics env;
		env.wet = 0.3f;
		env.decay = 0.4f;          // the hut: short
		env.coupling = coupling;
		env.neighbor_wet = 1.f;
		env.neighbor_decay = 4.f;  // the cave: long
		env.neighbor_dir = v3f(-1, 0, 0); // node -X = listener's right
		rig.applyEnvironment(env, frame);
		ALuint src = makeSource(buf, true);
		rig.applySource(src, afx::mixFor(afx::Bus::Local, env, {}, params), false, false,
				env, frame);
		alSourcePlay(src);
		auto out = lb.renderSeconds(2.f);
		alDeleteSources(1, &src);
		return measure(out, 0.6f, 1.8f);
	};
	Energy none = run(0.f), open = run(0.8f);
	UASSERT(open.total() > none.total() * 20.0 + 1e-7);
	UASSERT(open.r > open.l * 1.3);
	alDeleteBuffers(1, &buf);
}

// Footsteps on boards over a void get body: more low end than on the ground.
void TestAfxEfx::testHollowSteps(Loopback &lb)
{
	ListenerFrame frame;
	afx::Params params;
	ALuint buf = makeBuffer(1, 0.08f, true);
	auto run = [&](float hollow) {
		EfxRig rig(lb.dev, lb.sends);
		afx::Acoustics env;
		env.hollow = hollow;
		rig.applyEnvironment(env, frame);
		ALuint src = makeSource(buf, true);
		rig.applySource(src, afx::mixFor(afx::Bus::Local, env, {}, params, true), false,
				false, env, frame);
		alSourcePlay(src);
		auto out = lb.renderSeconds(0.4f);
		alDeleteSources(1, &src);
		return out;
	};
	auto low = [](const std::vector<float> &x) {
		// energy below ~250 Hz (one-pole low-pass, both channels)
		double e = 0, l = 0, r = 0;
		const double k = 1.0 - std::exp(-2.0 * M_PI * 250.0 / RATE);
		for (size_t i = 0; i + 1 < x.size(); i += 2) {
			l += (x[i] - l) * k;
			r += (x[i + 1] - r) * k;
			e += l * l + r * r;
		}
		return e;
	};
	const double solid = low(run(0.f)), hollow = low(run(1.f));
	UASSERT(hollow > solid * 2.0);
	alDeleteBuffers(1, &buf);
}

#endif // USE_SOUND

// Luanti "afx" fork: environmental audio
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// OpenAL EFX plumbing: one EAX reverb slot (the room the listener is in), one
// echo slot and one "behind a wall" equalizer slot, plus per-source low-pass
// filters and aux sends. Lives on the
// sound thread. All levels go through filters, never AL_GAIN, so the engine's
// own fades keep working.

#pragma once

#include "al_helpers.h"
#include "client/afx/afx_mix.h"
#include <vector>

#if __has_include(<AL/efx.h>)
	#include <AL/efx.h>
	#define AFX_HAVE_EFX 1
#elif __has_include(<efx.h>)
	#include <efx.h>
	#define AFX_HAVE_EFX 1
#else
	#define AFX_HAVE_EFX 0
#endif

namespace sound {

/// The listener's frame in sound space (right-handed, OpenAL).
struct ListenerFrame {
	v3f at{0, 0, -1};
	v3f up{0, 1, 0};

	/// Node-space direction → listener-local (x right, y up, z forward).
	v3f toLocal(v3f node_dir) const;
};

class EfxRig
{
public:
	/// Needs the context current on this thread. `sends` is the number of aux
	/// sends per source the context actually granted.
	EfxRig(ALCdevice *device, int sends);
	~EfxRig();

	DISABLE_CLASS_COPY(EfxRig)

	bool available() const { return m_ok; }
	bool hasEaxReverb() const { return m_eax; }
	int sends() const { return m_sends; }

	/// Push the (smoothed) environment into the effect slots.
	/// `dt` (seconds since the last call) moves the recorded-space fades.
	void applyEnvironment(const afx::Acoustics &env, const ListenerFrame &frame,
			float dt = 0.f);

	/// Route one source. `stereo` enables stereo-angle steering for beds.
	void applySource(ALuint source, const afx::Mix &mix, bool positional, bool stereo,
			const afx::Acoustics &env, const ListenerFrame &frame);

	/// Walls you can hear (slot 1 with EAX reverb)
	void applyWalls(const afx::Acoustics &env, const ListenerFrame &frame);
	bool hasWalls() const { return m_walls; }

	/// The far space a source sits in (send 5): its tail, panned toward it.
	void applyRemote(const afx::RemoteSpace &r, const ListenerFrame &frame);
	bool hasRemote() const { return m_remote_slot != 0; }

	/// The recorded space the room's reverb blends in (convolution, send 3):
	/// which one (index into afx::RECORDED_SPACES, -1 none), its impulse
	/// response, and its share of the reverb. Takes effect, faded, over the
	/// next applyEnvironment calls; a change of space fades out, swaps and
	/// fades back in.
	void setRecordedSpace(int index, ALuint ir_buffer, float share);
	bool hasConvolution() const { return m_conv_effect != 0; }
	int recordedSpace() const { return m_conv_index; }
	float recordedShare() const { return m_conv_share; }

	/// Disconnect a source from all sends and filters (afx disabled).
	void clearSource(ALuint source);
	/// Also undo bed steering (stereo angles, source radius).
	void resetSource(ALuint source, bool stereo);

	/// Context attributes asking for enough aux sends; zero-terminated.
	static const ALCint *contextAttributes();

private:
	bool m_ok = false;
	bool m_eax = false;
	bool m_stereo_angles = false;
	bool m_source_radius = false;
	int m_sends = 0;

#if AFX_HAVE_EFX
	LPALGENEFFECTS alGenEffects = nullptr;
	LPALDELETEEFFECTS alDeleteEffects = nullptr;
	LPALEFFECTI alEffecti = nullptr;
	LPALEFFECTF alEffectf = nullptr;
	LPALEFFECTFV alEffectfv = nullptr;
	LPALGENFILTERS alGenFilters = nullptr;
	LPALDELETEFILTERS alDeleteFilters = nullptr;
	LPALFILTERI alFilteri = nullptr;
	LPALFILTERF alFilterf = nullptr;
	LPALGENAUXILIARYEFFECTSLOTS alGenAuxiliaryEffectSlots = nullptr;
	LPALDELETEAUXILIARYEFFECTSLOTS alDeleteAuxiliaryEffectSlots = nullptr;
	LPALAUXILIARYEFFECTSLOTI alAuxiliaryEffectSloti = nullptr;
	LPALAUXILIARYEFFECTSLOTF alAuxiliaryEffectSlotf = nullptr;
#endif

	ALuint m_reverb_effect = 0, m_echo_effect = 0;
	ALuint m_muffle_effect = 0;
	ALuint m_reverb_slot = 0, m_echo_slot = 0, m_muffle_slot = 0;
	// The space next door (a cave beyond the doorway) and the space a far
	// source sits in share one slot (send 5): a far space's tail, panned
	// toward it. Whichever is louder sets it; each source's send carries
	// the levels.
	float m_nb_level = 0.f, m_rm_level = 0.f;
	struct FarSpace { float decay = -1.f, hf = -1.f, refl = 0.f, late = 0.f, refl_delay = 0.f,
			late_delay = 0.f, gainhf = 0.f; v3f pan; };
	FarSpace m_nb_space, m_rm_space, m_far_last;
	void updateFar();
	// Recorded spaces (send 3): a slot per impulse response, since a slot
	// can't change its buffer once it has played; the old one is deleted
	// when no source uses it any more
	ALuint m_conv_effect = 0, m_conv_slot = 0;
	std::vector<ALuint> m_conv_retired;
	int m_conv_index = -1, m_conv_want = -1;
	ALuint m_conv_want_buffer = 0;
	ALuint m_conv_failed_buffer = 0; // a response the effect wouldn't take
	float m_conv_share = 0.f, m_conv_want_share = 0.f;
	void stepConvolution(const afx::Acoustics &env, float dt);
	// the cavity under a hollow floor: a short, low, boomy ring
	ALuint m_hollow_effect = 0, m_hollow_slot = 0;
	// where a source is: a cave's tail on a zombie in it, while you stand
	// somewhere dead
	ALuint m_remote_effect = 0, m_remote_slot = 0;
	bool m_walls = false;
	float m_walls_t[2] = {-1.f, -1.f}, m_walls_far = -1.f, m_walls_last_level = 1.f;
	v3f m_walls_pan[2];
	ALuint m_filter = 0; // scratch: filter params are copied when attached

	// Last values pushed to the effects, to skip redundant reloads
	afx::Acoustics m_last;
	bool m_have_last = false;

	void setLowpass(float gain, float gainhf);
};

} // namespace sound

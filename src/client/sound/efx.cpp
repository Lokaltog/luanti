// Luanti "afx" fork: environmental audio
// SPDX-License-Identifier: LGPL-2.1-or-later

#include "efx.h"

#include <algorithm>
#include <cmath>

namespace sound {

v3f ListenerFrame::toLocal(v3f node_dir) const
{
	v3f d = swap_handedness(node_dir);
	v3f right = at.crossProduct(up);
	if (right.getLengthSQ() > 1e-8f)
		right.normalize();
	return v3f(d.dotProduct(right), d.dotProduct(up), d.dotProduct(at));
}

#if !AFX_HAVE_EFX

EfxRig::EfxRig(ALCdevice *, int) {}
EfxRig::~EfxRig() = default;
void EfxRig::applyEnvironment(const afx::Acoustics &, const ListenerFrame &) {}
void EfxRig::applySource(ALuint, const afx::Mix &, bool, bool, const afx::Acoustics &,
		const ListenerFrame &) {}
void EfxRig::clearSource(ALuint) {}
void EfxRig::setLowpass(float, float) {}
const ALCint *EfxRig::contextAttributes() { return nullptr; }

#else

static float clampf(float v, float lo, float hi) { return std::max(lo, std::min(hi, v)); }
static float lerpf(float a, float b, float t) { return a + (b - a) * t; }

const ALCint *EfxRig::contextAttributes()
{
	static const ALCint attrs[] = {ALC_MAX_AUXILIARY_SENDS, 6, 0};
	return attrs;
}

template <typename T>
static bool loadProc(T &fn, const char *name)
{
	fn = reinterpret_cast<T>(alGetProcAddress(name));
	return fn != nullptr;
}

EfxRig::EfxRig(ALCdevice *device, int sends) : m_sends(sends)
{
	if (!alcIsExtensionPresent(device, "ALC_EXT_EFX")) {
		infostream << "afx: ALC_EXT_EFX not present, environmental audio off" << std::endl;
		return;
	}
	bool ok = loadProc(alGenEffects, "alGenEffects") &&
			loadProc(alDeleteEffects, "alDeleteEffects") &&
			loadProc(alEffecti, "alEffecti") &&
			loadProc(alEffectf, "alEffectf") &&
			loadProc(alEffectfv, "alEffectfv") &&
			loadProc(alGenFilters, "alGenFilters") &&
			loadProc(alDeleteFilters, "alDeleteFilters") &&
			loadProc(alFilteri, "alFilteri") &&
			loadProc(alFilterf, "alFilterf") &&
			loadProc(alGenAuxiliaryEffectSlots, "alGenAuxiliaryEffectSlots") &&
			loadProc(alDeleteAuxiliaryEffectSlots, "alDeleteAuxiliaryEffectSlots") &&
			loadProc(alAuxiliaryEffectSloti, "alAuxiliaryEffectSloti") &&
			loadProc(alAuxiliaryEffectSlotf, "alAuxiliaryEffectSlotf");
	if (!ok || m_sends < 1) {
		warningstream << "afx: EFX entry points missing or no aux sends ("
				<< m_sends << "), environmental audio off" << std::endl;
		return;
	}
	alGetError();

	alGenEffects(1, &m_reverb_effect);
	alEffecti(m_reverb_effect, AL_EFFECT_TYPE, AL_EFFECT_EAXREVERB);
	m_eax = alGetError() == AL_NO_ERROR;
	if (!m_eax) {
		alEffecti(m_reverb_effect, AL_EFFECT_TYPE, AL_EFFECT_REVERB);
		alGetError();
	}

	alGenAuxiliaryEffectSlots(1, &m_reverb_slot);
	alAuxiliaryEffectSlotf(m_reverb_slot, AL_EFFECTSLOT_GAIN, 0.f);

	if (m_sends >= 2) {
		alGenEffects(1, &m_echo_effect);
		alEffecti(m_echo_effect, AL_EFFECT_TYPE, AL_EFFECT_ECHO);
		alGenAuxiliaryEffectSlots(1, &m_echo_slot);
		alAuxiliaryEffectSlotf(m_echo_slot, AL_EFFECTSLOT_GAIN, 0.f);
	}

	if (m_sends >= 3) {
		// Behind a wall: a steep low-pass the per-source shelf filter can't
		// do. Lows through, -10 dB at 700 Hz, -18 dB around 2.5 kHz, and the
		// high shelf stacks another -18 dB on top.
		alGenEffects(1, &m_muffle_effect);
		alEffecti(m_muffle_effect, AL_EFFECT_TYPE, AL_EFFECT_EQUALIZER);
		alEffectf(m_muffle_effect, AL_EQUALIZER_LOW_GAIN, 1.6f);
		alEffectf(m_muffle_effect, AL_EQUALIZER_LOW_CUTOFF, 180.f);
		alEffectf(m_muffle_effect, AL_EQUALIZER_MID1_GAIN, 0.32f);
		alEffectf(m_muffle_effect, AL_EQUALIZER_MID1_CENTER, 700.f);
		alEffectf(m_muffle_effect, AL_EQUALIZER_MID1_WIDTH, 1.0f);
		alEffectf(m_muffle_effect, AL_EQUALIZER_MID2_GAIN, 0.126f);
		alEffectf(m_muffle_effect, AL_EQUALIZER_MID2_CENTER, 2500.f);
		alEffectf(m_muffle_effect, AL_EQUALIZER_MID2_WIDTH, 1.0f);
		alEffectf(m_muffle_effect, AL_EQUALIZER_HIGH_GAIN, 0.126f);
		alEffectf(m_muffle_effect, AL_EQUALIZER_HIGH_CUTOFF, 4000.f);
		alGenAuxiliaryEffectSlots(1, &m_muffle_slot);
		alAuxiliaryEffectSloti(m_muffle_slot, AL_EFFECTSLOT_EFFECT, m_muffle_effect);
		alAuxiliaryEffectSlotf(m_muffle_slot, AL_EFFECTSLOT_GAIN, 1.f);
	}

	if (m_sends >= 5 && m_eax) {
		const ALuint e = m_hollow_effect = [&] { ALuint id; alGenEffects(1, &id); return id; }();
		alEffecti(e, AL_EFFECT_TYPE, AL_EFFECT_EAXREVERB);
		alEffectf(e, AL_EAXREVERB_DENSITY, 1.f);
		alEffectf(e, AL_EAXREVERB_DIFFUSION, 0.6f);
		alEffectf(e, AL_EAXREVERB_GAIN, 0.9f);
		alEffectf(e, AL_EAXREVERB_GAINHF, 0.08f);
		alEffectf(e, AL_EAXREVERB_GAINLF, 1.f);
		alEffectf(e, AL_EAXREVERB_DECAY_TIME, 0.35f);
		alEffectf(e, AL_EAXREVERB_DECAY_HFRATIO, 0.1f);
		alEffectf(e, AL_EAXREVERB_DECAY_LFRATIO, 2.0f);
		alEffectf(e, AL_EAXREVERB_REFLECTIONS_GAIN, 3.0f);
		alEffectf(e, AL_EAXREVERB_REFLECTIONS_DELAY, 0.004f);
		alEffectf(e, AL_EAXREVERB_LATE_REVERB_GAIN, 3.0f);
		alEffectf(e, AL_EAXREVERB_LATE_REVERB_DELAY, 0.006f);
		alEffectf(e, AL_EAXREVERB_HFREFERENCE, 1500.f);
		alEffectf(e, AL_EAXREVERB_LFREFERENCE, 180.f);
		alGenAuxiliaryEffectSlots(1, &m_hollow_slot);
		alAuxiliaryEffectSloti(m_hollow_slot, AL_EFFECTSLOT_EFFECT, e);
		alAuxiliaryEffectSlotf(m_hollow_slot, AL_EFFECTSLOT_GAIN, 1.f);
	}

	if (m_sends >= 4 && m_eax) {
		alGenEffects(1, &m_nb_effect);
		alEffecti(m_nb_effect, AL_EFFECT_TYPE, AL_EFFECT_EAXREVERB);
		alGenAuxiliaryEffectSlots(1, &m_nb_slot);
		alAuxiliaryEffectSlotf(m_nb_slot, AL_EFFECTSLOT_GAIN, 0.f);
	}

	alGenFilters(1, &m_filter);
	alFilteri(m_filter, AL_FILTER_TYPE, AL_FILTER_LOWPASS);

	if (warn_if_al_error("afx: creating EFX objects") != AL_NO_ERROR)
		return;

	m_stereo_angles = alIsExtensionPresent("AL_EXT_STEREO_ANGLES");
	m_source_radius = alIsExtensionPresent("AL_EXT_SOURCE_RADIUS");
	m_ok = true;
	infostream << "afx: EFX ready, " << (m_eax ? "EAX reverb" : "standard reverb")
			<< ", " << m_sends << " sends"
			<< (m_stereo_angles ? ", stereo angles" : "")
			<< (m_source_radius ? ", source radius" : "") << std::endl;
}

EfxRig::~EfxRig()
{
	if (!alDeleteEffects)
		return;
	if (m_reverb_slot)
		alDeleteAuxiliaryEffectSlots(1, &m_reverb_slot);
	if (m_echo_slot)
		alDeleteAuxiliaryEffectSlots(1, &m_echo_slot);
	if (m_muffle_slot)
		alDeleteAuxiliaryEffectSlots(1, &m_muffle_slot);
	if (m_nb_slot)
		alDeleteAuxiliaryEffectSlots(1, &m_nb_slot);
	if (m_hollow_slot)
		alDeleteAuxiliaryEffectSlots(1, &m_hollow_slot);
	if (m_hollow_effect)
		alDeleteEffects(1, &m_hollow_effect);
	if (m_nb_effect)
		alDeleteEffects(1, &m_nb_effect);
	if (m_muffle_effect)
		alDeleteEffects(1, &m_muffle_effect);
	if (m_reverb_effect)
		alDeleteEffects(1, &m_reverb_effect);
	if (m_echo_effect)
		alDeleteEffects(1, &m_echo_effect);
	if (m_filter)
		alDeleteFilters(1, &m_filter);
}

static bool differs(float a, float b, float rel = 0.01f, float abs_eps = 1e-3f)
{
	return std::fabs(a - b) > std::max(abs_eps, rel * std::max(std::fabs(a), std::fabs(b)));
}

static bool differs(v3f a, v3f b) { return (a - b).getLengthSQ() > 1e-4f; }

void EfxRig::applyEnvironment(const afx::Acoustics &env, const ListenerFrame &frame)
{
	if (!m_ok)
		return;

	alAuxiliaryEffectSlotf(m_reverb_slot, AL_EFFECTSLOT_GAIN, clampf(env.wet, 0.f, 1.f));
	if (m_echo_slot)
		alAuxiliaryEffectSlotf(m_echo_slot, AL_EFFECTSLOT_GAIN,
				clampf(env.echo * env.echo_feedback * 2.f, 0.f, 1.f)); // flutter only

	// Pans are listener-relative and change as the player turns, so they are
	// recomputed every apply; the rest only reloads on real change.
	const v3f refl_pan = frame.toLocal(env.reflections_pan);
	const v3f echo_local = frame.toLocal(env.echo_dir);
	const afx::Acoustics &l = m_last;
	bool reverb_dirty = !m_have_last ||
			differs(env.decay, l.decay) || differs(env.decay_hf_ratio, l.decay_hf_ratio) ||
			differs(env.density, l.density) || differs(env.diffusion, l.diffusion) ||
			differs(env.reflections_gain, l.reflections_gain) ||
			differs(env.reflections_delay, l.reflections_delay) ||
			differs(env.late_gain, l.late_gain) || differs(env.late_delay, l.late_delay) ||
			differs(env.hf, l.hf) || differs(env.submerged, l.submerged) ||
			differs(refl_pan, l.reflections_pan);
	bool echo_dirty = m_echo_slot && (!m_have_last ||
			differs(env.echo_delay, l.echo_delay) || differs(env.echo_feedback, l.echo_feedback) ||
			differs(env.echo_damping, l.echo_damping) || differs(echo_local, l.echo_dir));

	if (reverb_dirty) {
		// Underwater the room sounds small, dense and dark.
		const float s = env.submerged;
		const float decay = clampf(lerpf(env.decay, 0.6f, s), 0.1f, 20.f);
		const float gainhf = clampf(lerpf(0.6f + 0.39f * env.hf, 0.15f, s), 0.f, 1.f);
		if (m_eax) {
			const ALuint e = m_reverb_effect;
			alEffectf(e, AL_EAXREVERB_DENSITY, clampf(lerpf(env.density, 1.f, s), 0.f, 1.f));
			alEffectf(e, AL_EAXREVERB_DIFFUSION, clampf(env.diffusion, 0.f, 1.f));
			alEffectf(e, AL_EAXREVERB_GAIN, 0.32f);
			alEffectf(e, AL_EAXREVERB_GAINHF, gainhf);
			alEffectf(e, AL_EAXREVERB_GAINLF, 1.f);
			alEffectf(e, AL_EAXREVERB_DECAY_TIME, decay);
			alEffectf(e, AL_EAXREVERB_DECAY_HFRATIO, clampf(env.decay_hf_ratio, 0.1f, 2.f));
			alEffectf(e, AL_EAXREVERB_DECAY_LFRATIO, 1.f);
			alEffectf(e, AL_EAXREVERB_REFLECTIONS_GAIN, clampf(env.reflections_gain, 0.f, 3.16f));
			alEffectf(e, AL_EAXREVERB_REFLECTIONS_DELAY, clampf(env.reflections_delay, 0.f, 0.3f));
			ALfloat rp[3] = {refl_pan.X, refl_pan.Y, refl_pan.Z};
			alEffectfv(e, AL_EAXREVERB_REFLECTIONS_PAN, rp);
			alEffectf(e, AL_EAXREVERB_LATE_REVERB_GAIN, clampf(env.late_gain, 0.f, 10.f));
			alEffectf(e, AL_EAXREVERB_LATE_REVERB_DELAY, clampf(env.late_delay, 0.f, 0.1f));
			ALfloat lp[3] = {0.f, 0.f, 0.f};
			alEffectfv(e, AL_EAXREVERB_LATE_REVERB_PAN, lp);
			alEffectf(e, AL_EAXREVERB_ECHO_DEPTH, 0.f);
			alEffectf(e, AL_EAXREVERB_MODULATION_DEPTH, 0.f);
			alEffectf(e, AL_EAXREVERB_AIR_ABSORPTION_GAINHF, 0.994f);
			alEffecti(e, AL_EAXREVERB_DECAY_HFLIMIT, AL_TRUE);
		} else {
			const ALuint e = m_reverb_effect;
			alEffectf(e, AL_REVERB_DENSITY, clampf(env.density, 0.f, 1.f));
			alEffectf(e, AL_REVERB_DIFFUSION, clampf(env.diffusion, 0.f, 1.f));
			alEffectf(e, AL_REVERB_GAIN, 0.32f);
			alEffectf(e, AL_REVERB_GAINHF, gainhf);
			alEffectf(e, AL_REVERB_DECAY_TIME, decay);
			alEffectf(e, AL_REVERB_DECAY_HFRATIO, clampf(env.decay_hf_ratio, 0.1f, 2.f));
			alEffectf(e, AL_REVERB_REFLECTIONS_GAIN, clampf(env.reflections_gain, 0.f, 3.16f));
			alEffectf(e, AL_REVERB_REFLECTIONS_DELAY, clampf(env.reflections_delay, 0.f, 0.3f));
			alEffectf(e, AL_REVERB_LATE_REVERB_GAIN, clampf(env.late_gain, 0.f, 10.f));
			alEffectf(e, AL_REVERB_LATE_REVERB_DELAY, clampf(env.late_delay, 0.f, 0.1f));
			alEffecti(e, AL_REVERB_DECAY_HFLIMIT, AL_TRUE);
		}
		// Re-attaching is what makes the slot pick up the new properties.
		alAuxiliaryEffectSloti(m_reverb_slot, AL_EFFECTSLOT_EFFECT, m_reverb_effect);
		m_last.decay = env.decay;
		m_last.decay_hf_ratio = env.decay_hf_ratio;
		m_last.density = env.density;
		m_last.diffusion = env.diffusion;
		m_last.reflections_gain = env.reflections_gain;
		m_last.reflections_delay = env.reflections_delay;
		m_last.late_gain = env.late_gain;
		m_last.late_delay = env.late_delay;
		m_last.hf = env.hf;
		m_last.submerged = env.submerged;
		m_last.reflections_pan = refl_pan;
	}

	if (echo_dirty) {
		const ALuint e = m_echo_effect;
		const float t = clampf(env.echo_delay, 0.f, 0.207f);
		// One tap at the wall's round trip (no ping-pong), panned toward it;
		// feedback only for parallel walls.
		alEffectf(e, AL_ECHO_DELAY, t);
		alEffectf(e, AL_ECHO_LRDELAY, 0.f);
		alEffectf(e, AL_ECHO_DAMPING, clampf(env.echo_damping, 0.f, 0.99f));
		alEffectf(e, AL_ECHO_FEEDBACK, clampf(env.echo_feedback, 0.f, 1.f));
		alEffectf(e, AL_ECHO_SPREAD, clampf(-echo_local.X, -1.f, 1.f));
		alAuxiliaryEffectSloti(m_echo_slot, AL_EFFECTSLOT_EFFECT, m_echo_effect);
		m_last.echo_delay = env.echo_delay;
		m_last.echo_feedback = env.echo_feedback;
		m_last.echo_damping = env.echo_damping;
		m_last.echo_dir = echo_local;
	}
	if (m_nb_slot) {
		alAuxiliaryEffectSlotf(m_nb_slot, AL_EFFECTSLOT_GAIN,
				clampf(env.coupling * env.neighbor_wet * 1.4f, 0.f, 1.f));
		const v3f pan = frame.toLocal(env.neighbor_dir) * 0.85f;
		if (std::fabs(env.neighbor_decay - m_nb_last_decay) > 0.05f ||
				std::fabs(env.neighbor_hf_ratio - m_nb_last_hf) > 0.05f ||
				(pan - m_nb_last_pan).getLengthSQ() > 1e-3f) {
			const ALuint e = m_nb_effect;
			alEffectf(e, AL_EAXREVERB_DENSITY, 1.f);
			alEffectf(e, AL_EAXREVERB_DIFFUSION, 1.f);
			alEffectf(e, AL_EAXREVERB_GAIN, 0.32f);
			alEffectf(e, AL_EAXREVERB_GAINHF, 0.7f);
			alEffectf(e, AL_EAXREVERB_DECAY_TIME, clampf(env.neighbor_decay, 0.1f, 20.f));
			alEffectf(e, AL_EAXREVERB_DECAY_HFRATIO, clampf(env.neighbor_hf_ratio, 0.1f, 2.f));
			// No early reflections: we hear the far space's tail, arriving late
			alEffectf(e, AL_EAXREVERB_REFLECTIONS_GAIN, 0.05f);
			alEffectf(e, AL_EAXREVERB_LATE_REVERB_GAIN, 1.6f);
			alEffectf(e, AL_EAXREVERB_LATE_REVERB_DELAY, 0.04f);
			ALfloat lp[3] = {pan.X, pan.Y, pan.Z};
			alEffectfv(e, AL_EAXREVERB_LATE_REVERB_PAN, lp);
			alEffectfv(e, AL_EAXREVERB_REFLECTIONS_PAN, lp);
			alEffecti(e, AL_EAXREVERB_DECAY_HFLIMIT, AL_TRUE);
			alAuxiliaryEffectSloti(m_nb_slot, AL_EFFECTSLOT_EFFECT, e);
			m_nb_last_decay = env.neighbor_decay;
			m_nb_last_hf = env.neighbor_hf_ratio;
			m_nb_last_pan = pan;
		}
	}
	m_have_last = true;
	warn_if_al_error("afx: applyEnvironment");
}

void EfxRig::setLowpass(float gain, float gainhf)
{
	alFilterf(m_filter, AL_LOWPASS_GAIN, clampf(gain, 0.f, 1.f));
	alFilterf(m_filter, AL_LOWPASS_GAINHF, clampf(gainhf, 0.f, 1.f));
}

void EfxRig::applySource(ALuint src, const afx::Mix &mix, bool positional, bool stereo,
		const afx::Acoustics &env, const ListenerFrame &frame)
{
	if (!m_ok)
		return;

	setLowpass(mix.direct_gain, mix.direct_hf);
	alSourcei(src, AL_DIRECT_FILTER, m_filter);

	if (mix.reverb_send > 1e-3f) {
		setLowpass(mix.reverb_send, mix.reverb_hf);
		alSource3i(src, AL_AUXILIARY_SEND_FILTER, m_reverb_slot, 0, m_filter);
	} else {
		alSource3i(src, AL_AUXILIARY_SEND_FILTER, AL_EFFECTSLOT_NULL, 0, AL_FILTER_NULL);
	}
	if (m_echo_slot) {
		if (mix.echo_send > 1e-3f) {
			setLowpass(mix.echo_send, 1.f);
			alSource3i(src, AL_AUXILIARY_SEND_FILTER, m_echo_slot, 1, m_filter);
		} else {
			alSource3i(src, AL_AUXILIARY_SEND_FILTER, AL_EFFECTSLOT_NULL, 1, AL_FILTER_NULL);
		}
	}

	if (m_hollow_slot) {
		if (mix.hollow_send > 1e-3f) {
			setLowpass(std::min(1.f, mix.hollow_send), 0.3f);
			alSource3i(src, AL_AUXILIARY_SEND_FILTER, m_hollow_slot, 4, m_filter);
		} else {
			alSource3i(src, AL_AUXILIARY_SEND_FILTER, AL_EFFECTSLOT_NULL, 4, AL_FILTER_NULL);
		}
	}

	if (m_nb_slot) {
		if (mix.neighbor_send > 1e-3f) {
			setLowpass(mix.neighbor_send, 0.8f);
			alSource3i(src, AL_AUXILIARY_SEND_FILTER, m_nb_slot, 3, m_filter);
		} else {
			alSource3i(src, AL_AUXILIARY_SEND_FILTER, AL_EFFECTSLOT_NULL, 3, AL_FILTER_NULL);
		}
	}

	if (m_muffle_slot) {
		if (mix.muffle_send > 1e-3f) {
			setLowpass(mix.muffle_send, 0.5f);
			alSource3i(src, AL_AUXILIARY_SEND_FILTER, m_muffle_slot, 2, m_filter);
		} else {
			alSource3i(src, AL_AUXILIARY_SEND_FILTER, AL_EFFECTSLOT_NULL, 2, AL_FILTER_NULL);
		}
	}

	if (positional)
		alSourcef(src, AL_AIR_ABSORPTION_FACTOR, 3.f);

	if (mix.spatial_bed) {
		// Steer the bed toward the opening; the deeper inside, the narrower.
		const float focus = clampf(env.outdoor_focus, 0.f, 1.f);
		v3f local = frame.toLocal(env.outdoor_dir);
		local.Y = 0.f; // beds pan in the horizontal plane
		const bool has_dir = local.getLengthSQ() > 1e-4f;
		const float f = has_dir ? focus : 0.f;
		// Unfocused beds sit around the listener (centred); focus swings the
		// centre toward the opening.
		const float az_open = has_dir ? std::atan2(-local.X, local.Z) : 0.f;
		const float az = std::atan2(std::sin(az_open) * f, std::cos(az_open) * f + (1.f - f));
		if (stereo && m_stereo_angles) {
			const float w = lerpf(1.4f, 0.2f, f);
			ALfloat angles[2] = {az + w, az - w};
			alSourcefv(src, AL_STEREO_ANGLES, angles);
		} else if (!stereo) {
			// Relative source one node away; a radius ≥ distance spreads it
			// all around, shrinking it collapses it to a point at the opening.
			alSource3f(src, AL_POSITION, -std::sin(az), 0.f, -std::cos(az));
			if (m_source_radius)
				alSourcef(src, AL_SOURCE_RADIUS, lerpf(2.f, 0.4f, f));
		}
	}
	warn_if_al_error("afx: applySource");
}

void EfxRig::clearSource(ALuint src)
{
	if (!m_ok)
		return;
	alSourcei(src, AL_DIRECT_FILTER, AL_FILTER_NULL);
	alSource3i(src, AL_AUXILIARY_SEND_FILTER, AL_EFFECTSLOT_NULL, 0, AL_FILTER_NULL);
	if (m_echo_slot)
		alSource3i(src, AL_AUXILIARY_SEND_FILTER, AL_EFFECTSLOT_NULL, 1, AL_FILTER_NULL);
	if (m_muffle_slot)
		alSource3i(src, AL_AUXILIARY_SEND_FILTER, AL_EFFECTSLOT_NULL, 2, AL_FILTER_NULL);
	if (m_nb_slot)
		alSource3i(src, AL_AUXILIARY_SEND_FILTER, AL_EFFECTSLOT_NULL, 3, AL_FILTER_NULL);
	if (m_hollow_slot)
		alSource3i(src, AL_AUXILIARY_SEND_FILTER, AL_EFFECTSLOT_NULL, 4, AL_FILTER_NULL);
}

#endif

} // namespace sound

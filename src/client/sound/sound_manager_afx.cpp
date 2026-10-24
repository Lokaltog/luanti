// Luanti "afx" fork: environmental audio
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// The sound-thread side of afx: analyse snapshots, glide toward the result,
// occlude positional sources and route every source through EFX.
//
// Rates: snapshots arrive at afx_probe_interval (~2.5 Hz). The smoother and
// per-source occlusion glide every step (~60 Hz); EFX parameters are pushed
// at 30 Hz; occlusion rays are recast at ~7 Hz.

#include "sound_manager.h"
#include "porting.h"
#include "util/tracy_wrapper.h"

#include <chrono>
#if __has_include(<AL/alext.h>)
	#include <AL/alext.h>
#elif __has_include(<alext.h>)
	#include <alext.h>
#endif

namespace sound {

static constexpr f32 AFX_APPLY_INTERVAL = 1.0f / 30.0f;
static constexpr f32 AFX_OCCLUSION_INTERVAL = 0.15f;
static constexpr f32 AFX_OCCLUSION_TAU = 0.25f;
static constexpr f32 AFX_REPORT_INTERVAL = 0.25f;

void OpenALSoundManager::setAcoustics(std::shared_ptr<const afx::VoxelSnapshot> snap,
		const afx::Params &params)
{
	const bool rules_changed = params.dry_sounds != m_afx_params.dry_sounds ||
			params.outdoor_sounds != m_afx_params.outdoor_sounds ||
			params.indoor_sounds != m_afx_params.indoor_sounds;
	if (params.enabled && (params.doppler != m_afx_params.doppler || !m_afx_doppler_set)) {
		alDopplerFactor(std::max(0.0f, params.doppler));
		m_afx_doppler_set = true;
	}
	if (params.hrtf != (m_hrtf_set == 1) && (params.hrtf || m_hrtf_set == 1))
		applyHrtf(params.hrtf);
	m_afx_params = params;
	if (rules_changed)
		m_bus_rules = afx::BusRules::fromParams(params);

	if (!m_efx || !m_efx->available())
		return;

	if (!params.enabled) {
		if (m_afx_active)
			deactivateAcoustics();
		return;
	}

	m_afx_snap = std::move(snap);
	m_afx_field_valid = false;
	if (!m_afx_snap)
		return;

	auto t0 = std::chrono::steady_clock::now();
	afx::Acoustics target = afx::analyze(*m_afx_snap, m_listener_pos_node, params);
	auto t1 = std::chrono::steady_clock::now();
	m_afx_probe_ms = std::chrono::duration<f32, std::milli>(t1 - t0).count();
	if (params.debug)
		infostream << "afx target: hollow " << target.hollow << " coupling " << target.coupling
				<< " nb_decay " << target.neighbor_decay << std::endl;

	// A door opens out of a sealed room: the air moves (a faint low
	// whoomp), and the outdoors swells in (the smoother does that)
	m_whoomp_cooldown = std::max(0.0f, m_whoomp_cooldown - params.probe_interval);
	// (not a teleport, a respawn, a new world: only when you're still)
	const bool still = (m_listener_pos_node - m_whoomp_last_ear).getLength() < 1.5f;
	m_whoomp_last_ear = m_listener_pos_node;
	if (!m_afx_smoother.valid() || !m_afx_active)
		m_whoomp_cooldown = 3.0f;
	if (still && m_afx_smoother.valid() && m_last_target_outdoor < 0.02f && target.outdoor_gain > 0.05f &&
			target.enclosure > 0.6f && !target.submerged && m_whoomp_cooldown <= 0.0f) {
		m_whoomp_cooldown = 4.0f;
		std::string w = getLoadedSoundNameFromGroup("afx_dry_whoomp");
		if (!w.empty()) {
			auto snd = createPlayingSound(w, false, 0.35f * std::min(1.0f, target.outdoor_gain * 4.0f),
					1.0f, 0.0f, std::nullopt, afx::Bus::Dry);
			if (snd)
				m_internal_sounds.push_back(std::move(snd));
		}
	}
	m_last_target_outdoor = target.outdoor_gain;

	const bool first = !m_afx_smoother.valid();
	m_afx_smoother.setTarget(target);

	if (first || !m_afx_active) {
		// Start from where we are rather than gliding in from nothing, and
		// pick up sounds that started before the first snapshot.
		m_afx_smoother.reset(target);
		m_afx_active = true;
		for (auto &[id, snd] : m_sounds_playing) {
			if (snd->getBus() == afx::Bus::World)
				snd->setOcclusionNow(computeOcclusion(*snd));
			applyMix(*snd);
		}
		m_efx->applyEnvironment(m_afx_smoother.current(), m_listener_frame);
	}
}

void OpenALSoundManager::applyHrtf(bool on)
{
	m_hrtf_set = on ? 1 : 0;
#if defined(ALC_HRTF_SOFT)
	if (!alcIsExtensionPresent(m_device, "ALC_SOFT_HRTF")) {
		warningstream << "afx: no HRTF support in this OpenAL" << std::endl;
		return;
	}
	auto reset = reinterpret_cast<LPALCRESETDEVICESOFT>(
			alcGetProcAddress(m_device, "alcResetDeviceSOFT"));
	if (!reset)
		return;
	// Keep the aux sends the context was made with
	// Off hands the choice back to OpenAL Soft (its headphone detection),
	// rather than forcing HRTF off
	const ALCint attrs[] = {ALC_MAX_AUXILIARY_SENDS, 6, ALC_HRTF_SOFT,
			on ? ALC_TRUE : ALC_DONT_CARE_SOFT, 0};
	if (!reset(m_device, attrs)) {
		warningstream << "afx: resetting the device for HRTF failed" << std::endl;
		return;
	}
	ALCint status = 0;
	alcGetIntegerv(m_device, ALC_HRTF_STATUS_SOFT, 1, &status);
	infostream << "afx: HRTF " << (on ? "requested" : "off") << ", status " << status
			<< (status == ALC_HRTF_ENABLED_SOFT ? " (enabled)" :
				status == ALC_HRTF_UNSUPPORTED_FORMAT_SOFT ? " (needs stereo output)" : "")
			<< std::endl;
#endif
}

void OpenALSoundManager::deactivateAcoustics()
{
	afx::Acoustics neutral;
	neutral.wet = 0.f;
	neutral.echo = 0.f;
	m_efx->applyEnvironment(neutral, m_listener_frame);
	for (auto &[id, snd] : m_sounds_playing) {
		m_efx->resetSource(snd->getSourceId(), snd->isStereo());
		snd->resetAfx();
	}
	alDopplerFactor(1.0f); // OpenAL's default, what the engine had before afx
	m_afx_doppler_set = false;
	m_roomtone.reset();
	m_moan.reset();
	m_moan_level = 0.0f;
	m_remote = m_remote_target = afx::RemoteSpace();
	m_efx->applyRemote(m_remote, m_listener_frame);
	m_afx_active = false;
	m_afx_snap.reset();
	m_afx_smoother = afx::Smoother();
}

afx::Occlusion OpenALSoundManager::computeOcclusion(const PlayingSound &snd)
{
	if (!m_afx_snap || !snd.isPositional())
		return {};
	afx::Occlusion o = afx::occlusion(*m_afx_snap, m_listener_pos_node, snd.getPosNode());
	if (o.amount > 0.2f) {
		// Hidden: find the way around. The field is built once per recast,
		// on demand, from where the ear is now.
		if (!m_afx_field_valid) {
			afx::buildAirField(*m_afx_snap, m_listener_pos_node, 20, m_afx_field);
			m_afx_field_valid = true;
		}
		afx::routeAround(*m_afx_snap, m_afx_field, m_listener_pos_node, snd.getPosNode(), o);
	}
	// Far off, over the land: a ridge between you and it bends the sound
	// over its top, duller the higher it rises above the line
	const v3f src = snd.getPosNode();
	if (m_afx_snap->heights && o.path < 0.5f &&
			(src - m_listener_pos_node).getLengthSQ() > 24.0f * 24.0f) {
		const f32 e = afx::terrainExcess(*m_afx_snap->heights, m_listener_pos_node, src);
		if (e > 0.5f) {
			const f32 g = std::exp(-e / 10.0f), hf = std::exp(-e / 4.0f);
			// blocked through the rock: over the top is the way it comes; in
			// the clear nearby but a ridge further on: the ridge decides
			const bool blocked = o.amount > 0.5f;
			if ((blocked && g > o.wall) || (!blocked && 1.0f - g > o.amount)) {
				o.amount = 1.0f;
				o.wall = 0.0f;
				o.path = 1.0f;
				o.path_gain = g;
				o.path_hf = hf;
				o.virt_pos = src;
			}
		}
	}
	return o;
}

static bool anyGlob(const std::string &name, std::initializer_list<const char *> globs)
{
	for (const char *g : globs)
		if (afx::globMatch(g, name))
			return true;
	return false;
}

void OpenALSoundManager::noteImpact(const std::string &group, f32 gain,
		const std::optional<std::pair<v3f, v3f>> &pos_vel)
{
	if (!m_afx_active || m_playing_echo)
		return;
	// Thunder rolls back off the mountains around: a few late, dull taps
	// from the high ground, each as late as its detour
	if (pos_vel && anyGlob(group, {"*thunder*"}) && m_afx_snap && m_afx_snap->heights &&
			m_afx_params.echo > 0.0f) {
		const v3f src = swap_handedness(pos_vel->first);
		const v3f ear = m_listener_pos_node;
		for (const v3f &m : afx::echoPeaks(*m_afx_snap->heights, ear, src, 3)) {
			const f32 detour = (src - m).getLength() + (m - ear).getLength();
			SoundSpec spec(group, std::min(1.0f, gain) * 0.32f * m_afx_params.echo, false);
			spec.pitch = 0.94f;
			DelayedSound d{0, spec, m, v3f(), detour / 343.0f, std::nullopt};
			d.echo = true;
			m_delayed.push_back(std::move(d));
		}
	}
	if (group.rfind("afx_", 0) == 0)
		return;
	const bool blast = anyGlob(group, {"*explo*", "*tnt*", "*blast*"});
	if (!blast && !anyGlob(group, {"*hurt*", "*damage*", "*death*", "*thunder*"}))
		return;
	f32 dist = 0.0f;
	if (pos_vel)
		dist = (swap_handedness(pos_vel->first) - m_listener_pos_node).getLength();
	const f32 loud = std::min(1.0f, gain) * std::clamp(6.0f / std::max(1.0f, dist), 0.0f, 1.0f);
	m_duck = std::max(m_duck, std::min(1.0f, loud * 1.2f));
	if (blast && dist < 12.0f) {
		const f32 d = std::clamp(1.0f - dist / 12.0f, 0.0f, 1.0f) * std::min(1.0f, gain * 1.5f);
		if (d > m_deafen) {
			m_deafen = d;
			m_deafen_hold = 0.5f;
		}
		// the ringing: one at a time
		if (d > 0.35f && m_ring_cooldown <= 0.0f) {
			m_ring_cooldown = 3.0f;
			std::string ring = getLoadedSoundNameFromGroup("afx_dry_ring");
			if (!ring.empty()) {
				auto snd = createPlayingSound(ring, false, 0.6f * d, 1.0f, 0.0f,
						std::nullopt, afx::Bus::Dry);
				if (snd)
					m_internal_sounds.push_back(std::move(snd));
			}
		}
	}
}

void OpenALSoundManager::stepImpacts(f32 dtime)
{
	m_duck *= std::exp(-dtime / 1.0f);
	if (m_deafen_hold > 0.0f)
		m_deafen_hold -= dtime;
	else
		m_deafen *= std::exp(-dtime / 2.5f);
	if (m_deafen < 0.01f)
		m_deafen = 0.0f;
	m_ring_cooldown -= dtime;
	for (size_t i = 0; i < m_internal_sounds.size();) {
		if (m_internal_sounds[i]->isDead()) {
			m_internal_sounds[i] = std::move(m_internal_sounds.back());
			m_internal_sounds.pop_back();
		} else {
			i++;
		}
	}
}

void OpenALSoundManager::applyMix(PlayingSound &snd)
{
	afx::Acoustics env = m_afx_smoother.current();
	env.duck = m_duck;
	env.deafen = m_deafen;
	afx::Mix mix = afx::mixFor(snd.getBus(), env, snd.getOcclusion(), m_afx_params,
			snd.isFootstep());
	if (snd.getBus() == afx::Bus::World) {
		afx::carrySpace(mix, snd.getSpace(), m_remote, env, snd.getOcclusion(), m_afx_params);
		if (snd.isPositional()) {
			// heard from where the sound comes in (around a corner: the opening)
			const afx::Occlusion &o = snd.getOcclusion();
			const v3f from = o.path > 0.5f ? o.virt_pos : snd.getPosNode();
			afx::carryWeather(mix, from - m_listener_pos_node, m_afx_params);
			// OpenAL only distance-attenuates sends into reverbs (and barely);
			// the muffle EQ gets the source's full level at any range. Give it
			// the direct path's 1/d (reference distance 1 node). The walls
			// slot follows the source's own distance law (send auto off).
			const f32 dist = (from - m_listener_pos_node).getLength();
			const f32 att = 1.0f / std::max(1.0f, dist);
			mix.muffle_send *= att;
			if (!m_efx->hasWalls())
				mix.echo_send *= att;
			// the far space and the space next door: a far source feeds them less
			const f32 far = std::min(1.0f, 6.0f / std::max(1.0f, dist));
			mix.remote_send *= far;
			mix.neighbor_send *= far;
			// Gain staging. The engine plays positional sounds at 3x (its
			// reference distance was cut from 3 nodes to 1); with AL_MAX_GAIN
			// at 1 that saturates, and every source from a footstep to a
			// blast sent the same level into the effects. Let AL_GAIN reach 3,
			// keep the direct path where it was (clamped up close as before),
			// and let the sends follow the source's level.
			const f32 g = std::max(1e-4f, snd.getGainCached());
			const f32 near = std::min(1.0f, 1.0f / (3.0f * g));
			mix.direct_gain *= near;
			mix.muffle_send *= near;
			const f32 sf = std::min(0.667f, 0.5f / g);
			mix.reverb_send *= sf;
			mix.echo_send *= sf;
			mix.neighbor_send *= sf;
			mix.remote_send *= sf;
			mix.hollow_send *= sf;
			alSourcef(snd.getSourceId(), AL_MAX_GAIN, 3.0f);
		}
	}
	if (snd.isRain() && m_afx_params.rain_front < 0.999f) {
		// a storm front: the rain bed comes in (or leaves) from one side
		const f32 f = std::clamp(m_afx_params.rain_front, 0.0f, 1.0f);
		const f32 level = f * f * (3.0f - 2.0f * f);
		mix.direct_gain *= level;
		mix.muffle_send *= level;
		mix.reverb_send *= level;
		const v3f fd = m_afx_params.rain_front_dir;
		if (fd.getLengthSQ() > 0.5f) {
			// out in it, it's over there; indoors the opening still decides
			const f32 out = std::clamp(1.0f - env.enclosure, 0.0f, 1.0f);
			const f32 w = out * (1.0f - f);
			// under open sky there's no opening to blend with: just the front
			v3f dir = out > 0.5f ? fd : env.outdoor_dir * (1.0f - w) + fd * w;
			if (dir.getLengthSQ() > 1e-4f)
				env.outdoor_dir = dir.normalize();
			env.outdoor_focus = std::max(env.outdoor_focus, 0.85f * w);
		}
	}
	if (snd.getBus() == afx::Bus::Dry)
		m_efx->clearSource(snd.getSourceId());
	else
		m_efx->applySource(snd.getSourceId(), mix, snd.isPositional(), snd.isStereo(),
				env, m_listener_frame);
}

void OpenALSoundManager::prepareSound(PlayingSound &snd)
{
	if (!m_afx_active)
		return;
	if (snd.getBus() == afx::Bus::World) {
		snd.setOcclusionNow(computeOcclusion(snd));
		if (m_afx_snap && m_afx_params.source_space > 0.0f && snd.isPositional() &&
				(snd.getPosNode() - m_listener_pos_node).getLengthSQ() > 25.0f)
			snd.setSpace(afx::analyzeSpace(*m_afx_snap, snd.getPosNode(), m_afx_params));
	}
	{
		if (m_afx_params.debug) {
			const afx::Occlusion &o = snd.getOcclusion();
			afx::Acoustics env = m_afx_smoother.current();
			afx::Mix mix = afx::mixFor(snd.getBus(), env, o, m_afx_params, snd.isFootstep());
			infostream << "afx src " << snd.getLoggingName() << " bus " << (int)snd.getBus() << " d "
					<< (snd.getPosNode() - m_listener_pos_node).getLength()
					<< " occ " << o.amount << " hf " << o.hf << " wall " << o.wall
					<< " path " << o.path << " pg " << o.path_gain
					<< " -> direct " << mix.direct_gain << " hf " << mix.direct_hf
					<< " muffle " << mix.muffle_send << " rev " << mix.reverb_send << std::endl;
		}
	}
	applyMix(snd);
}

void OpenALSoundManager::stepSourceSpaces(f32 dtime, bool recast)
{
	const afx::Acoustics &env = m_afx_smoother.current();
	if (recast) {
		// Look again at the spaces of sources that moved, a few per recast
		int budget = 4;
		afx::RemoteSpace best;
		f32 best_w = 0.0f;
		for (auto &[id, snd] : m_sounds_playing) {
			if (snd->getBus() != afx::Bus::World || !snd->isPositional())
				continue;
			const v3f d = snd->getPosNode() - m_listener_pos_node;
			const bool far = d.getLengthSQ() > 25.0f;
			// Close by, it's in our room: nothing of its own to carry
			if (!far || m_afx_params.source_space <= 0.0f) {
				if (snd->getSpace().valid)
					snd->setSpace(afx::SourceSpace());
				continue;
			}
			if (snd->spaceStale(AFX_OCCLUSION_INTERVAL) && budget > 0 && m_afx_snap) {
				budget--;
				snd->setSpace(afx::analyzeSpace(*m_afx_snap, snd->getPosNode(), m_afx_params));
			}
			// how much of it reaches us: the inverse-distance law, roughly
			const afx::Occlusion &o = snd->getOcclusion();
			const f32 reach = std::min(1.0f, 8.0f / std::max(1.0f, d.getLength())) *
					(1.0f - 0.6f * o.amount * (1.0f - o.path)) * std::min(1.0f, snd->getGain() * 2.0f);
			afx::considerRemote(snd->getSpace(), reach, v3f(d).normalize(), env, best, best_w);
		}
		if (m_afx_params.source_space <= 0.0f)
			best = afx::RemoteSpace();
		// Keep playing the last space while its tail rings out
		if (best.level <= 0.0f) {
			m_remote_target.level = 0.0f;
		} else {
			m_remote_target = best;
		}
	}
	// Glide: the level fades, the space itself switches only while quiet
	const f32 k = 1.0f - std::exp(-dtime / (0.4f * m_afx_params.smoothing));
	m_remote.level += (m_remote_target.level - m_remote.level) * k;
	if (m_remote.level < 0.05f || m_remote_target.level <= 0.0f ||
			std::fabs(m_remote_target.decay - m_remote.decay) < 0.5f * m_remote.decay) {
		m_remote.decay += (m_remote_target.decay - m_remote.decay) * k;
		m_remote.hf_ratio += (m_remote_target.hf_ratio - m_remote.hf_ratio) * k;
	} else {
		// a different space: duck the old one first
		m_remote.level *= 1.0f - k;
	}
	if (m_remote_target.dir.getLengthSQ() > 0.0f) {
		v3f dir = m_remote.dir + (m_remote_target.dir - m_remote.dir) * k;
		if (dir.getLengthSQ() > 1e-6f)
			m_remote.dir = dir.normalize();
	}
}

void OpenALSoundManager::stepCaveMoan(f32 dtime)
{
	const afx::Acoustics &env = m_afx_smoother.current();
	// In a cave with its mouth in earshot, when the wind gusts across it.
	// The bigger the cave and the smaller the mouth, the lower it moans.
	const f32 g = env.outdoor_gain;
	const f32 cave = std::clamp((env.enclosure - 0.6f) / 0.3f, 0.0f, 1.0f) *
			std::clamp((env.natural - 0.4f) / 0.3f, 0.0f, 1.0f);
	const f32 mouth = std::clamp(g * 8.0f, 0.0f, 1.0f) * std::clamp((0.6f - g) / 0.4f, 0.0f, 1.0f);
	const f32 gust = std::clamp((m_afx_params.wind - 0.9f) / 0.6f, 0.0f, 1.0f);
	const f32 target = 0.05f * cave * mouth * gust * (1.0f - env.submerged) * (1.0f - m_deafen);
	const f32 k = 1.0f - std::exp(-dtime / 1.5f); // it swells and fades with the gust
	m_moan_level += (target - m_moan_level) * k;
	if (!m_moan) {
		m_moan_retry -= dtime;
		if (m_moan_level < 0.003f || m_moan_retry > 0.0f || m_is_paused)
			return;
		m_moan_retry = 10.0f;
		const std::string name = getLoadedSoundNameFromGroup("afx_cave_moan");
		if (name.empty())
			return;
		m_moan = createPlayingSound(name, true, 0.0f, 1.0f, 0.0f,
				std::make_pair(swap_handedness(m_listener_pos_node), v3f()), afx::Bus::World);
		if (!m_moan)
			return;
		if (m_moan->isStreaming())
			m_sounds_streaming_next_bigstep.push_back(m_moan);
	}
	if (m_moan_level < 0.002f && target < 0.002f) {
		m_moan.reset();
		return;
	}
	// heard from the mouth, a few nodes off toward the way out
	const v3f at = m_listener_pos_node + env.outdoor_dir * std::clamp(env.outdoor_path, 2.0f, 10.0f);
	m_moan->updatePosVel(swap_handedness(at), v3f());
	m_moan->setGain(m_moan_level);
	const f32 pitch = std::clamp(1.4f - 0.05f * env.mean_free_path - 0.5f * g, 0.55f, 1.5f);
	m_moan->setPitch(pitch);
	afx::Mix mix;
	mix.reverb_send = 1.0f;
	mix.reverb_hf = 0.6f;
	m_efx->applySource(m_moan->getSourceId(), mix, true, false, env, m_listener_frame);
}

void OpenALSoundManager::stepRoomTone(f32 dtime)
{
	afx::Acoustics env = m_afx_smoother.current();
	env.duck = m_duck;
	env.deafen = m_deafen;
	const afx::RoomTone t = afx::roomTone(env, m_afx_params);
	if (!m_roomtone) {
		m_roomtone_retry -= dtime;
		if (t.gain < 0.002f || m_roomtone_retry > 0.0f || m_is_paused)
			return;
		m_roomtone_retry = 10.0f; // missing sound: don't look every frame
		const std::string name = getLoadedSoundNameFromGroup("afx_roomtone");
		if (name.empty())
			return;
		// Bus::Indoor so the start-up mix is sane; the real mix is set here
		m_roomtone = createPlayingSound(name, true, 0.0f, t.pitch, 0.0f, std::nullopt,
				afx::Bus::Indoor);
		if (!m_roomtone)
			return;
		if (m_roomtone->isStreaming())
			m_sounds_streaming_next_bigstep.push_back(m_roomtone);
	}
	// silent for a while (out under the sky): let it go
	if (t.gain < 0.002f) {
		m_roomtone_quiet += dtime;
		if (m_roomtone_quiet > 5.0f) {
			m_roomtone.reset();
			m_roomtone_quiet = 0.0f;
			m_roomtone_retry = 0.0f;
			return;
		}
	} else {
		m_roomtone_quiet = 0.0f;
	}
	m_roomtone->setGain(t.gain);
	if (std::fabs(t.pitch - m_roomtone_pitch) > 0.01f) {
		m_roomtone->setPitch(t.pitch);
		m_roomtone_pitch = t.pitch;
	}
	m_efx->applySource(m_roomtone->getSourceId(), t.mix, false, false, env, m_listener_frame);
}

void OpenALSoundManager::stepAcoustics(f32 dtime)
{
	if (!m_afx_active)
		return;
	ZoneScoped;

	m_afx_smoother.step(dtime, m_afx_params.smoothing);
	stepImpacts(dtime);

	m_afx_occlusion_timer -= dtime;
	const bool recast = m_afx_occlusion_timer <= 0.0f;
	if (recast) {
		m_afx_occlusion_timer = AFX_OCCLUSION_INTERVAL;
		m_afx_field_valid = false;
	}

	stepSourceSpaces(dtime, recast);

	u32 occluded = 0, routed = 0;
	for (auto &[id, snd] : m_sounds_playing) {
		if (snd->getBus() != afx::Bus::World)
			continue;
		if (recast)
			snd->setOcclusionTarget(computeOcclusion(*snd));
		snd->stepOcclusion(dtime, AFX_OCCLUSION_TAU * m_afx_params.smoothing);
		if (snd->getOcclusion().amount > 0.1f)
			occluded++;
		if (snd->getOcclusion().path > 0.5f)
			routed++;
	}

	m_afx_apply_timer -= dtime;
	if (m_afx_apply_timer <= 0.0f) {
		m_afx_apply_timer = AFX_APPLY_INTERVAL;
		afx::Acoustics env = m_afx_smoother.current();
		env.deafen = m_deafen;
		// one batch: every source's sends change together, in one update
		static const bool defer = alIsExtensionPresent("AL_SOFT_deferred_updates");
		static auto defer_fn = defer ?
				reinterpret_cast<void (AL_APIENTRY *)()>(alGetProcAddress("alDeferUpdatesSOFT")) : nullptr;
		static auto process_fn = defer ?
				reinterpret_cast<void (AL_APIENTRY *)()>(alGetProcAddress("alProcessUpdatesSOFT")) : nullptr;
		if (defer_fn)
			defer_fn();
		m_efx->applyEnvironment(env, m_listener_frame);
		m_efx->applyRemote(m_remote, m_listener_frame);
		for (auto &[id, snd] : m_sounds_playing)
			applyMix(*snd);
		if (process_fn)
			process_fn();
		stepRoomTone(AFX_APPLY_INTERVAL);
		stepCaveMoan(AFX_APPLY_INTERVAL);
	}

	if (m_afx_params.debug) {
		static f32 dbg_timer = 0.0f;
		dbg_timer -= dtime;
		if (dbg_timer <= 0.0f) {
			dbg_timer = 2.0f;
			afx::Acoustics env = m_afx_smoother.current();
			for (auto &[id, snd] : m_sounds_playing) {
				const f32 g = snd->getGain();
				if (g < 0.02f)
					continue;
				afx::Mix mix = afx::mixFor(snd->getBus(), env, snd->getOcclusion(), m_afx_params,
						snd->isFootstep());
				if (snd->getBus() == afx::Bus::World) {
					afx::carrySpace(mix, snd->getSpace(), m_remote, env, snd->getOcclusion(),
							m_afx_params);
					if (snd->isPositional())
						afx::carryWeather(mix, snd->getPosNode() - m_listener_pos_node, m_afx_params);
				}
				infostream << "afx mix " << snd->getLoggingName() << " " << afx::busName(snd->getBus())
						<< " gain " << g << " direct " << mix.direct_gain << " hf " << mix.direct_hf
						<< " muffle " << mix.muffle_send << " rev " << mix.reverb_send
						<< " remote " << mix.remote_send << " air " << mix.air_absorption << std::endl;
			}
			infostream << "afx weather wind " << m_afx_params.wind << " dir " << m_afx_params.wind_dir.X
					<< "," << m_afx_params.wind_dir.Z << " rain " << m_afx_params.rain
					<< " night " << m_afx_params.night << std::endl;
			infostream << "afx remote level " << m_remote.level << " decay " << m_remote.decay
					<< " | roomtone " << (m_roomtone ? m_roomtone->getGain() : 0.0f) << std::endl;
		}
	}

	m_afx_report_timer -= dtime;
	if (m_afx_report_timer <= 0.0f) {
		m_afx_report_timer = AFX_REPORT_INTERVAL;
		afx::Report r;
		r.valid = true;
		r.now = m_afx_smoother.current();
		r.probe_ms = m_afx_probe_ms;
		r.sources = (u32)m_sounds_playing.size();
		r.occluded = occluded;
		r.routed = routed;
		r.efx = true;
		send(sound_manager_messages_to_proxy::ReportAcoustics{r});
	}
}

} // namespace sound

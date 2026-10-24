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
	if (params.doppler != m_afx_params.doppler || !m_afx_doppler_set) {
		alDopplerFactor(std::max(0.0f, params.doppler));
		m_afx_doppler_set = true;
	}
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

void OpenALSoundManager::deactivateAcoustics()
{
	afx::Acoustics neutral;
	neutral.wet = 0.f;
	neutral.echo = 0.f;
	m_efx->applyEnvironment(neutral, m_listener_frame);
	for (auto &[id, snd] : m_sounds_playing)
		m_efx->clearSource(snd->getSourceId());
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
	return o;
}

void OpenALSoundManager::applyMix(PlayingSound &snd)
{
	const afx::Acoustics &env = m_afx_smoother.current();
	afx::Mix mix = afx::mixFor(snd.getBus(), env, snd.getOcclusion(), m_afx_params,
			snd.isFootstep());
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
	if (snd.getBus() == afx::Bus::World)
		snd.setOcclusionNow(computeOcclusion(snd));
	applyMix(snd);
}

void OpenALSoundManager::stepAcoustics(f32 dtime)
{
	if (!m_afx_active)
		return;
	ZoneScoped;

	m_afx_smoother.step(dtime, m_afx_params.smoothing);

	m_afx_occlusion_timer -= dtime;
	const bool recast = m_afx_occlusion_timer <= 0.0f;
	if (recast) {
		m_afx_occlusion_timer = AFX_OCCLUSION_INTERVAL;
		m_afx_field_valid = false;
	}

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
		m_efx->applyEnvironment(m_afx_smoother.current(), m_listener_frame);
		for (auto &[id, snd] : m_sounds_playing)
			applyMix(*snd);
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

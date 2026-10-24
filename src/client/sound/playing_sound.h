// Luanti
// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (C) 2022 DS
// Copyright (C) 2013 celeron55, Perttu Ahola <celeron55@gmail.com>
// Copyright (C) 2011 Sebastian 'Bahamada' Rühl
// Copyright (C) 2011 Cyriaque 'Cisoun' Skrapits <cysoun@gmail.com>
// Copyright (C) 2011 Giuseppe Bilotta <giuseppe.bilotta@gmail.com>

#pragma once

#include "sound_data.h"
#include "client/afx/afx_mix.h"
namespace sound { struct ALExtensions; }

namespace sound {

/**
 * A sound that is currently played.
 * Can be streaming.
 * Can be fading.
 */
class PlayingSound final
{
	struct FadeState {
		f32 step;
		f32 target_gain;
	};

	ALuint m_source_id;
	std::shared_ptr<ISoundDataOpen> m_data;
	ALuint m_next_sample_pos = 0;
	bool m_looping;
	bool m_is_positional;
	bool m_stopped_means_dead = true;
	std::optional<FadeState> m_fade_state = std::nullopt;
	u64 m_creation_time_s; // timestamp from porting::getTimeS()

	// afx
	afx::Bus m_bus;
	bool m_footstep = false;
	v3f m_pos_node;            // node space, for occlusion
	afx::Occlusion m_occ_target, m_occ;

public:
	PlayingSound(ALuint source_id, std::shared_ptr<ISoundDataOpen> data, bool loop,
			f32 volume, f32 pitch, f32 start_time,
			const std::optional<std::pair<v3f, v3f>> &pos_vel_opt,
			const ALExtensions &exts [[maybe_unused]],
			afx::Bus bus = afx::Bus::Dry);

	~PlayingSound() noexcept
	{
		alDeleteSources(1, &m_source_id);
	}

	DISABLE_CLASS_COPY(PlayingSound)

	// return false means streaming finished
	bool stepStream(bool playback_speed_changed = false);

	// returns true if it wasn't fading already
	bool fade(f32 step, f32 target_gain) noexcept;

	// returns true if more fade is needed later
	bool doFade(f32 dtime) noexcept;

	void updatePosVel(const v3f &pos, const v3f &vel) noexcept;

	void setGain(f32 gain) noexcept;

	f32 getGain() noexcept;

	void setPitch(f32 pitch);

	bool isStreaming() const noexcept { return m_data->isStreaming(); }

	void play() noexcept { alSourcePlay(m_source_id); }

	// returns one of AL_INITIAL, AL_PLAYING, AL_PAUSED, AL_STOPPED
	ALint getState() noexcept
	{
		ALint state;
		alGetSourcei(m_source_id, AL_SOURCE_STATE, &state);
		return state;
	}

	bool isDead() noexcept
	{
		// streaming sounds can (but should not) stop because the queue runs empty
		return m_stopped_means_dead && getState() == AL_STOPPED;
	}

	void pause() noexcept
	{
		// this is a NOP if state != AL_PLAYING
		alSourcePause(m_source_id);
	}

	void resume() noexcept
	{
		if (getState() == AL_PAUSED)
			play();
	}

	std::string getLoggingName() const;

	u64 getCreationTimeS() const { return m_creation_time_s; }

	/* afx */

	ALuint getSourceId() const { return m_source_id; }
	afx::Bus getBus() const { return m_bus; }
	bool isFootstep() const { return m_footstep; }
	void setFootstep(bool f) { m_footstep = f; }
	bool isPositional() const { return m_is_positional; }
	bool isStereo() const { return m_data && m_data->m_decode_info.is_stereo; }
	v3f getPosNode() const { return m_pos_node; }
	void setOcclusionTarget(const afx::Occlusion &o) { m_occ_target = o; }
	/// For new sounds: no glide, or the first moments would play unoccluded.
	void setOcclusionNow(const afx::Occlusion &o) { m_occ_target = m_occ = o; }
	const afx::Occlusion &getOcclusion() const { return m_occ; }
	/// Glide the occlusion toward its target (called every sound-thread step).
	void stepOcclusion(f32 dtime, f32 tau);
};

} // namespace sound

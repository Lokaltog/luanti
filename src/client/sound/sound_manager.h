// Luanti
// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (C) 2022 DS
// Copyright (C) 2013 celeron55, Perttu Ahola <celeron55@gmail.com>
// Copyright (C) 2011 Sebastian 'Bahamada' Rühl
// Copyright (C) 2011 Cyriaque 'Cisoun' Skrapits <cysoun@gmail.com>
// Copyright (C) 2011 Giuseppe Bilotta <giuseppe.bilotta@gmail.com>

#pragma once

#include "playing_sound.h"
#include "al_extensions.h"
#include "efx.h"
#include "client/afx/afx_smoother.h"
#include "sound_constants.h"
#include "sound_manager_messages.h"
#include "sound_spec.h"
#include "threading/thread.h"
#include "util/container.h" // MutexedQueue

namespace sound {

class SoundManagerSingleton;

/*
 * The SoundManager thread
 *
 * It's not an ISoundManager. It doesn't allocate ids, and doesn't accept id 0.
 * All sound loading and interaction with OpenAL happens in this thread, and in
 * SoundManagerSingleton.
 * Access from other threads happens via ProxySoundManager.
 *
 * See sound_constants.h for more details.
 */

class OpenALSoundManager final : public Thread
{
private:
	std::unique_ptr<SoundFallbackPathProvider> m_fallback_path_provider;

	ALCdevice *const m_device;
	ALCcontext *const m_context;

	const ALExtensions m_exts;

	// time in seconds until which removeDeadSounds will be called again
	f32 m_time_until_dead_removal = REMOVE_DEAD_SOUNDS_INTERVAL;

	// loaded sounds
	std::unordered_map<std::string, std::unique_ptr<ISoundDataUnopen>> m_sound_datas_unopen;
	std::unordered_map<std::string, std::shared_ptr<ISoundDataOpen>> m_sound_datas_open;
	// sound groups
	std::unordered_map<std::string, std::vector<std::string>> m_sound_groups;

	// currently playing sounds
	std::unordered_map<sound_handle_t, std::shared_ptr<PlayingSound>> m_sounds_playing;

	// streamed sounds
	std::vector<std::weak_ptr<PlayingSound>> m_sounds_streaming_current_bigstep;
	std::vector<std::weak_ptr<PlayingSound>> m_sounds_streaming_next_bigstep;
	// time left until current bigstep finishes
	f32 m_stream_timer = STREAM_BIGSTEP_TIME;

	std::vector<std::weak_ptr<PlayingSound>> m_sounds_fading;

	// if true, all sounds will be directly paused after creation
	bool m_is_paused = false;

	// afx: environmental audio
	std::unique_ptr<EfxRig> m_efx;
	afx::Params m_afx_params;
	afx::BusRules m_bus_rules;
	std::shared_ptr<const afx::VoxelSnapshot> m_afx_snap;
	afx::Smoother m_afx_smoother;
	ListenerFrame m_listener_frame;
	v3f m_listener_pos_node;
	bool m_afx_active = false; // sources are routed through EFX
	bool m_afx_doppler_set = false;
	afx::AirField m_afx_field; // routes around corners, rebuilt per recast
	bool m_afx_field_valid = false;
	f32 m_duck = 0.0f, m_deafen = 0.0f, m_deafen_hold = 0.0f, m_ring_cooldown = 0.0f;
	std::vector<std::shared_ptr<PlayingSound>> m_internal_sounds; // the ear ring
	void noteImpact(const std::string &group, f32 gain,
			const std::optional<std::pair<v3f, v3f>> &pos_vel);
	void stepImpacts(f32 dtime);
	// afx: last sample picked per group, so it isn't picked twice in a row
	std::unordered_map<std::string, std::string> m_last_pick;
	f32 m_afx_apply_timer = 0.0f;
	f32 m_afx_occlusion_timer = 0.0f;
	f32 m_afx_report_timer = 0.0f;
	f32 m_afx_probe_ms = 0.0f;

	// afx: positional sounds waiting for their sound to travel to the ear
	struct DelayedSound {
		sound_handle_t id;
		SoundSpec spec;
		v3f pos, vel;   // node space
		f32 due;        // seconds left
		std::optional<std::pair<f32, f32>> fade; // step, target
	};
	std::vector<DelayedSound> m_delayed;
	bool takeDelayed(sound_handle_t id, DelayedSound **out);
	void stepDelayed(f32 dtime);

	// stuff for only-once / rate-limited warnings
	std::unordered_set<std::string> m_warned_positional_stereo_sounds;
	u64 m_next_lingering_sounds_print; // timestamp from porting::getTimeS()

public:
	// used for communication with ProxySoundManager
	MutexedQueue<SoundManagerMsgToMgr> m_queue_to_mgr;
	MutexedQueue<SoundManagerMsgToProxy> m_queue_to_proxy;

private:
	void stepStreams(f32 dtime);
	void doFades(f32 dtime);

	/**
	 * Gives the open sound for a loaded sound.
	 * Opens the sound if currently unopened.
	 *
	 * @param sound_name Name of the sound.
	 * @return The open sound.
	 */
	std::shared_ptr<ISoundDataOpen> openSingleSound(const std::string &sound_name);

	/**
	 * Gets a random sound name from a group.
	 *
	 * @param group_name The name of the sound group.
	 * @return The name of a sound in the group, or "" on failure. Getting the
	 *         sound with `openSingleSound` directly afterwards will not fail.
	 */
	std::string getLoadedSoundNameFromGroup(const std::string &group_name);

	/**
	 * Same as `getLoadedSoundNameFromGroup`, but if sound does not exist, try to
	 * load from local files.
	 */
	std::string getOrLoadLoadedSoundNameFromGroup(const std::string &group_name);

	std::shared_ptr<PlayingSound> createPlayingSound(const std::string &sound_name,
			bool loop, f32 volume, f32 pitch, f32 start_time,
			const std::optional<std::pair<v3f, v3f>> &pos_vel_opt,
			afx::Bus bus = afx::Bus::Dry, bool footstep = false);

	void playSoundGeneric(sound_handle_t id, const std::string &group_name, bool loop,
			f32 volume, f32 fade, f32 pitch, bool use_local_fallback, f32 start_time,
			const std::optional<std::pair<v3f, v3f>> &pos_vel_opt);

	/* afx */

	void setAcoustics(std::shared_ptr<const afx::VoxelSnapshot> snap, const afx::Params &params);
	void stepAcoustics(f32 dtime);
	afx::Occlusion computeOcclusion(const PlayingSound &snd);
	void applyMix(PlayingSound &snd);
	/// Route a sound before it starts, so its first samples are already right.
	void prepareSound(PlayingSound &snd);
	/// Detach every source from EFX (afx turned off).
	void deactivateAcoustics();

	/**
	 * Deletes sounds that are dead (=finished).
	 *
	 * @return Number of removed sounds.
	 */
	int removeDeadSounds();

public:
	OpenALSoundManager(SoundManagerSingleton *smg,
			std::unique_ptr<SoundFallbackPathProvider> fallback_path_provider);

	~OpenALSoundManager() override;

	DISABLE_CLASS_COPY(OpenALSoundManager)

private:
	/* Similar to ISoundManager */

	void step(f32 dtime);
	void pauseAll();
	void resumeAll();

	void updateListener(const v3f &pos_, const v3f &vel_, const v3f &at_, const v3f &up_);
	void setListenerGain(f32 gain);

	bool loadSoundFile(const std::string &name, const std::string &filepath);
	bool loadSoundData(const std::string &name, std::string &&filedata);
	void loadSoundFileNoCheck(const std::string &name, const std::string &filepath);
	void loadSoundDataNoCheck(const std::string &name, std::string &&filedata);
	void addSoundToGroup(const std::string &sound_name, const std::string &group_name);

	void playSound(sound_handle_t id, const SoundSpec &spec);
	void playSoundAt(sound_handle_t id, const SoundSpec &spec, const v3f &pos_,
			const v3f &vel_);
	void stopSound(sound_handle_t sound);
	void fadeSound(sound_handle_t soundid, f32 step, f32 target_gain);
	void updateSoundPosVel(sound_handle_t sound, const v3f &pos_, const v3f &vel_);

	void printPlayingSounds(bool rate_limit);

protected:
	/* Thread stuff */

	void *run() override;

private:
	void send(SoundManagerMsgToProxy msg)
	{
		m_queue_to_proxy.push_back(std::move(msg));
	}

	void reportRemovedSound(sound_handle_t id)
	{
		send(sound_manager_messages_to_proxy::ReportRemovedSound{id});
	}
};

} // namespace sound

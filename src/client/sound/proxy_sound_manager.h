// Luanti
// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (C) 2023 DS

#pragma once

#include "sound_manager.h"

namespace sound {

/*
 * The public ISoundManager interface
 */

class ProxySoundManager final : public ISoundManager
{
	OpenALSoundManager m_sound_manager;
	// sound names from loadSoundData and loadSoundFile
	std::unordered_set<std::string> m_known_sound_names;
	// afx: last report from the sound thread
	afx::Report m_acoustic_report;
	// afx: groups known to exist (from addSoundToGroup)
	std::unordered_set<std::string> m_known_groups;

	void send(SoundManagerMsgToMgr msg)
	{
		m_sound_manager.m_queue_to_mgr.push_back(std::move(msg));
	}

	enum class MsgResult { Ok, Empty, Stopped};
	MsgResult handleMsg(SoundManagerMsgToProxy &&msg);

public:
	ProxySoundManager(SoundManagerSingleton *smg,
			std::unique_ptr<SoundFallbackPathProvider> fallback_path_provider) :
		m_sound_manager(smg, std::move(fallback_path_provider))
	{
		m_sound_manager.start();
	}

	~ProxySoundManager() override;

	/* Interface */

	void step(f32 dtime) override;
	void pauseAll() override;
	void resumeAll() override;

	void updateListener(const v3f &pos_, const v3f &vel_, const v3f &at_, const v3f &up_) override;
	void setListenerGain(f32 gain) override;

	bool loadSoundFile(const std::string &name, const std::string &filepath) override;
	bool loadSoundData(const std::string &name, std::string &&filedata) override;
	void addSoundToGroup(const std::string &sound_name, const std::string &group_name) override;

	void playSound(sound_handle_t id, const SoundSpec &spec) override;
	void playSoundAt(sound_handle_t id, const SoundSpec &spec, const v3f &pos_,
			const v3f &vel_) override;
	void stopSound(sound_handle_t sound) override;
	void fadeSound(sound_handle_t soundid, f32 step, f32 target_gain) override;
	void updateSoundPosVel(sound_handle_t sound, const v3f &pos_, const v3f &vel_) override;
	void setSoundTone(sound_handle_t sound, f32 hf) override;

	void printPlayingSounds() override;

	void setAcoustics(std::shared_ptr<const afx::VoxelSnapshot> snap,
			const afx::Params &params) override;
	bool getAcousticReport(afx::Report &out) const override;
	bool hasSoundGroup(const std::string &group) const override
	{
		return m_known_groups.count(group) != 0 || soundExistsLocally(group);
	}
};

} // namespace sound

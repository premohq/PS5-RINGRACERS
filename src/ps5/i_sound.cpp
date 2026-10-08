// DR. ROBOTNIK'S RING RACERS
//-----------------------------------------------------------------------------
// Copyright (C) 2025 by Ronald "Eidolon" Kinard
// Copyright (C) 2025 by Kart Krew
//
// This program is free software distributed under the
// terms of the GNU General Public License, version 2.
// See the 'LICENSE' file for more details.
//-----------------------------------------------------------------------------
/// \file  ps5/i_sound.cpp
/// \brief Sound and music - PlayStation 5
///
/// sdl/new_sound.cpp with its device layer replaced and nothing else: the
/// same mixer graph, music player, sound effect channels and voice chat
/// playback, fed to the console's AudioOut instead of an SDL stream. Kept
/// as close to that file as possible so upstream changes to it carry over.

#include <algorithm>
#include <cmath>
#include <memory>

#include <atomic>
#include <deque>
#include <mutex>
#include <thread>
#include <tracy/tracy/Tracy.hpp>

#include "../audio/chunk_load.hpp"
#include "../audio/gain.hpp"
#include "../audio/mixer.hpp"
#include "../audio/music_player.hpp"
#include "../audio/resample.hpp"
#include "../audio/sound_chunk.hpp"
#include "../audio/sound_effect_player.hpp"
#include "../cxxutil.hpp"

#ifdef SRB2_CONFIG_ENABLE_WEBM_MOVIES
#include "../m_avrecorder.hpp"
#endif

#include "../doomdef.h"
#include "../i_sound.h"
#include "../m_misc.h"
#include "../s_sound.h"
#include "../sounds.h"
#include "../w_wad.h"
#include "../z_zone.h"

#include "ps5_sys.h"

using std::make_shared;
using std::make_unique;
using std::shared_ptr;
using std::unique_ptr;
using std::vector;

using srb2::audio::Gain;
using srb2::audio::Mixer;
using srb2::audio::MusicPlayer;
using srb2::audio::Resampler;
using srb2::audio::Sample;
using srb2::audio::SoundChunk;
using srb2::audio::SoundEffectPlayer;
using srb2::audio::Source;
using namespace srb2;

namespace
{
/// What SDL_AudioStream did for voice chat: Opus frames arrive as 48 kHz
/// mono float and the mixer runs at 44.1 kHz stereo. Linear interpolation,
/// which is what SDL3's stream does for a rate change too. Every caller holds
/// the audio lock, as on SDL, so this needs none of its own.
class VoiceResampler final
{
	static constexpr double kStep = 48000.0 / 44100.0;

	std::deque<float> input_;
	double position_ = 0.0;

public:
	void put(std::span<const std::byte> buf)
	{
		const size_t count = buf.size_bytes() / sizeof(float);
		const float* samples = reinterpret_cast<const float*>(buf.data());
		input_.insert(input_.end(), samples, samples + count);
	}

	size_t get(std::span<std::byte> out)
	{
		Sample<2>* frames = reinterpret_cast<Sample<2>*>(out.data());
		const size_t capacity = out.size_bytes() / sizeof(Sample<2>);
		size_t written = 0;

		while (written < capacity && position_ + 1.0 < static_cast<double>(input_.size()))
		{
			const size_t i = static_cast<size_t>(position_);
			const float t = static_cast<float>(position_ - static_cast<double>(i));
			const float v = input_[i] + (input_[i + 1] - input_[i]) * t;
			frames[written++] = Sample<2> {v, v};
			position_ += kStep;
		}

		const size_t consumed = static_cast<size_t>(position_);
		input_.erase(input_.begin(), input_.begin() + std::min(consumed, input_.size()));
		position_ -= static_cast<double>(consumed);

		return written * sizeof(Sample<2>);
	}

	void clear() noexcept
	{
		input_.clear();
		position_ = 0.0;
	}
};

class VoiceStreamPlayer : public Source<2>
{
	VoiceResampler stream_;
	float volume_ = 1.0f;
	float sep_ = 0.0f;
	bool terminal_ = true;

public:
	VoiceStreamPlayer() = default;
	virtual ~VoiceStreamPlayer() = default;

	virtual std::size_t generate(std::span<Sample<2>> buffer) override
	{
		size_t written = stream_.get(std::as_writable_bytes(buffer)) / sizeof(Sample<2>);

		for (size_t i = written; i < buffer.size(); i++)
		{
			buffer[i] = {0.f, 0.f};
		}

		// Apply gain de-popping if the last generation was terminal
		if (terminal_)
		{
			for (size_t i = 0; i < std::min<size_t>(16, written); i++)
			{
				buffer[i].amplitudes[0] *= (float)(i) / 16;
				buffer[i].amplitudes[1] *= (float)(i) / 16;
			}
			terminal_ = false;
		}

		if (written < buffer.size())
		{
			terminal_ = true;
		}

		for (size_t i = 0; i < written; i++)
		{
			float sep_pan = ((sep_ + 1.f) / 2.f) * (3.14159 / 2.f);

			float left_scale = std::cos(sep_pan);
			float right_scale = std::sin(sep_pan);
			buffer[i] = {std::clamp(buffer[i].amplitudes[0] * volume_ * left_scale, -1.f, 1.f), std::clamp(buffer[i].amplitudes[1] * volume_ * right_scale, -1.f, 1.f)};
		}

		return buffer.size();
	};

	VoiceResampler& stream() noexcept { return stream_; }

	void set_properties(float volume, float sep) noexcept
	{
		volume_ = volume;
		sep_ = sep;
	}
};

} // namespace

// extern in i_sound.h
uint8_t sound_started = false;

static unique_ptr<Gain<2>> master_gain;
static shared_ptr<Mixer<2>> master;
static shared_ptr<Mixer<2>> mixer_sound_effects;
static shared_ptr<Mixer<2>> mixer_music;
static shared_ptr<Mixer<2>> mixer_voice;
static shared_ptr<MusicPlayer> music_player;
static shared_ptr<Resampler<2>> resample_music_player;
static shared_ptr<Gain<2>> gain_sound_effects;
static shared_ptr<Gain<2>> gain_music_player;
static shared_ptr<Gain<2>> gain_music_channel;
static shared_ptr<Gain<2>> gain_voice_channel;

static vector<shared_ptr<SoundEffectPlayer>> sound_effect_channels;
static vector<shared_ptr<VoiceStreamPlayer>> player_voice_channels;

#ifdef SRB2_CONFIG_ENABLE_WEBM_MOVIES
static shared_ptr<srb2::media::AVRecorder> av_recorder;
#endif

static void (*music_fade_callback)();

/// SDL's audio stream lock was recursive, and I_UpdateSound relies on it.
static std::recursive_mutex g_audio_mutex;
static std::thread g_audio_thread;
static std::atomic<bool> g_audio_running {false};
static int32_t g_audio_port = -1;

/// The console's main output takes 48 kHz and nothing else; the mixer graph
/// below stays at the 44.1 kHz the PC build mixes at, and this converts.
static constexpr uint32_t kOutputRate = 48000;
static constexpr uint32_t kMixRate = 44100;
/// Frames per sceAudioOutOutput call: 256 is the smallest grain the port
/// accepts, a little over 5 ms.
static constexpr uint32_t kGrain = 256;

static size_t g_sound_chunk_bytes = 0;

static size_t SoundChunkHeapBytes(const srb2::audio::SoundChunk& chunk)
{
	return sizeof(srb2::audio::SoundChunk) + chunk.samples.capacity() * sizeof(srb2::audio::Sample<1>);
}

size_t I_GetSoundMemUsage(void)
{
	return g_sound_chunk_bytes;
}

void* I_GetSfx(sfxinfo_t* sfx)
{
	if (sfx->lumpnum == LUMPERROR)
		sfx->lumpnum = S_GetSfxLumpNum(sfx);
	sfx->length = W_LumpLength(sfx->lumpnum);

	std::byte* lump = static_cast<std::byte*>(W_CacheLumpNum(sfx->lumpnum, PU_SOUND));
	auto _ = srb2::finally([lump]() { Z_Free(lump); });

	std::span<std::byte> data_span(lump, sfx->length);
	std::optional<SoundChunk> chunk = srb2::audio::try_load_chunk(data_span);

	if (!chunk)
		return nullptr;

	SoundChunk* heap_chunk = new SoundChunk {std::move(*chunk)};
	g_sound_chunk_bytes += SoundChunkHeapBytes(*heap_chunk);

	return heap_chunk;
}

void I_FreeSfx(sfxinfo_t* sfx)
{
	if (sfx->data)
	{
		SoundChunk* chunk = static_cast<SoundChunk*>(sfx->data);
		g_sound_chunk_bytes -= SoundChunkHeapBytes(*chunk);
		auto _ = srb2::finally([chunk]() { delete chunk; });

		// Stop any channels playing this chunk
		for (auto& player : sound_effect_channels)
		{
			if (player->is_playing_chunk(chunk))
			{
				player->reset();
			}
		}
	}
	sfx->data = nullptr;
	sfx->lumpnum = LUMPERROR;
}

namespace
{

class SdlAudioLockHandle
{
public:
	SdlAudioLockHandle() { g_audio_mutex.lock(); }
	~SdlAudioLockHandle() { g_audio_mutex.unlock(); }
};

#ifdef TRACY_ENABLE
static const char* kAudio = "Audio";
#endif

/// The mixer's output as a Source, so the engine's own Resampler can turn it
/// into 48 kHz.
class MasterTap final : public Source<2>
{
public:
	virtual std::size_t generate(std::span<Sample<2>> buffer) override
	{
		for (auto& s : buffer)
			s = Sample<2> {0.f, 0.f};
		if (master_gain)
			master_gain->generate(buffer);
		return buffer.size();
	}
};

static shared_ptr<Resampler<2>> output_resampler;

/// What SDL's audio callback did, on a thread of our own: mix a grain under
/// the lock, then hand it to AudioOut, which blocks until the previous one
/// has played and so paces the loop.
static void audio_thread()
{
	static std::array<Sample<2>, kGrain> float_buffer = {};

	while (g_audio_running.load())
	{
		{
			SdlAudioLockHandle _;
			floatdenormalstate_t dtzstate = M_EnterFloatDenormalToZero();
			try
			{
				const size_t got = output_resampler ? output_resampler->generate(std::span {float_buffer.data(), float_buffer.size()}) : 0;
				for (size_t i = got; i < float_buffer.size(); i++)
					float_buffer[i] = Sample<2> {0.f, 0.f};
				for (auto& s : float_buffer)
				{
					s = {
						std::clamp(s.amplitudes[0], -1.f, 1.f),
						std::clamp(s.amplitudes[1], -1.f, 1.f),
					};
				}
			}
			catch (...)
			{
				float_buffer.fill(Sample<2> {0.f, 0.f});
			}
			M_ExitFloatDenormalToZero(dtzstate);
		}

		sceAudioOutOutput(g_audio_port, float_buffer.data());
	}
}

void initialize_sound()
{
	// A failure here may only mean something else in the process got there
	// first; opening the port is the real test.
	const int32_t init_err = sceAudioOutInit();
	if (init_err < 0)
		CONS_Printf("sceAudioOutInit: 0x%08x\n", static_cast<unsigned>(init_err));

	for (int32_t user : {PS5_InitialUser(), static_cast<int32_t>(PS5_USER_ID_SYSTEM)})
	{
		g_audio_port = sceAudioOutOpen(user, PS5_AUDIO_OUT_PORT_TYPE_MAIN, 0, kGrain, kOutputRate,
			PS5_AUDIO_OUT_PARAM_FORMAT_FLOAT_STEREO);
		if (g_audio_port >= 0)
			break;
	}
	if (g_audio_port < 0)
	{
		CONS_Alert(CONS_ERROR, "sceAudioOutOpen failed: 0x%08x\n", static_cast<unsigned>(g_audio_port));
		g_audio_port = -1;
		return;
	}

	{
		SdlAudioLockHandle _;

		master_gain = make_unique<Gain<2>>();
		master = make_shared<Mixer<2>>();
		master_gain->bind(master);
		mixer_sound_effects = make_shared<Mixer<2>>();
		mixer_music = make_shared<Mixer<2>>();
		mixer_voice = make_shared<Mixer<2>>();
		music_player = make_shared<MusicPlayer>();
		resample_music_player = make_shared<Resampler<2>>(music_player, 1.f);
		gain_sound_effects = make_shared<Gain<2>>();
		gain_music_player = make_shared<Gain<2>>();
		gain_music_channel = make_shared<Gain<2>>();
		gain_voice_channel = make_shared<Gain<2>>();
		gain_sound_effects->bind(mixer_sound_effects);
		gain_music_player->bind(resample_music_player);
		gain_music_channel->bind(mixer_music);
		gain_voice_channel->bind(mixer_voice);
		master->add_source(gain_sound_effects);
		master->add_source(gain_music_channel);
		master->add_source(gain_voice_channel);
		mixer_music->add_source(gain_music_player);
		sound_effect_channels.clear();
		for (size_t i = 0; i < static_cast<size_t>(cv_numChannels.value); i++)
		{
			shared_ptr<SoundEffectPlayer> player = make_shared<SoundEffectPlayer>();
			sound_effect_channels.push_back(player);
			mixer_sound_effects->add_source(player);
		}
		player_voice_channels.clear();
		for (size_t i = 0; i < MAXPLAYERS; i++)
		{
			shared_ptr<VoiceStreamPlayer> player = make_shared<VoiceStreamPlayer>();
			player_voice_channels.push_back(player);
			mixer_voice->add_source(player);
		}

		output_resampler = make_shared<Resampler<2>>(make_shared<MasterTap>(),
			static_cast<float>(kMixRate) / static_cast<float>(kOutputRate));
	}

	g_audio_running = true;
	g_audio_thread = std::thread(audio_thread);

	sound_started = true;
}

} // namespace

void I_StartupSound(void)
{
	if (!sound_started)
		initialize_sound();
}

void I_ShutdownSound(void)
{
	if (g_audio_running.exchange(false) && g_audio_thread.joinable())
		g_audio_thread.join();
	if (g_audio_port >= 0)
	{
		sceAudioOutClose(g_audio_port);
		g_audio_port = -1;
	}

	SdlAudioLockHandle _;
	output_resampler = nullptr;

	master_gain = nullptr;
	master = nullptr;
	mixer_sound_effects = nullptr;
	mixer_music = nullptr;
	mixer_voice = nullptr;
	music_player = nullptr;
	resample_music_player = nullptr;
	gain_sound_effects = nullptr;
	gain_music_player = nullptr;
	gain_music_channel = nullptr;
	gain_voice_channel = nullptr;
	sound_effect_channels.clear();
	player_voice_channels.clear();

	sound_started = false;
}

void I_UpdateSound(void)
{
	// The SDL audio lock is re-entrant, so it is safe to lock twice
	// for the "fade to stop music" callback later.
	SdlAudioLockHandle _;

	if (music_fade_callback && !music_player->fading())
	{
		auto old_callback = music_fade_callback;
		music_fade_callback = nullptr;
		(old_callback());
	}
	return;
}

//
//  SFX I/O
//

int32_t I_StartSound(sfxenum_t id, uint8_t vol, uint8_t sep, uint8_t pitch, uint8_t priority, int32_t channel)
{
	(void) pitch;
	(void) priority;

	SdlAudioLockHandle _;

	if (channel >= 0 && static_cast<size_t>(channel) >= sound_effect_channels.size())
		return -1;

	shared_ptr<SoundEffectPlayer> player_channel;
	if (channel < 0)
	{
		// find a free sfx channel
		for (size_t i = 0; i < sound_effect_channels.size(); i++)
		{
			if (sound_effect_channels[i]->finished())
			{
				player_channel = sound_effect_channels[i];
				channel = i;
				break;
			}
		}
	}
	else
	{
		player_channel = sound_effect_channels[channel];
	}

	if (!player_channel)
		return -1;

	SoundChunk* chunk = static_cast<SoundChunk*>(S_sfx[id].data);
	if (chunk == nullptr)
		return -1;

	float vol_float = static_cast<float>(vol) / 255.f;
	float sep_float = static_cast<float>(sep) / 127.f - 1.f;

	player_channel->start(chunk, vol_float, sep_float);

	return channel;
}

void I_StopSound(int32_t handle)
{
	SdlAudioLockHandle _;

	if (sound_effect_channels.empty())
		return;

	if (handle < 0)
		return;

	size_t index = handle;

	if (index >= sound_effect_channels.size())
		return;

	sound_effect_channels[index]->reset();
}

dboolean I_SoundIsPlaying(int32_t handle)
{
	SdlAudioLockHandle _;

	// Handle is channel index
	if (sound_effect_channels.empty())
		return 0;

	if (handle < 0)
		return 0;

	size_t index = handle;

	if (index >= sound_effect_channels.size())
		return 0;

	return sound_effect_channels[index]->finished() ? 0 : 1;
}

void I_UpdateSoundParams(int32_t handle, uint8_t vol, uint8_t sep, uint8_t pitch)
{
	(void) pitch;

	SdlAudioLockHandle _;

	if (sound_effect_channels.empty())
		return;

	if (handle < 0)
		return;

	size_t index = handle;

	if (index >= sound_effect_channels.size())
		return;

	shared_ptr<SoundEffectPlayer>& channel = sound_effect_channels[index];
	if (!channel->finished())
	{
		float vol_float = static_cast<float>(vol) / 255.f;
		float sep_float = static_cast<float>(sep) / 127.f - 1.f;
		channel->update(vol_float, sep_float);
	}
}

void I_SetSfxVolume(int volume)
{
	SdlAudioLockHandle _;
	float vol = static_cast<float>(volume) / 100.f;

	if (gain_sound_effects)
	{
		gain_sound_effects->gain(std::clamp(vol * vol * vol, 0.f, 1.f));
	}
}

void I_SetVoiceVolume(int volume)
{
	SdlAudioLockHandle _;
	float vol = static_cast<float>(volume) / 100.f;

	if (gain_voice_channel)
	{
		gain_voice_channel->gain(std::clamp(vol * vol * vol, 0.f, 1.f));
	}
}

void I_SetMasterVolume(int volume)
{
	SdlAudioLockHandle _;
	float vol = static_cast<float>(volume) / 100.f;

	if (master_gain)
	{
		master_gain->gain(std::clamp(vol * vol * vol, 0.f, 1.f));
	}
}

/// ------------------------
//  MUSIC SYSTEM
/// ------------------------

void I_InitMusic(void)
{
	if (!sound_started)
		initialize_sound();

	SdlAudioLockHandle _;

	if (music_player != nullptr)
		*music_player = audio::MusicPlayer();
}

void I_ShutdownMusic(void)
{
	SdlAudioLockHandle _;

	if (music_player)
		*music_player = audio::MusicPlayer();
}

/// ------------------------
//  MUSIC PROPERTIES
/// ------------------------

const char* I_SongType(void)
{
	if (!music_player)
		return nullptr;

	SdlAudioLockHandle _;

	std::optional<audio::MusicType> music_type = music_player->music_type();

	if (music_type == std::nullopt)
	{
		return nullptr;
	}

	switch (*music_type)
	{
	case audio::MusicType::kOgg:
		return "OGG";
	case audio::MusicType::kMod:
		return "Mod";
	default:
		return nullptr;
	}
}

dboolean I_SongPlaying(void)
{
	if (!music_player)
		return false;

	SdlAudioLockHandle _;

	return music_player->music_type().has_value();
}

dboolean I_SongPaused(void)
{
	if (!music_player)
		return false;

	SdlAudioLockHandle _;

	return !music_player->playing();
}

/// ------------------------
//  MUSIC EFFECTS
/// ------------------------

dboolean I_SetSongSpeed(float speed)
{
	if (resample_music_player)
	{
		resample_music_player->ratio(speed);
		return true;
	}

	return false;
}

/// ------------------------
//  MUSIC SEEKING
/// ------------------------

uint32_t I_GetSongLength(void)
{
	if (!music_player)
		return 0;

	SdlAudioLockHandle _;

	std::optional<float> duration = music_player->duration_seconds();

	if (!duration)
		return 0;

	return static_cast<uint32_t>(std::round(*duration * 1000.f));
}

dboolean I_SetSongLoopPoint(uint32_t looppoint)
{
	if (!music_player)
		return 0;

	SdlAudioLockHandle _;

	if (music_player->music_type() == audio::MusicType::kOgg)
	{
		music_player->loop_point_seconds(looppoint / 1000.f);
		return true;
	}

	return false;
}

uint32_t I_GetSongLoopPoint(void)
{
	if (!music_player)
		return 0;

	SdlAudioLockHandle _;

	std::optional<float> loop_point_seconds = music_player->loop_point_seconds();

	if (!loop_point_seconds)
		return 0;

	return static_cast<uint32_t>(std::round(*loop_point_seconds * 1000.f));
}

dboolean I_SetSongPosition(uint32_t position)
{
	if (!music_player)
		return false;

	SdlAudioLockHandle _;

	music_player->seek(position / 1000.f);
	return true;
}

uint32_t I_GetSongPosition(void)
{
	if (!music_player)
		return 0;

	SdlAudioLockHandle _;

	std::optional<float> position_seconds = music_player->position_seconds();

	if (!position_seconds)
		return 0;

	return static_cast<uint32_t>(std::round(*position_seconds * 1000.f));
}

void I_UpdateSongLagThreshold(void)
{
}

void I_UpdateSongLagConditions(void)
{
}

/// ------------------------
//  MUSIC PLAYBACK
/// ------------------------

namespace
{
void print_walk_ex_stack(const std::exception& ex)
{
	CONS_Alert(CONS_WARNING, "  Caused by: %s\n", ex.what());
	try
	{
		std::rethrow_if_nested(ex);
	}
	catch (const std::exception& ex)
	{
		print_walk_ex_stack(ex);
	}
}

void print_ex(const std::exception& ex)
{
	CONS_Alert(CONS_WARNING, "Exception loading music: %s\n", ex.what());
	try
	{
		std::rethrow_if_nested(ex);
	}
	catch (const std::exception& ex)
	{
		print_walk_ex_stack(ex);
	}
}
} // namespace

dboolean I_LoadSong(char* data, size_t len)
{
	if (!music_player)
		return false;

	std::span<std::byte> data_span(reinterpret_cast<std::byte*>(data), len);
	audio::MusicPlayer new_player;
	try
	{
		new_player = audio::MusicPlayer {data_span};
	}
	catch (const std::exception& ex)
	{
		print_ex(ex);
		return false;
	}

	if (music_fade_callback && music_player->fading())
	{
		auto old_callback = music_fade_callback;
		music_fade_callback = nullptr;
		(old_callback)();
	}

	SdlAudioLockHandle _;

	try
	{
		*music_player = std::move(new_player);
	}
	catch (const std::exception& ex)
	{
		print_ex(ex);
		return false;
	}

	if (gain_music_player)
	{
		// Reset song volume to 1.0 for newly loaded songs.
		gain_music_player->gain(1.0);
	}

	return true;
}

void I_UnloadSong(void)
{
	if (!music_player)
		return;

	if (music_fade_callback && music_player->fading())
	{
		auto old_callback = music_fade_callback;
		music_fade_callback = nullptr;
		(old_callback)();
	}

	SdlAudioLockHandle _;

	*music_player = audio::MusicPlayer();
}

dboolean I_PlaySong(dboolean looping)
{
	if (!music_player)
		return false;

	SdlAudioLockHandle _;

	music_player->play(looping);

	return true;
}

void I_StopSong(void)
{
	if (!music_player)
		return;

	SdlAudioLockHandle _;

	music_player->stop();
}

void I_PauseSong(void)
{
	if (!music_player)
		return;

	SdlAudioLockHandle _;

	music_player->pause();
}

void I_ResumeSong(void)
{
	if (!music_player)
		return;

	SdlAudioLockHandle _;

	music_player->unpause();
}

void I_SetMusicVolume(int volume)
{
	float vol = static_cast<float>(volume) / 100.f;

	if (gain_music_channel)
	{
		// Music channel volume is interpreted as logarithmic rather than linear.
		// We approximate by cubing the gain level so vol 50 roughly sounds half as loud.
		gain_music_channel->gain(std::clamp(vol * vol * vol, 0.f, 1.f));
	}
}

void I_SetCurrentSongVolume(int volume)
{
	float vol = static_cast<float>(volume) / 100.f;

	if (gain_music_player)
	{
		// However, different from music channel volume, musicdef volumes are explicitly linear.
		gain_music_player->gain(std::max(vol, 0.f));
	}
}

dboolean I_SetSongTrack(int track)
{
	(void) track;
	return false;
}

/// ------------------------
//  MUSIC FADING
/// ------------------------

void I_SetInternalMusicVolume(uint8_t volume)
{
	if (!music_player)
		return;

	SdlAudioLockHandle _;

	float gain = volume / 100.f;
	music_player->internal_gain(gain);
}

void I_StopFadingSong(void)
{
	if (!music_player)
		return;

	SdlAudioLockHandle _;

	music_player->stop_fade();
}

dboolean I_FadeSongFromVolume(uint8_t target_volume, uint8_t source_volume, uint32_t ms, void (*callback)(void))
{
	if (!music_player)
		return false;

	SdlAudioLockHandle _;

	float source_gain = source_volume / 100.f;
	float target_gain = target_volume / 100.f;
	float seconds = ms / 1000.f;

	music_player->fade_from_to(source_gain, target_gain, seconds);

	if (music_fade_callback)
		music_fade_callback();
	music_fade_callback = callback;

	return true;
}

dboolean I_FadeSong(uint8_t target_volume, uint32_t ms, void (*callback)(void))
{
	if (!music_player)
		return false;

	SdlAudioLockHandle _;

	float target_gain = target_volume / 100.f;
	float seconds = ms / 1000.f;

	music_player->fade_to(target_gain, seconds);

	if (music_fade_callback)
		music_fade_callback();
	music_fade_callback = callback;

	return true;
}

static void stop_song_cb(void)
{
	if (!music_player)
		return;

	SdlAudioLockHandle _;

	music_player->stop();
}

dboolean I_FadeOutStopSong(uint32_t ms)
{
	return I_FadeSong(0.f, ms, stop_song_cb);
}

dboolean I_FadeInPlaySong(uint32_t ms, dboolean looping)
{
	if (I_PlaySong(looping))
		return I_FadeSongFromVolume(100, 0, ms, nullptr);
	else
		return false;
}

void I_UpdateAudioRecorder(void)
{
#ifdef SRB2_CONFIG_ENABLE_WEBM_MOVIES
	// must be locked since av_recorder is used by audio_callback
	SdlAudioLockHandle _;

	av_recorder = g_av_recorder;
#endif
}

// Voice chat: other players are heard (I_QueueVoiceFrameFromPlayer, above
// and below), but there is no microphone. Capture would go through
// libSceAudioIn, for which there is neither a public header nor a port
// to check a layout against, so the console reports none and the game's
// push-to-talk stays off.

dboolean I_SoundInputIsEnabled(void)
{
	return false;
}

dboolean I_SoundInputSetEnabled(dboolean enabled)
{
	(void)enabled;
	return false;
}

uint32_t I_SoundInputDequeueSamples(void *data, uint32_t len)
{
	(void)data;
	(void)len;
	return 0;
}

uint32_t I_SoundInputRemainingSamples(void)
{
	return 0;
}

void I_QueueVoiceFrameFromPlayer(int32_t playernum, void *data, uint32_t len, dboolean terminal)
{
	if (!sound_started)
	{
		return;
	}

	SdlAudioLockHandle _;
	VoiceStreamPlayer* player = player_voice_channels.at(playernum).get();
	player->stream().put(std::span((std::byte*)data, len));
}

void I_SetPlayerVoiceProperties(int32_t playernum, float volume, float sep)
{
	if (!sound_started)
	{
		return;
	}

	SdlAudioLockHandle _;
	VoiceStreamPlayer* player = player_voice_channels.at(playernum).get();
	player->set_properties(volume * volume * volume, sep);
}

void I_ResetVoiceQueue(int32_t playernum)
{
	if (!sound_started)
	{
		return;
	}

	SdlAudioLockHandle _;
	VoiceStreamPlayer* player = player_voice_channels.at(playernum).get();
	player->stream().clear();
}

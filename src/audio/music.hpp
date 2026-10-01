#pragma once

#include "audio/audio.hpp"
#include "audio/wav.hpp"
#include "io/vfs.hpp"

#include <cstdint>

namespace sl_open::audio
{
struct MusicRuntime
{
	WavStream stream;
	const WavAsset* pending_asset{};
	std::uint32_t fade_service_tick{};
	std::int16_t requested_volume{127};
	std::int16_t pending_volume{};
	std::int16_t pending_loop_count{};
	std::int16_t fade_decrement{};
	bool pending{};
	bool fading{};
	bool paused{};
};

std::uint32_t music_loop_byte_offset(const char* path);
int music_play(
	sl_open::io::Vfs& vfs,
	Runtime& audio,
	MusicRuntime& music,
	const WavAsset& asset,
	std::int32_t loop_count,
	std::int32_t volume,
	std::int32_t mode,
	std::uint32_t audio_tick);
void music_update(
	sl_open::io::Vfs& vfs,
	Runtime& audio,
	MusicRuntime& music,
	std::uint32_t audio_tick);
bool music_is_playing(const MusicRuntime& music);
void music_pause(MusicRuntime& music);
void music_resume(MusicRuntime& music);
void music_close(sl_open::io::Vfs& vfs, MusicRuntime& music);
}

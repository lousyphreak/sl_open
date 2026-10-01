#include "audio/music.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>

namespace sl_open::audio
{
namespace
{
struct LoopPoint
{
	const char* basename;
	std::uint32_t byte_offset;
};

constexpr LoopPoint kLoopPoints[] = {
	{"new_mission01", 188318},
	{"new_mission02", 237569},
	{"new_mission03", 102043},
	{"new_mission04", 223314},
	{"new_mission05", 178163},
	{"new_mission06", 144433},
	{"new_mission07", 105604},
	{"new_mission08", 75874},
	{"new_mission09", 55392},
	{"new_mission10", 127749},
	{"new_defeat", 166910},
	{"new_launch", 355065},
	{"new_pensive", 132922},
	{"new_victory", 176922},
	{"new_searching mission 01", 297178},
	{"new_searching mission 02", 126079},
	{"new_searching mission 03", 108719},
	{"new_searching mission 04", 143807},
	{"new_searching mission 05", 101402},
	{"new_searching mission 06", 64511},
	{"new_searching mission 07", 116109},
	{"new_searching mission 08", 158719},
	{"new_searching mission 09", 90111},
	{"new_searching mission 10", 146431},
	{"new_sim01", 389937},
	{"new_sim02", 211074},
	{"new_sim03", 95328},
	{"new_sim04", 438286},
	{"new_sim05", 244327},
	{"new_sim06", 157445},
	{"new_sim07", 148140},
	{"new_sim08", 201251},
	{"new_sim09", 264760},
	{"new_sim10", 337966},
	{"new__spare!", 187648},
};

const char* basename(const char* path)
{
	const char* result = path;
	for (const char* cursor = path; *cursor != '\0'; ++cursor)
	{
		if (*cursor == '/' || *cursor == '\\')
		{
			result = cursor + 1;
		}
	}
	return result;
}

bool prefix_equal_case_insensitive(const char* value, const char* prefix)
{
	while (*prefix != '\0')
	{
		if (*value == '\0'
			|| std::tolower(static_cast<unsigned char>(*value))
				!= std::tolower(static_cast<unsigned char>(*prefix)))
		{
			return false;
		}
		++value;
		++prefix;
	}
	return true;
}

bool open_immediate(
	sl_open::io::Vfs& vfs,
	Runtime& audio,
	MusicRuntime& music,
	const WavAsset& asset,
	std::int32_t loop_count,
	std::int32_t volume)
{
	const bool paused = music.paused;
	wav_stream_close(vfs, music.stream);
	music.pending = false;
	music.pending_asset = nullptr;
	music.fading = false;
	music.fade_decrement = 0;
	if (!audio.ready
		|| !wav_stream_open_looped(
			asset,
			audio.streams[0].source,
			audio.streams[0].buffers,
			loop_count,
			music_loop_byte_offset(asset.path),
			music.stream))
	{
		return false;
	}
	music.requested_volume =
		static_cast<std::int16_t>(std::clamp(volume, 0, 127));
	apply_music_gain(audio, music.requested_volume);
	if (paused)
	{
		wav_stream_pause(music.stream);
	}
	return true;
}
}

std::uint32_t music_loop_byte_offset(const char* path)
{
	if (path == nullptr)
	{
		return 0;
	}
	const char* name = basename(path);
	for (const LoopPoint& point : kLoopPoints)
	{
		if (prefix_equal_case_insensitive(name, point.basename))
		{
			return point.byte_offset;
		}
	}
	return 0;
}

int music_play(
	sl_open::io::Vfs& vfs,
	Runtime& audio,
	MusicRuntime& music,
	const WavAsset& asset,
	std::int32_t loop_count,
	std::int32_t volume,
	std::int32_t mode,
	std::uint32_t audio_tick)
{
	if (mode == 1)
	{
		return open_immediate(
			vfs, audio, music, asset, loop_count, volume)
			? 0
			: 1;
	}

	music.pending_asset = &asset;
	music.pending_loop_count =
		static_cast<std::int16_t>(loop_count);
	music.pending_volume =
		static_cast<std::int16_t>(volume);
	music.pending = true;
	if (music.stream.active)
	{
		music.fading = true;
		music.fade_decrement = 5;
		music.fade_service_tick = audio_tick;
	}
	return 1;
}

bool music_is_playing(const MusicRuntime& music)
{
	// A Miles stream remains status four while it is open, including during
	// buffering and pause. WavStream::active is the matching ownership state;
	// the OpenAL source may transiently be INITIAL during an asynchronous read
	// or STOPPED during a recoverable underrun.
	return music.stream.active;
}

void music_update(
	sl_open::io::Vfs& vfs,
	Runtime& audio,
	MusicRuntime& music,
	std::uint32_t audio_tick)
{
	if (music.stream.active && !music.paused)
	{
		wav_stream_update(vfs, music.stream);
	}
	if (!music.paused && music.fading)
	{
		while (audio_tick - music.fade_service_tick >= 6)
		{
			music.fade_service_tick += 6;
			music.requested_volume -= music.fade_decrement;
			if (music.requested_volume < 0)
			{
				wav_stream_close(vfs, music.stream);
				music.fading = false;
				music.fade_decrement = 0;
				break;
			}
			apply_music_gain(audio, music.requested_volume);
		}
	}
	if (!music.paused
		&& music.pending
		&& !music_is_playing(music))
	{
		const std::int16_t loop_count = music.pending_loop_count;
		const std::int16_t volume = music.pending_volume;
		const WavAsset* asset = music.pending_asset;
		music.pending = false;
		music.pending_asset = nullptr;
		if (asset != nullptr)
		{
			open_immediate(vfs, audio, music, *asset, loop_count, volume);
		}
	}
}

void music_pause(MusicRuntime& music)
{
	if (!music.paused)
	{
		wav_stream_pause(music.stream);
		music.paused = true;
	}
}

void music_resume(MusicRuntime& music)
{
	if (music.paused)
	{
		wav_stream_resume(music.stream);
		music.paused = false;
	}
}

void music_close(sl_open::io::Vfs& vfs, MusicRuntime& music)
{
	wav_stream_close(vfs, music.stream);
	music = {};
}
}

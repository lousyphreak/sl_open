#pragma once

#include <AL/al.h>
#include <AL/alc.h>

#include <cstdint>

namespace sl_open::audio
{
constexpr std::uint32_t kVoiceCount = 48;
constexpr std::uint32_t kStreamCount = 5;
constexpr std::uint32_t kStreamBufferCount = 16;

enum class SpatialMode : std::uint8_t
{
	Off,
	Standard,
	Hrtf,
};

struct Voice
{
	ALuint source{};
	ALuint buffer{};
	std::uint32_t priority{};
	int requested_volume{};
	int remaining_plays{};
	bool protected_from_eviction{};
};

struct Stream
{
	ALuint source{};
	ALuint buffers[kStreamBufferCount]{};
};

struct Runtime
{
	ALCdevice* device{};
	ALCcontext* context{};
	Voice voices[kVoiceCount]{};
	Stream streams[kStreamCount]{};
	std::uint32_t voice_count{};
	std::uint32_t stream_count{};
	int effects_volume{80};
	int music_volume{80};
	int speech_volume{127};
	int master_volume{127};
	SpatialMode spatial_mode{SpatialMode::Standard};
	bool hrtf_supported{};
	bool ready{};
};

bool init(Runtime& runtime);
void shutdown(Runtime& runtime);
bool set_spatial_mode(Runtime& runtime, SpatialMode mode);
float speech_gain(const Runtime& runtime);
void apply_music_gain(Runtime& runtime, int requested_volume);
void apply_stream_gains(Runtime& runtime);
const char* device_name(const Runtime& runtime);
}

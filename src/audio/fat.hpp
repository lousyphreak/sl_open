#pragma once

#include "audio/audio.hpp"
#include "core/blob.hpp"
#include "io/vfs.hpp"

#include <cstdint>

namespace sl_open::audio
{
constexpr std::uint32_t kFatMaxSamples = 256;
constexpr std::uint32_t kEffectVoiceCount = 16;

struct FatSample
{
	std::uint32_t pcm_offset{};
	std::uint32_t pcm_bytes{};
	std::uint32_t frame_count{};
	std::uint32_t sample_rate{};
	std::uint32_t priority{};
	std::uint16_t channels{};
};

struct FatBank
{
	sl_open::io::VfsFile file;
	sl_open::io::ReadRequest request;
	sl_open::Blob stored;
	sl_open::Blob pcm;
	FatSample samples[kFatMaxSamples]{};
	std::uint32_t sample_count{};
	bool read_pending{};
	bool loading{};
	bool ready{};
};

bool fat_bank_open(sl_open::io::Vfs& vfs, const char* path, FatBank& bank);
bool fat_bank_open_from_archive(
	sl_open::io::Vfs& vfs,
	const char* archive,
	const char* path,
	FatBank& bank);
bool fat_bank_update(sl_open::io::Vfs& vfs, FatBank& bank);
void fat_bank_close(sl_open::io::Vfs& vfs, FatBank& bank);

const std::int16_t* fat_sample_pcm(const FatBank& bank, std::uint32_t index);

int fat_play_auto(
	Runtime& runtime,
	const FatBank& bank,
	std::uint32_t sample_index,
	int volume,
	int loop_count,
	int pan,
	int pitch_steps);
int fat_play_in_slot(
	Runtime& runtime,
	const FatBank& bank,
	std::uint32_t slot,
	std::uint32_t sample_index,
	int volume,
	int loop_count,
	int pan,
	int pitch_steps);
int fat_play_spatial(
	Runtime& runtime,
	const FatBank& bank,
	std::uint32_t sample_index,
	int volume,
	int loop_count,
	const float position[3],
	const float direction[3],
	float minimum_distance,
	float maximum_distance,
	float cone_inner_degrees,
	float cone_outer_degrees,
	float cone_outer_gain);
void fat_stop(Runtime& runtime, std::uint32_t slot);
void fat_update_voices(Runtime& runtime);
}

#pragma once

#include "core/blob.hpp"
#include "io/vfs.hpp"

#include <AL/al.h>

#include <cstddef>
#include <cstdint>

namespace sl_open::audio
{
enum class WavEncoding : std::uint8_t
{
	Pcm,
	ImaAdpcm,
};

struct WavInfo
{
	WavEncoding encoding{};
	std::uint32_t sample_rate{};
	std::uint32_t total_frames{};
	std::uint32_t data_offset{};
	std::uint32_t data_size{};
	std::uint16_t channels{};
	std::uint16_t block_align{};
	std::uint16_t bits_per_sample{};
	std::uint16_t samples_per_block{};
};

struct Pcm
{
	sl_open::Blob samples;
	std::uint32_t sample_rate{};
	std::uint32_t frame_count{};
	std::uint16_t channels{};
};

struct WavAsset
{
	char path[128]{};
	sl_open::Blob file;
	WavInfo info;
};

constexpr std::size_t kWavHeaderBytes = 4096;
constexpr std::size_t kWavEncodedBytes = 8192;
constexpr std::size_t kWavPcmFrames = 8192;

struct WavStream
{
	sl_open::io::VfsFile file;
	sl_open::io::ReadRequest request;
	WavInfo info;
	ALuint source{};
	ALuint free_buffers[4]{};
	std::uint8_t header[kWavHeaderBytes]{};
	std::uint8_t encoded[kWavEncodedBytes]{};
	std::int16_t pcm[kWavPcmFrames * 2]{};
	const std::uint8_t* memory{};
	std::size_t memory_size{};
	std::uint64_t encoded_offset{};
	std::uint32_t encoded_remaining{};
	std::uint32_t frames_remaining{};
	std::uint32_t read_size{};
	std::uint32_t free_count{};
	std::uint32_t loop_byte_offset{};
	std::uint32_t loop_skip_frames{};
	std::int32_t loop_count{1};
	bool read_pending{};
	bool paused{};
	bool active{};
};

bool wav_asset_load(
	sl_open::io::Vfs& vfs,
	const char* path,
	WavAsset& asset);
bool wav_asset_matches(const WavAsset& asset, const char* path);

bool parse_wav(
	const std::uint8_t* data,
	std::size_t available,
	std::size_t file_size,
	WavInfo& info);
bool decode_wav_into(
	const std::uint8_t* data,
	std::size_t size,
	std::int16_t* samples,
	std::size_t sample_capacity,
	WavInfo& info);
bool decode_wav(const std::uint8_t* data, std::size_t size, Pcm& pcm);

bool wav_stream_open(
	sl_open::io::Vfs& vfs,
	const char* path,
	ALuint source,
	const ALuint* buffers,
	bool loop,
	WavStream& stream);
bool wav_stream_update(sl_open::io::Vfs& vfs, WavStream& stream);
bool wav_stream_open_looped(
	const WavAsset& asset,
	ALuint source,
	const ALuint* buffers,
	std::int32_t loop_count,
	std::uint32_t loop_byte_offset,
	WavStream& stream);
bool wav_stream_open_looped(
	sl_open::io::Vfs& vfs,
	const char* path,
	ALuint source,
	const ALuint* buffers,
	std::int32_t loop_count,
	std::uint32_t loop_byte_offset,
	WavStream& stream);
void wav_stream_pause(WavStream& stream);
void wav_stream_resume(WavStream& stream);
void wav_stream_close(sl_open::io::Vfs& vfs, WavStream& stream);
}

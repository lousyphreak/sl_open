#pragma once

#include "audio/cb97.hpp"
#include "core/blob.hpp"
#include "io/vfs.hpp"

#include <AL/al.h>

#include <cstdint>

namespace sl_open::audio
{
constexpr std::uint32_t kCb97PcmBufferSamples = 8192;

struct Cb97Asset
{
	char path[128]{};
	sl_open::Blob encoded;
};

struct Cb97Stream
{
	sl_open::io::VfsFile file;
	sl_open::io::ReadRequest request;
	sl_open::Blob encoded;
	const std::uint8_t* encoded_data{};
	std::size_t encoded_size{};
	Cb97Decoder decoder;
	ALuint source{};
	ALuint free_buffers[4]{};
	std::int16_t pcm[kCb97PcmBufferSamples]{};
	std::uint32_t free_count{};
	bool decoder_ready{};
	bool read_pending{};
	bool loop{};
	bool active{};
	bool paused{};
};

bool cb97_asset_load(
	sl_open::io::Vfs& vfs,
	const char* path,
	Cb97Asset& asset);
bool cb97_asset_matches(const Cb97Asset& asset, const char* path);
bool cb97_stream_open(
	const Cb97Asset& asset,
	ALuint source,
	const ALuint* buffers,
	bool loop,
	Cb97Stream& stream,
	float gain = 1.0f);
bool cb97_stream_open(
	sl_open::io::Vfs& vfs,
	const char* path,
	ALuint source,
	const ALuint* buffers,
	bool loop,
	Cb97Stream& stream,
	float gain = 1.0f);
bool cb97_stream_update(sl_open::io::Vfs& vfs, Cb97Stream& stream);
void cb97_stream_pause(Cb97Stream& stream);
void cb97_stream_resume(Cb97Stream& stream);
void cb97_stream_close(sl_open::io::Vfs& vfs, Cb97Stream& stream);
}

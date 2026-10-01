#pragma once

#include "core/blob.hpp"
#include "io/vfs.hpp"

#include <AL/al.h>
#include <dr_mp3.h>

#include <cstdint>

namespace sl_open::audio
{
constexpr std::uint32_t kMp3PcmFrames = 8192;

struct Mp3Stream
{
	sl_open::io::VfsFile file;
	sl_open::io::ReadRequest request;
	sl_open::Blob encoded;
	drmp3 decoder{};
	ALuint source{};
	ALuint free_buffers[4]{};
	std::int16_t pcm[kMp3PcmFrames * 2]{};
	std::uint32_t free_count{};
	bool decoder_ready{};
	bool read_pending{};
	bool loop{};
	bool active{};
};

bool mp3_stream_open(
	sl_open::io::Vfs& vfs,
	const char* path,
	ALuint source,
	const ALuint* buffers,
	bool loop,
	Mp3Stream& stream);
bool mp3_stream_update(sl_open::io::Vfs& vfs, Mp3Stream& stream);
void mp3_stream_close(sl_open::io::Vfs& vfs, Mp3Stream& stream);
}

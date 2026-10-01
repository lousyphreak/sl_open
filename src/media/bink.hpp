#pragma once

#include "io/vfs.hpp"

#include <cstddef>
#include <cstdint>

namespace sl_open::media
{
constexpr std::uint32_t kBinkMaxFrames = 4096;
constexpr std::uint32_t kBinkMaxFrameBytes = 192 * 1024;
constexpr std::uint32_t kBinkHeaderBytes = 44 + 12 + kBinkMaxFrames * 4;

enum class BinkOpenStatus : std::uint8_t
{
	pending,
	ready,
	failed,
};

struct BinkAudioInfo
{
	std::uint32_t max_decoded_bytes{};
	std::uint32_t track_id{};
	std::uint16_t sample_rate{};
	std::uint8_t channels{};
};

struct BinkInfo
{
	std::uint32_t tag{};
	std::uint32_t file_size{};
	std::uint32_t frame_count{};
	std::uint32_t largest_frame{};
	std::uint32_t width{};
	std::uint32_t height{};
	std::uint32_t fps_numerator{};
	std::uint32_t fps_denominator{};
	std::uint32_t video_flags{};
	BinkAudioInfo audio{};
	bool has_audio{};
};

struct BinkFrame
{
	std::uint32_t offset{};
	std::uint32_t size{};
	bool keyframe{};
};

struct BinkFile
{
	io::VfsFile file;
	io::ReadRequest request;
	BinkInfo info;
	BinkFrame frames[kBinkMaxFrames]{};
	std::uint8_t header[kBinkHeaderBytes]{};
	std::uint32_t header_size{};
	std::uint32_t read_size{};
	BinkOpenStatus status{BinkOpenStatus::failed};
	bool reading_prefix{};
};

bool parse_bink_header(
	const std::uint8_t* data,
	std::size_t available,
	std::size_t file_size,
	BinkInfo& info,
	BinkFrame* frames,
	std::size_t frame_capacity,
	std::uint32_t& required_bytes);

BinkOpenStatus bink_open(io::Vfs& vfs, const char* path, BinkFile& movie);
BinkOpenStatus bink_open_update(io::Vfs& vfs, BinkFile& movie);
io::IoStatus bink_read_frame(
	io::Vfs& vfs,
	BinkFile& movie,
	std::uint32_t frame,
	void* destination,
	std::size_t capacity,
	io::ReadRequest& request);
void bink_close(io::Vfs& vfs, BinkFile& movie);
}

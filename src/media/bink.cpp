#include "media/bink.hpp"

#include "io/endian.hpp"

#include <cstring>

namespace sl_open::media
{
namespace
{
constexpr std::uint32_t make_tag(char a, char b, char c, char d)
{
	return static_cast<std::uint8_t>(a)
		| static_cast<std::uint32_t>(static_cast<std::uint8_t>(b)) << 8
		| static_cast<std::uint32_t>(static_cast<std::uint8_t>(c)) << 16
		| static_cast<std::uint32_t>(static_cast<std::uint8_t>(d)) << 24;
}

constexpr std::uint32_t kBikF = make_tag('B', 'I', 'K', 'f');
constexpr std::uint32_t kBikG = make_tag('B', 'I', 'K', 'g');
constexpr std::uint32_t kBikI = make_tag('B', 'I', 'K', 'i');
constexpr std::uint16_t kAudioStereo = 0x2000;
constexpr std::uint16_t kAudioDct = 0x1000;
constexpr std::size_t kPrefixBytes = 44;

bool supported_tag(std::uint32_t tag)
{
	return tag == kBikF || tag == kBikG || tag == kBikI;
}

BinkOpenStatus fail_open(io::Vfs& vfs, BinkFile& movie)
{
	io::vfs_close(vfs, movie.file);
	movie.status = BinkOpenStatus::failed;
	movie.request = {};
	return movie.status;
}

BinkOpenStatus begin_header_read(
	io::Vfs& vfs,
	BinkFile& movie,
	std::uint32_t offset,
	std::uint32_t size)
{
	movie.read_size = size;
	movie.request = {};
	const io::IoStatus status = io::vfs_read_at(
		vfs,
		movie.file,
		offset,
		movie.header + offset,
		size,
		movie.request);
	if (status == io::IoStatus::failed)
	{
		return fail_open(vfs, movie);
	}
	movie.status = BinkOpenStatus::pending;
	return movie.status;
}

BinkOpenStatus consume_header_read(io::Vfs& vfs, BinkFile& movie)
{
	if (movie.request.status == io::IoStatus::pending)
	{
		io::vfs_poll(vfs, movie.request);
	}
	if (movie.request.status == io::IoStatus::pending)
	{
		return BinkOpenStatus::pending;
	}
	if (movie.request.status != io::IoStatus::complete
		|| movie.request.transferred != movie.read_size)
	{
		return fail_open(vfs, movie);
	}

	if (movie.reading_prefix)
	{
		BinkInfo prefix_info;
		std::uint32_t required = 0;
		if (!parse_bink_header(
				movie.header,
				kPrefixBytes,
				static_cast<std::size_t>(movie.file.size),
				prefix_info,
				nullptr,
				0,
				required)
			|| required > kBinkHeaderBytes
			|| required > movie.file.size)
		{
			return fail_open(vfs, movie);
		}
		movie.header_size = required;
		movie.reading_prefix = false;
		if (required > kPrefixBytes)
		{
			return begin_header_read(
				vfs,
				movie,
				static_cast<std::uint32_t>(kPrefixBytes),
				required - static_cast<std::uint32_t>(kPrefixBytes));
		}
	}

	std::uint32_t required = 0;
	if (!parse_bink_header(
			movie.header,
			movie.header_size,
			static_cast<std::size_t>(movie.file.size),
			movie.info,
			movie.frames,
			kBinkMaxFrames,
			required)
		|| required != movie.header_size)
	{
		return fail_open(vfs, movie);
	}
	movie.request = {};
	movie.status = BinkOpenStatus::ready;
	return movie.status;
}
}

bool parse_bink_header(
	const std::uint8_t* data,
	std::size_t available,
	std::size_t file_size,
	BinkInfo& info,
	BinkFrame* frames,
	std::size_t frame_capacity,
	std::uint32_t& required_bytes)
{
	info = {};
	required_bytes = 0;
	if (data == nullptr || available < kPrefixBytes)
	{
		return false;
	}

	info.tag = io::read_le32(data);
	const std::uint64_t declared_file_size =
		static_cast<std::uint64_t>(io::read_le32(data + 4)) + 8;
	if (declared_file_size > UINT32_MAX)
	{
		return false;
	}
	info.file_size = static_cast<std::uint32_t>(declared_file_size);
	info.frame_count = io::read_le32(data + 8);
	info.largest_frame = io::read_le32(data + 12);
	info.width = io::read_le32(data + 20);
	info.height = io::read_le32(data + 24);
	info.fps_numerator = io::read_le32(data + 28);
	info.fps_denominator = io::read_le32(data + 32);
	info.video_flags = io::read_le32(data + 36);
	const std::uint32_t audio_tracks = io::read_le32(data + 40);

	if (!supported_tag(info.tag)
		|| info.file_size < kPrefixBytes || info.file_size > file_size
		|| info.frame_count == 0 || info.frame_count > kBinkMaxFrames
		|| info.largest_frame == 0 || info.largest_frame > kBinkMaxFrameBytes
		|| info.largest_frame > info.file_size
		|| info.width == 0 || info.width > 640
		|| info.height == 0 || info.height > 480
		|| info.fps_numerator == 0 || info.fps_denominator == 0
		|| info.video_flags != 0
		|| audio_tracks > 1)
	{
		return false;
	}

	const std::uint64_t header_bytes =
		kPrefixBytes
		+ static_cast<std::uint64_t>(audio_tracks) * 12
		+ static_cast<std::uint64_t>(info.frame_count) * 4;
	if (header_bytes > kBinkHeaderBytes || header_bytes > UINT32_MAX)
	{
		return false;
	}
	required_bytes = static_cast<std::uint32_t>(header_bytes);
	if (available < required_bytes)
	{
		return true;
	}

	std::size_t position = kPrefixBytes;
	if (audio_tracks != 0)
	{
		info.audio.max_decoded_bytes = io::read_le32(data + position);
		position += 4;
		info.audio.sample_rate = io::read_le16(data + position);
		const std::uint16_t audio_flags = io::read_le16(data + position + 2);
		position += 4;
		info.audio.track_id = io::read_le32(data + position);
		position += 4;
		info.audio.channels = (audio_flags & kAudioStereo) != 0 ? 2 : 1;
		info.has_audio = true;
		if (info.audio.max_decoded_bytes == 0
			|| info.audio.max_decoded_bytes > 192 * 1024
			|| (info.audio.sample_rate != 22050
				&& info.audio.sample_rate != 44100)
			|| (audio_flags & kAudioDct) != 0)
		{
			return false;
		}
	}

	if (frames == nullptr)
	{
		return true;
	}
	if (frame_capacity < info.frame_count)
	{
		return false;
	}

	std::uint32_t packed_offset = io::read_le32(data + position);
	for (std::uint32_t index = 0; index < info.frame_count; ++index)
	{
		const std::uint32_t offset = packed_offset & ~std::uint32_t{1};
		const bool keyframe = (packed_offset & 1) != 0;
		if (index + 1 < info.frame_count)
		{
			packed_offset = io::read_le32(data + position + (index + 1) * 4);
		}
		else
		{
			packed_offset = info.file_size;
		}
		const std::uint32_t next_offset = packed_offset & ~std::uint32_t{1};
		if (offset < required_bytes || next_offset <= offset
			|| next_offset > info.file_size
			|| next_offset - offset > kBinkMaxFrameBytes)
		{
			return false;
		}
		frames[index] = {offset, next_offset - offset, keyframe};
	}
	return true;
}

BinkOpenStatus bink_open(io::Vfs& vfs, const char* path, BinkFile& movie)
{
	bink_close(vfs, movie);
	if (!io::vfs_open(vfs, path, movie.file)
		|| movie.file.size < kPrefixBytes)
	{
		return BinkOpenStatus::failed;
	}
	movie.header_size = static_cast<std::uint32_t>(kPrefixBytes);
	movie.reading_prefix = true;
	return begin_header_read(
		vfs, movie, 0, static_cast<std::uint32_t>(kPrefixBytes));
}

BinkOpenStatus bink_open_update(io::Vfs& vfs, BinkFile& movie)
{
	if (movie.status != BinkOpenStatus::pending)
	{
		return movie.status;
	}
	return consume_header_read(vfs, movie);
}

io::IoStatus bink_read_frame(
	io::Vfs& vfs,
	BinkFile& movie,
	std::uint32_t frame,
	void* destination,
	std::size_t capacity,
	io::ReadRequest& request)
{
	if (movie.status != BinkOpenStatus::ready
		|| frame >= movie.info.frame_count
		|| destination == nullptr
		|| capacity < movie.frames[frame].size)
	{
		request = {};
		return io::IoStatus::failed;
	}
	return io::vfs_read_at(
		vfs,
		movie.file,
		movie.frames[frame].offset,
		destination,
		movie.frames[frame].size,
		request);
}

void bink_close(io::Vfs& vfs, BinkFile& movie)
{
	io::vfs_close(vfs, movie.file);
	movie.request = {};
	movie.info = {};
	movie.header_size = 0;
	movie.read_size = 0;
	movie.status = BinkOpenStatus::failed;
	movie.reading_prefix = false;
}
}

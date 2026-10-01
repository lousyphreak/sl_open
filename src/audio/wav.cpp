#include "audio/wav.hpp"

#include "io/endian.hpp"

#include <algorithm>
#include <cctype>
#include <climits>
#include <cstdio>
#include <cstring>

namespace sl_open::audio
{
namespace
{
constexpr int kImaIndexDelta[8] = {-1, -1, -1, -1, 2, 4, 6, 8};
constexpr int kImaStep[89] = {
	7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31,
	34, 37, 41, 45, 50, 55, 60, 66, 73, 80, 88, 97, 107, 118,
	130, 143, 157, 173, 190, 209, 230, 253, 279, 307, 337, 371,
	408, 449, 494, 544, 598, 658, 724, 796, 876, 963, 1060, 1166,
	1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024,
	3327, 3660, 4026, 4428, 4871, 5358, 5894, 6484, 7132, 7846,
	8630, 9493, 10442, 11487, 12635, 13899, 15289, 16818, 18500,
	20350, 22385, 24623, 27086, 29794, 32767,
};

struct ImaState
{
	int predictor{};
	int step_index{};
};

bool path_equal(const char* left, const char* right)
{
	while (*left != '\0' && *right != '\0')
	{
		const unsigned char a = static_cast<unsigned char>(
			*left == '\\' ? '/' : *left);
		const unsigned char b = static_cast<unsigned char>(
			*right == '\\' ? '/' : *right);
		if (std::tolower(a) != std::tolower(b))
		{
			return false;
		}
		++left;
		++right;
	}
	return *left == *right;
}

std::int16_t ima_sample(ImaState& state, std::uint8_t nibble)
{
	const int step = kImaStep[state.step_index];
	int difference = step >> 3;
	if ((nibble & 1) != 0)
	{
		difference += step >> 2;
	}
	if ((nibble & 2) != 0)
	{
		difference += step >> 1;
	}
	if ((nibble & 4) != 0)
	{
		difference += step;
	}
	state.predictor += (nibble & 8) != 0 ? -difference : difference;
	state.predictor = std::clamp(state.predictor, -32768, 32767);
	state.step_index = std::clamp(
		state.step_index + kImaIndexDelta[nibble & 7], 0, 88);
	return static_cast<std::int16_t>(state.predictor);
}

bool decode_ima_block(
	const WavInfo& info,
	const std::uint8_t* input,
	std::size_t input_size,
	std::int16_t* output,
	std::uint32_t frame_capacity,
	std::uint32_t& frames)
{
	frames = 0;
	const std::size_t header_size = static_cast<std::size_t>(info.channels) * 4;
	if (input_size < header_size || frame_capacity == 0)
	{
		return false;
	}

	ImaState state[2];
	for (std::uint32_t channel = 0; channel < info.channels; ++channel)
	{
		const std::uint8_t* header = input + channel * 4;
		state[channel].predictor =
			static_cast<std::int16_t>(sl_open::io::read_le16(header));
		state[channel].step_index = header[2];
		if (state[channel].step_index > 88 || header[3] != 0)
		{
			return false;
		}
		output[channel] = static_cast<std::int16_t>(state[channel].predictor);
	}
	frames = 1;

	std::size_t position = header_size;
	if (info.channels == 1)
	{
		while (position < input_size && frames < frame_capacity
			&& frames < info.samples_per_block)
		{
			const std::uint8_t byte = input[position++];
			output[frames++] = ima_sample(state[0], byte & 0x0f);
			if (frames < frame_capacity && frames < info.samples_per_block)
			{
				output[frames++] = ima_sample(state[0], byte >> 4);
			}
		}
		return true;
	}

	while (position + 8 <= input_size
		&& frames < frame_capacity && frames < info.samples_per_block)
	{
		std::int16_t decoded[2][8];
		for (std::uint32_t channel = 0; channel < 2; ++channel)
		{
			for (std::uint32_t byte = 0; byte < 4; ++byte)
			{
				const std::uint8_t packed = input[position + channel * 4 + byte];
				decoded[channel][byte * 2] =
					ima_sample(state[channel], packed & 0x0f);
				decoded[channel][byte * 2 + 1] =
					ima_sample(state[channel], packed >> 4);
			}
		}
		position += 8;

		const std::uint32_t group_frames = std::min<std::uint32_t>(
			8, std::min(frame_capacity, static_cast<std::uint32_t>(
				info.samples_per_block)) - frames);
		for (std::uint32_t frame = 0; frame < group_frames; ++frame)
		{
			output[(frames + frame) * 2] = decoded[0][frame];
			output[(frames + frame) * 2 + 1] = decoded[1][frame];
		}
		frames += group_frames;
	}
	return true;
}

bool decode_chunk(
	const WavInfo& info,
	const std::uint8_t* input,
	std::size_t input_size,
	std::int16_t* output,
	std::uint32_t frame_capacity,
	std::uint32_t& frames)
{
	frames = 0;
	if (info.encoding == WavEncoding::Pcm)
	{
		const std::size_t bytes_per_frame =
			static_cast<std::size_t>(info.channels) * info.bits_per_sample / 8;
		if (bytes_per_frame == 0)
		{
			return false;
		}
		frames = static_cast<std::uint32_t>(std::min<std::size_t>(
			input_size / bytes_per_frame, frame_capacity));
		const std::size_t samples = static_cast<std::size_t>(frames) * info.channels;
		if (info.bits_per_sample == 8)
		{
			for (std::size_t index = 0; index < samples; ++index)
			{
				output[index] =
					static_cast<std::int16_t>((static_cast<int>(input[index]) - 128) << 8);
			}
		}
		else
		{
			for (std::size_t index = 0; index < samples; ++index)
			{
				output[index] =
					static_cast<std::int16_t>(sl_open::io::read_le16(input + index * 2));
			}
		}
		return true;
	}

	std::size_t position = 0;
	while (position < input_size && frames < frame_capacity)
	{
		const std::size_t block_size = std::min<std::size_t>(
			info.block_align, input_size - position);
		std::uint32_t block_frames = 0;
		if (!decode_ima_block(
				info,
				input + position,
				block_size,
				output + static_cast<std::size_t>(frames) * info.channels,
				frame_capacity - frames,
				block_frames))
		{
			return false;
		}
		frames += block_frames;
		position += block_size;
	}
	return true;
}

bool finish_stream_read(sl_open::io::Vfs& vfs, WavStream& stream)
{
	if (stream.request.status != sl_open::io::IoStatus::complete
		|| stream.request.transferred != stream.read_size)
	{
		wav_stream_close(vfs, stream);
		return false;
	}

	std::uint32_t frames = 0;
	const std::uint32_t pcm_capacity =
		static_cast<std::uint32_t>(kWavPcmFrames);
	const std::uint32_t skipped_capacity =
		std::min(stream.loop_skip_frames, pcm_capacity);
	const std::uint32_t capacity = std::min<std::uint32_t>(
		pcm_capacity,
		stream.frames_remaining > pcm_capacity - skipped_capacity
			? pcm_capacity
			: stream.frames_remaining + skipped_capacity);
	if (!decode_chunk(
			stream.info,
			stream.encoded,
			stream.read_size,
			stream.pcm,
			capacity,
			frames)
		|| frames == 0 || stream.free_count == 0)
	{
		wav_stream_close(vfs, stream);
		return false;
	}
	if (stream.loop_skip_frames != 0)
	{
		const std::uint32_t skipped =
			std::min(stream.loop_skip_frames, frames);
		frames -= skipped;
		stream.loop_skip_frames -= skipped;
		if (frames != 0)
		{
			std::memmove(
				stream.pcm,
				stream.pcm
					+ static_cast<std::size_t>(skipped)
						* stream.info.channels,
				static_cast<std::size_t>(frames)
					* stream.info.channels
					* sizeof(std::int16_t));
		}
	}

	const ALuint buffer = stream.free_buffers[--stream.free_count];
	const ALenum format =
		stream.info.channels == 1 ? AL_FORMAT_MONO16 : AL_FORMAT_STEREO16;
	while (alGetError() != AL_NO_ERROR)
	{
	}
	if (frames != 0)
	{
		alBufferData(
			buffer,
			format,
			stream.pcm,
			static_cast<ALsizei>(
				static_cast<std::size_t>(frames)
					* stream.info.channels * 2),
			static_cast<ALsizei>(stream.info.sample_rate));
		alSourceQueueBuffers(stream.source, 1, &buffer);
		if (alGetError() != AL_NO_ERROR)
		{
			wav_stream_close(vfs, stream);
			return false;
		}
	}
	else
	{
		stream.free_buffers[stream.free_count++] = buffer;
	}

	stream.encoded_offset += stream.read_size;
	stream.encoded_remaining -= stream.read_size;
	stream.frames_remaining -= frames;
	stream.read_pending = false;
	stream.request = {};
	return true;
}

std::uint32_t next_read_size(const WavStream& stream)
{
	std::uint32_t size = std::min<std::uint32_t>(
		stream.encoded_remaining, kWavEncodedBytes);
	const std::uint32_t alignment = stream.info.encoding == WavEncoding::ImaAdpcm
		? stream.info.block_align
		: static_cast<std::uint32_t>(
			stream.info.channels * stream.info.bits_per_sample / 8);
	if (size < stream.encoded_remaining)
	{
		size -= size % alignment;
	}
	return size;
}
}

bool wav_asset_load(
	sl_open::io::Vfs& vfs,
	const char* path,
	WavAsset& asset)
{
	asset = {};
	if (path == nullptr
		|| std::snprintf(asset.path, sizeof(asset.path), "%s", path) <= 0
		|| !sl_open::io::vfs_read_all(vfs, path, asset.file)
		|| !parse_wav(
			asset.file.data,
			std::min(asset.file.size, kWavHeaderBytes),
			asset.file.size,
			asset.info))
	{
		asset = {};
		return false;
	}
	return true;
}

bool wav_asset_matches(const WavAsset& asset, const char* path)
{
	return path != nullptr && path_equal(asset.path, path);
}

bool parse_wav(
	const std::uint8_t* data,
	std::size_t available,
	std::size_t file_size,
	WavInfo& info)
{
	info = {};
	if (data == nullptr || available < 12
		|| std::memcmp(data, "RIFF", 4) != 0
		|| std::memcmp(data + 8, "WAVE", 4) != 0)
	{
		return false;
	}

	bool have_format = false;
	bool have_data = false;
	std::uint32_t fact_frames = 0;
	std::size_t position = 12;
	while (position + 8 <= available)
	{
		const std::uint32_t chunk_size = sl_open::io::read_le32(data + position + 4);
		const std::size_t payload = position + 8;
		if (std::memcmp(data + position, "data", 4) == 0)
		{
			if (payload > UINT32_MAX || chunk_size > file_size - payload)
			{
				return false;
			}
			info.data_offset = static_cast<std::uint32_t>(payload);
			info.data_size = chunk_size;
			have_data = true;
			break;
		}
		if (chunk_size > available - payload)
		{
			return false;
		}

		if (std::memcmp(data + position, "fmt ", 4) == 0)
		{
			if (chunk_size < 16)
			{
				return false;
			}
			const std::uint16_t tag = sl_open::io::read_le16(data + payload);
			if (tag != 1 && tag != 0x11)
			{
				return false;
			}
			info.encoding = tag == 1 ? WavEncoding::Pcm : WavEncoding::ImaAdpcm;
			info.channels = sl_open::io::read_le16(data + payload + 2);
			info.sample_rate = sl_open::io::read_le32(data + payload + 4);
			info.block_align = sl_open::io::read_le16(data + payload + 12);
			info.bits_per_sample = sl_open::io::read_le16(data + payload + 14);
			if (info.encoding == WavEncoding::ImaAdpcm)
			{
				if (chunk_size < 20
					|| sl_open::io::read_le16(data + payload + 16) < 2)
				{
					return false;
				}
				info.samples_per_block = sl_open::io::read_le16(data + payload + 18);
			}
			have_format = true;
		}
		else if (std::memcmp(data + position, "fact", 4) == 0 && chunk_size >= 4)
		{
			fact_frames = sl_open::io::read_le32(data + payload);
		}
		position = payload + chunk_size + (chunk_size & 1);
	}

	if (!have_format || !have_data || info.channels < 1 || info.channels > 2
		|| info.sample_rate == 0 || info.sample_rate > INT_MAX
		|| info.block_align == 0)
	{
		return false;
	}
	if (info.encoding == WavEncoding::Pcm)
	{
		if ((info.bits_per_sample != 8 && info.bits_per_sample != 16)
			|| info.block_align != info.channels * info.bits_per_sample / 8)
		{
			return false;
		}
		info.total_frames = info.data_size / info.block_align;
	}
	else
	{
		if (info.bits_per_sample != 4 || info.samples_per_block == 0
			|| info.block_align < info.channels * 4)
		{
			return false;
		}
		const std::uint64_t blocks =
			(info.data_size + info.block_align - 1) / info.block_align;
		const std::uint64_t derived = blocks * info.samples_per_block;
		if (derived > UINT32_MAX)
		{
			return false;
		}
		info.total_frames = fact_frames != 0
			? fact_frames
			: static_cast<std::uint32_t>(derived);
	}
	return info.total_frames != 0;
}

bool decode_wav(const std::uint8_t* data, std::size_t size, Pcm& pcm)
{
	pcm = {};
	WavInfo info;
	if (!parse_wav(data, size, size, info)
		|| static_cast<std::uint64_t>(info.total_frames) * info.channels
			> SIZE_MAX / sizeof(std::int16_t)
		|| !pcm.samples.allocate(
			static_cast<std::size_t>(info.total_frames) * info.channels
				* sizeof(std::int16_t)))
	{
		return false;
	}

	if (!decode_wav_into(
			data,
			size,
			reinterpret_cast<std::int16_t*>(pcm.samples.data),
			static_cast<std::size_t>(info.total_frames) * info.channels,
			info))
	{
		pcm = {};
		return false;
	}
	pcm.sample_rate = info.sample_rate;
	pcm.frame_count = info.total_frames;
	pcm.channels = info.channels;
	return true;
}

bool decode_wav_into(
	const std::uint8_t* data,
	std::size_t size,
	std::int16_t* samples,
	std::size_t sample_capacity,
	WavInfo& info)
{
	if (!parse_wav(data, size, size, info))
	{
		return false;
	}

	const std::uint64_t required =
		static_cast<std::uint64_t>(info.total_frames) * info.channels;
	if (samples == nullptr || required > sample_capacity)
	{
		return false;
	}

	std::uint32_t frames = 0;
	return decode_chunk(
		info,
		data + info.data_offset,
		info.data_size,
		samples,
		info.total_frames,
		frames)
		&& frames == info.total_frames;
}

bool wav_stream_open(
	sl_open::io::Vfs& vfs,
	const char* path,
	ALuint source,
	const ALuint* buffers,
	bool loop,
	WavStream& stream)
{
	return wav_stream_open_looped(
		vfs,
		path,
		source,
		buffers,
		loop ? 0 : 1,
		0,
		stream);
}

bool wav_stream_open_looped(
	const WavAsset& asset,
	ALuint source,
	const ALuint* buffers,
	std::int32_t loop_count,
	std::uint32_t loop_byte_offset,
	WavStream& stream)
{
	sl_open::io::Vfs unused;
	wav_stream_close(unused, stream);
	if (source == 0 || asset.file.size == 0)
	{
		return false;
	}

	stream.info = asset.info;
	stream.memory = asset.file.data;
	stream.memory_size = asset.file.size;
	stream.source = source;
	std::memcpy(stream.free_buffers, buffers, sizeof(stream.free_buffers));
	stream.free_count = 4;
	stream.encoded_offset = stream.info.data_offset;
	stream.encoded_remaining = stream.info.data_size;
	stream.frames_remaining = stream.info.total_frames;
	stream.loop_count = loop_count;
	stream.loop_byte_offset = loop_byte_offset;
	stream.active = true;

	alSourceStop(source);
	alSourcei(source, AL_BUFFER, 0);
	alSourcei(source, AL_SOURCE_RELATIVE, AL_TRUE);
	alSource3f(source, AL_POSITION, 0.0f, 0.0f, 0.0f);
	alSourcef(source, AL_GAIN, 1.0f);
	return wav_stream_update(unused, stream);
}

bool wav_stream_open_looped(
	sl_open::io::Vfs& vfs,
	const char* path,
	ALuint source,
	const ALuint* buffers,
	std::int32_t loop_count,
	std::uint32_t loop_byte_offset,
	WavStream& stream)
{
	wav_stream_close(vfs, stream);
	if (source == 0 || !sl_open::io::vfs_open(vfs, path, stream.file))
	{
		return false;
	}

	const std::size_t header_size = std::min<std::uint64_t>(
		stream.file.size, kWavHeaderBytes);
	sl_open::io::ReadRequest request;
	if (sl_open::io::vfs_read_at(
			vfs, stream.file, 0, stream.header, header_size, request)
			!= sl_open::io::IoStatus::complete
		|| !parse_wav(stream.header, header_size, stream.file.size, stream.info)
		|| stream.info.block_align > kWavEncodedBytes)
	{
		wav_stream_close(vfs, stream);
		return false;
	}

	stream.source = source;
	std::memcpy(stream.free_buffers, buffers, sizeof(stream.free_buffers));
	stream.free_count = 4;
	stream.encoded_offset = stream.info.data_offset;
	stream.encoded_remaining = stream.info.data_size;
	stream.frames_remaining = stream.info.total_frames;
	stream.loop_count = loop_count;
	stream.loop_byte_offset = loop_byte_offset;
	stream.active = true;

	alSourceStop(source);
	alSourcei(source, AL_BUFFER, 0);
	alSourcei(source, AL_SOURCE_RELATIVE, AL_TRUE);
	alSource3f(source, AL_POSITION, 0.0f, 0.0f, 0.0f);
	alSourcef(source, AL_GAIN, 1.0f);
	return wav_stream_update(vfs, stream);
}

bool wav_stream_update(sl_open::io::Vfs& vfs, WavStream& stream)
{
	if (!stream.active)
	{
		return false;
	}

	ALint processed = 0;
	alGetSourcei(stream.source, AL_BUFFERS_PROCESSED, &processed);
	while (processed-- > 0)
	{
		if (stream.free_count >= 4)
		{
			wav_stream_close(vfs, stream);
			return false;
		}
		alSourceUnqueueBuffers(
			stream.source, 1, &stream.free_buffers[stream.free_count++]);
	}

	if (stream.read_pending)
	{
		sl_open::io::vfs_poll(vfs, stream.request);
		if (stream.request.status == sl_open::io::IoStatus::pending)
		{
			return true;
		}
		if (!finish_stream_read(vfs, stream))
		{
			return false;
		}
	}

	for (std::uint32_t fill = 0; fill < 4 && stream.free_count > 0; ++fill)
	{
		if (stream.encoded_remaining == 0 || stream.frames_remaining == 0)
		{
			if (stream.loop_count == 1)
			{
				break;
			}
			if (stream.loop_count > 1)
			{
				--stream.loop_count;
			}
			const std::uint32_t data_begin = stream.info.data_offset;
			const std::uint32_t data_end =
				stream.info.data_offset + stream.info.data_size;
			// Miles retains the WAVE data start separately from the stream
			// sub-block start. AIL_set_stream_loop_block offsets are
			// therefore relative to the beginning of decoded sound data,
			// not absolute RIFF file positions.
			const std::uint32_t relative = std::min(
				stream.loop_byte_offset, stream.info.data_size);
			if (stream.info.encoding == WavEncoding::ImaAdpcm)
			{
				const std::uint32_t block =
					relative / stream.info.block_align;
				const std::uint32_t in_block =
					relative % stream.info.block_align;
				stream.encoded_offset =
					data_begin + block * stream.info.block_align;
				const std::uint32_t header =
					stream.info.channels * 4;
				if (in_block < header)
				{
					stream.loop_skip_frames = 0;
				}
				else if (stream.info.channels == 1)
				{
					stream.loop_skip_frames =
						1 + (in_block - header) * 2;
				}
				else
				{
					stream.loop_skip_frames =
						1 + ((in_block - header) / 8) * 8;
				}
				stream.loop_skip_frames = std::min<std::uint32_t>(
					stream.loop_skip_frames,
					stream.info.samples_per_block);
				const std::uint64_t first_frame =
					static_cast<std::uint64_t>(block)
						* stream.info.samples_per_block
					+ stream.loop_skip_frames;
				stream.frames_remaining = first_frame
						>= stream.info.total_frames
					? 0
					: stream.info.total_frames
						- static_cast<std::uint32_t>(first_frame);
			}
			else
			{
				const std::uint32_t bytes_per_frame =
					stream.info.block_align;
				stream.encoded_offset = data_begin
					+ (relative / bytes_per_frame)
						* bytes_per_frame;
				const std::uint32_t first_frame =
					(stream.encoded_offset - data_begin)
						/ bytes_per_frame;
				stream.frames_remaining =
					stream.info.total_frames - first_frame;
			}
			stream.encoded_remaining =
				data_end - static_cast<std::uint32_t>(
					stream.encoded_offset);
		}

		stream.read_size = next_read_size(stream);
		stream.request = {};
		if (stream.memory != nullptr)
		{
			if (stream.encoded_offset > stream.memory_size
				|| stream.read_size
					> stream.memory_size - stream.encoded_offset)
			{
				wav_stream_close(vfs, stream);
				return false;
			}
			std::memcpy(
				stream.encoded,
				stream.memory + stream.encoded_offset,
				stream.read_size);
			stream.request.status = sl_open::io::IoStatus::complete;
			stream.request.transferred = stream.read_size;
			if (!finish_stream_read(vfs, stream))
			{
				return false;
			}
			continue;
		}
		const sl_open::io::IoStatus status = sl_open::io::vfs_read_at(
			vfs,
			stream.file,
			stream.encoded_offset,
			stream.encoded,
			stream.read_size,
			stream.request);
		if (status == sl_open::io::IoStatus::pending)
		{
			stream.read_pending = true;
			break;
		}
		if (status != sl_open::io::IoStatus::complete
			|| !finish_stream_read(vfs, stream))
		{
			return false;
		}
	}

	ALint queued = 0;
	ALint state = 0;
	alGetSourcei(stream.source, AL_BUFFERS_QUEUED, &queued);
	alGetSourcei(stream.source, AL_SOURCE_STATE, &state);
	if (queued > 0 && state != AL_PLAYING && !stream.paused)
	{
		alSourcePlay(stream.source);
	}
	else if (queued == 0 && stream.loop_count == 1
		&& stream.encoded_remaining == 0)
	{
		wav_stream_close(vfs, stream);
		return false;
	}
	return true;
}

void wav_stream_pause(WavStream& stream)
{
	if (stream.active && !stream.paused)
	{
		alSourcePause(stream.source);
		stream.paused = true;
	}
}

void wav_stream_resume(WavStream& stream)
{
	if (stream.active && stream.paused)
	{
		stream.paused = false;
		ALint queued = 0;
		alGetSourcei(stream.source, AL_BUFFERS_QUEUED, &queued);
		if (queued > 0)
		{
			alSourcePlay(stream.source);
		}
	}
}

void wav_stream_close(sl_open::io::Vfs& vfs, WavStream& stream)
{
	if (stream.source != 0)
	{
		alSourceStop(stream.source);
		ALint queued = 0;
		alGetSourcei(stream.source, AL_BUFFERS_QUEUED, &queued);
		while (queued-- > 0)
		{
			ALuint buffer = 0;
			alSourceUnqueueBuffers(stream.source, 1, &buffer);
		}
	}
	sl_open::io::vfs_close(vfs, stream.file);
	stream = {};
}
}

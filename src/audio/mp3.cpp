#define DR_MP3_IMPLEMENTATION
#include "audio/mp3.hpp"

#include <cstring>

namespace sl_open::audio
{
namespace
{
bool initialize_decoder(sl_open::io::Vfs& vfs, Mp3Stream& stream)
{
	if (stream.request.status != sl_open::io::IoStatus::complete
		|| stream.request.transferred != stream.encoded.size
		|| !drmp3_init_memory(
			&stream.decoder,
			stream.encoded.data,
			stream.encoded.size,
			nullptr)
		|| stream.decoder.channels < 1 || stream.decoder.channels > 2
		|| stream.decoder.sampleRate == 0)
	{
		mp3_stream_close(vfs, stream);
		return false;
	}
	stream.decoder_ready = true;
	stream.read_pending = false;
	stream.request = {};
	return true;
}

bool queue_decoded_buffer(sl_open::io::Vfs& vfs, Mp3Stream& stream)
{
	drmp3_uint64 frames = drmp3_read_pcm_frames_s16(
		&stream.decoder, kMp3PcmFrames, stream.pcm);
	if (frames == 0 && stream.loop)
	{
		if (!drmp3_seek_to_pcm_frame(&stream.decoder, 0))
		{
			mp3_stream_close(vfs, stream);
			return false;
		}
		frames = drmp3_read_pcm_frames_s16(
			&stream.decoder, kMp3PcmFrames, stream.pcm);
	}
	if (frames == 0)
	{
		return true;
	}

	const ALuint buffer = stream.free_buffers[--stream.free_count];
	const ALenum format =
		stream.decoder.channels == 1 ? AL_FORMAT_MONO16 : AL_FORMAT_STEREO16;
	while (alGetError() != AL_NO_ERROR)
	{
	}
	alBufferData(
		buffer,
		format,
		stream.pcm,
		static_cast<ALsizei>(
			frames * stream.decoder.channels * sizeof(std::int16_t)),
		static_cast<ALsizei>(stream.decoder.sampleRate));
	alSourceQueueBuffers(stream.source, 1, &buffer);
	if (alGetError() != AL_NO_ERROR)
	{
		mp3_stream_close(vfs, stream);
		return false;
	}
	return true;
}
}

bool mp3_stream_open(
	sl_open::io::Vfs& vfs,
	const char* path,
	ALuint source,
	const ALuint* buffers,
	bool loop,
	Mp3Stream& stream)
{
	mp3_stream_close(vfs, stream);
	if (source == 0 || !sl_open::io::vfs_open(vfs, path, stream.file)
		|| stream.file.size == 0 || stream.file.size > SIZE_MAX
		|| !stream.encoded.allocate(static_cast<std::size_t>(stream.file.size)))
	{
		mp3_stream_close(vfs, stream);
		return false;
	}

	stream.source = source;
	std::memcpy(stream.free_buffers, buffers, sizeof(stream.free_buffers));
	stream.free_count = 4;
	stream.loop = loop;
	stream.active = true;

	alSourceStop(source);
	alSourcei(source, AL_BUFFER, 0);
	alSourcei(source, AL_SOURCE_RELATIVE, AL_TRUE);
	alSource3f(source, AL_POSITION, 0.0f, 0.0f, 0.0f);
	alSourcef(source, AL_GAIN, 1.0f);

	const sl_open::io::IoStatus status = sl_open::io::vfs_read_at(
		vfs,
		stream.file,
		0,
		stream.encoded.data,
		stream.encoded.size,
		stream.request);
	if (status == sl_open::io::IoStatus::pending)
	{
		stream.read_pending = true;
		return true;
	}
	if (status != sl_open::io::IoStatus::complete || !initialize_decoder(vfs, stream))
	{
		return false;
	}
	return mp3_stream_update(vfs, stream);
}

bool mp3_stream_update(sl_open::io::Vfs& vfs, Mp3Stream& stream)
{
	if (!stream.active)
	{
		return false;
	}
	if (stream.read_pending)
	{
		sl_open::io::vfs_poll(vfs, stream.request);
		if (stream.request.status == sl_open::io::IoStatus::pending)
		{
			return true;
		}
		if (!initialize_decoder(vfs, stream))
		{
			return false;
		}
	}

	ALint processed = 0;
	alGetSourcei(stream.source, AL_BUFFERS_PROCESSED, &processed);
	while (processed-- > 0)
	{
		if (stream.free_count >= 4)
		{
			mp3_stream_close(vfs, stream);
			return false;
		}
		alSourceUnqueueBuffers(
			stream.source, 1, &stream.free_buffers[stream.free_count++]);
	}

	for (std::uint32_t fill = 0; fill < 4 && stream.free_count > 0; ++fill)
	{
		const drmp3_uint64 before = stream.decoder.currentPCMFrame;
		if (!queue_decoded_buffer(vfs, stream))
		{
			return false;
		}
		if (stream.decoder.currentPCMFrame == before)
		{
			break;
		}
	}

	ALint queued = 0;
	ALint state = 0;
	alGetSourcei(stream.source, AL_BUFFERS_QUEUED, &queued);
	alGetSourcei(stream.source, AL_SOURCE_STATE, &state);
	if (queued > 0 && state != AL_PLAYING)
	{
		alSourcePlay(stream.source);
	}
	else if (queued == 0 && !stream.loop)
	{
		mp3_stream_close(vfs, stream);
		return false;
	}
	return true;
}

void mp3_stream_close(sl_open::io::Vfs& vfs, Mp3Stream& stream)
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
	if (stream.decoder_ready)
	{
		drmp3_uninit(&stream.decoder);
	}
	sl_open::io::vfs_close(vfs, stream.file);
	stream = {};
}
}

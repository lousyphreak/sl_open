#include "audio/cb97_stream.hpp"

#include <cctype>
#include <cstring>

namespace sl_open::audio
{
namespace
{
bool asset_path(const char* path, char (&output)[128])
{
	const std::size_t length = path == nullptr ? 0 : std::strlen(path);
	if (length == 0 || length >= sizeof(output))
	{
		return false;
	}
	std::memcpy(output, path, length + 1);
	char* const extension = std::strchr(output, '.');
	if (extension != nullptr
		&& std::strncmp(extension + 1, "ut", 2) == 0)
	{
		*extension = '\0';
	}
	return true;
}

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

const char* basename(const char* path)
{
	const char* result = path;
	for (; *path != '\0'; ++path)
	{
		if (*path == '/' || *path == '\\')
		{
			result = path + 1;
		}
	}
	return result;
}

bool initialize_decoder(sl_open::io::Vfs& vfs, Cb97Stream& stream)
{
	if (stream.request.status != sl_open::io::IoStatus::complete
		|| stream.request.transferred != stream.encoded_size
		|| !cb97_init(
			stream.decoder, stream.encoded_data, stream.encoded_size))
	{
		cb97_stream_close(vfs, stream);
		return false;
	}
	stream.decoder_ready = true;
	stream.read_pending = false;
	stream.request = {};
	return true;
}

bool queue_buffer(
	sl_open::io::Vfs& vfs,
	Cb97Stream& stream,
	std::uint32_t& decoded_samples)
{
	decoded_samples = cb97_read(
		stream.decoder,
		stream.pcm,
		kCb97PcmBufferSamples,
		stream.loop);
	if (decoded_samples == 0)
	{
		return true;
	}

	const ALuint buffer = stream.free_buffers[--stream.free_count];
	while (alGetError() != AL_NO_ERROR)
	{
	}
	alBufferData(
		buffer,
		AL_FORMAT_MONO16,
		stream.pcm,
		static_cast<ALsizei>(decoded_samples * sizeof(std::int16_t)),
		kCb97SampleRate);
	alSourceQueueBuffers(stream.source, 1, &buffer);
	if (alGetError() != AL_NO_ERROR)
	{
		cb97_stream_close(vfs, stream);
		return false;
	}
	return true;
}
}

bool cb97_asset_load(
	sl_open::io::Vfs& vfs,
	const char* path,
	Cb97Asset& asset)
{
	asset = {};
	if (!asset_path(path, asset.path)
		|| !sl_open::io::vfs_read_all(vfs, asset.path, asset.encoded))
	{
		asset = {};
		return false;
	}
	return true;
}

bool cb97_asset_matches(const Cb97Asset& asset, const char* path)
{
	char normalized[128];
	return asset_path(path, normalized)
		&& (path_equal(asset.path, normalized)
			|| path_equal(basename(asset.path), basename(normalized)));
}

bool cb97_stream_open(
	const Cb97Asset& asset,
	ALuint source,
	const ALuint* buffers,
	bool loop,
	Cb97Stream& stream,
	float gain)
{
	sl_open::io::Vfs unused;
	cb97_stream_close(unused, stream);
	if (source == 0 || asset.encoded.size == 0)
	{
		return false;
	}

	stream.encoded_data = asset.encoded.data;
	stream.encoded_size = asset.encoded.size;
	stream.source = source;
	std::memcpy(stream.free_buffers, buffers, sizeof(stream.free_buffers));
	stream.free_count = 4;
	stream.loop = loop;
	stream.active = true;
	stream.paused = false;

	alSourceStop(source);
	alSourcei(source, AL_BUFFER, 0);
	alSourcei(source, AL_SOURCE_RELATIVE, AL_TRUE);
	alSource3f(source, AL_POSITION, 0.0f, 0.0f, 0.0f);
	alSourcef(source, AL_GAIN, gain);

	stream.request.status = sl_open::io::IoStatus::complete;
	stream.request.transferred = stream.encoded_size;
	if (!initialize_decoder(unused, stream))
	{
		return false;
	}
	return cb97_stream_update(unused, stream);
}

bool cb97_stream_open(
	sl_open::io::Vfs& vfs,
	const char* path,
	ALuint source,
	const ALuint* buffers,
	bool loop,
	Cb97Stream& stream,
	float gain)
{
	cb97_stream_close(vfs, stream);
	// HOG_bigread2 (LANCER.EXE 0x004c7f60), used by PlaySpeech at
	// 0x00458090, copies the requested name to a 128-byte work buffer and
	// removes a lower-case ".ut" beginning at its first dot before either
	// archive or loose-file lookup. Speech HOG entries consequently have no
	// extension even though mission DTE strings carry one.
	char normalized[128];
	if (!asset_path(path, normalized))
	{
		return false;
	}

	if (source == 0 || !sl_open::io::vfs_open(vfs, normalized, stream.file)
		|| stream.file.size == 0 || stream.file.size > SIZE_MAX
		|| !stream.encoded.allocate(static_cast<std::size_t>(stream.file.size)))
	{
		cb97_stream_close(vfs, stream);
		return false;
	}

	stream.source = source;
	std::memcpy(stream.free_buffers, buffers, sizeof(stream.free_buffers));
	stream.free_count = 4;
	stream.loop = loop;
	stream.active = true;
	stream.paused = false;
	stream.encoded_data = stream.encoded.data;
	stream.encoded_size = stream.encoded.size;

	alSourceStop(source);
	alSourcei(source, AL_BUFFER, 0);
	alSourcei(source, AL_SOURCE_RELATIVE, AL_TRUE);
	alSource3f(source, AL_POSITION, 0.0f, 0.0f, 0.0f);
	alSourcef(source, AL_GAIN, gain);

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
	return cb97_stream_update(vfs, stream);
}

bool cb97_stream_update(sl_open::io::Vfs& vfs, Cb97Stream& stream)
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
			cb97_stream_close(vfs, stream);
			return false;
		}
		alSourceUnqueueBuffers(
			stream.source, 1, &stream.free_buffers[stream.free_count++]);
	}

	for (std::uint32_t fill = 0; fill < 4 && stream.free_count > 0; ++fill)
	{
		std::uint32_t decoded_samples = 0;
		if (!queue_buffer(vfs, stream, decoded_samples))
		{
			return false;
		}
		if (decoded_samples == 0)
		{
			break;
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
	else if (queued == 0 && !stream.loop)
	{
		cb97_stream_close(vfs, stream);
		return false;
	}
	return true;
}

void cb97_stream_pause(Cb97Stream& stream)
{
	if (!stream.active || stream.paused)
	{
		return;
	}
	stream.paused = true;
	if (stream.source != 0)
	{
		alSourcePause(stream.source);
	}
}

void cb97_stream_resume(Cb97Stream& stream)
{
	if (!stream.active || !stream.paused)
	{
		return;
	}
	stream.paused = false;
	if (stream.source == 0)
	{
		return;
	}
	ALint queued = 0;
	alGetSourcei(stream.source, AL_BUFFERS_QUEUED, &queued);
	if (queued > 0)
	{
		alSourcePlay(stream.source);
	}
}

void cb97_stream_close(sl_open::io::Vfs& vfs, Cb97Stream& stream)
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

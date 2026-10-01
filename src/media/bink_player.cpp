#include "media/bink_player.hpp"

#include "io/endian.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace sl_open::media
{
namespace
{
constexpr std::uint64_t kAudioStallToleranceMilliseconds = 1000;

void clear_al_error()
{
	while (alGetError() != AL_NO_ERROR)
	{
	}
}

void release_audio(BinkMovie& movie)
{
	if (movie.source == 0)
	{
		return;
	}
	alSourceStop(movie.source);
	ALint queued = 0;
	alGetSourcei(movie.source, AL_BUFFERS_QUEUED, &queued);
	while (queued-- > 0)
	{
		ALuint buffer = 0;
		alSourceUnqueueBuffers(movie.source, 1, &buffer);
	}
}

void collect_audio_buffers(BinkMovie& movie)
{
	if (!movie.audio_enabled)
	{
		return;
	}
	ALint processed = 0;
	alGetSourcei(movie.source, AL_BUFFERS_PROCESSED, &processed);
	while (processed-- > 0
		&& movie.free_count < audio::kStreamBufferCount)
	{
		alSourceUnqueueBuffers(
			movie.source, 1, &movie.free_buffers[movie.free_count]);
		++movie.free_count;
	}
}

bool queue_audio(
	const std::uint8_t* packet,
	std::uint32_t packet_size,
	BinkMovie& movie)
{
	if (packet_size == 0)
	{
		return true;
	}
	if (movie.free_count == 0
		|| !bink_audio_packet_begin(movie.audio, packet, packet_size))
	{
		return false;
	}

	std::size_t sample_count = 0;
	bool finished = false;
	while (!finished)
	{
		std::uint32_t frames = 0;
		const std::size_t remaining_samples =
			sizeof(movie.pcm) / sizeof(movie.pcm[0]) - sample_count;
		if (remaining_samples / movie.audio.channels == 0
			|| !bink_audio_decode_block(
				movie.audio,
				movie.pcm + sample_count,
				remaining_samples / movie.audio.channels,
				frames,
				finished))
		{
			return false;
		}
		sample_count +=
			static_cast<std::size_t>(frames) * movie.audio.channels;
	}

	const ALuint buffer = movie.free_buffers[--movie.free_count];
	const ALenum format =
		movie.audio.channels == 1 ? AL_FORMAT_MONO16 : AL_FORMAT_STEREO16;
	clear_al_error();
	alBufferData(
		buffer,
		format,
		movie.pcm,
		static_cast<ALsizei>(sample_count * sizeof(std::int16_t)),
		static_cast<ALsizei>(movie.audio.sample_rate));
	alSourceQueueBuffers(movie.source, 1, &buffer);
	if (alGetError() != AL_NO_ERROR)
	{
		return false;
	}
	return true;
}

bool start_audio_if_needed(BinkMovie& movie)
{
	if (!movie.audio_enabled)
	{
		return true;
	}
	ALint playing = AL_STOPPED;
	alGetSourcei(movie.source, AL_SOURCE_STATE, &playing);
	if (playing != AL_PLAYING)
	{
		alSourcePlay(movie.source);
	}
	return alGetError() == AL_NO_ERROR;
}

std::uint8_t clamp_color(int value)
{
	return static_cast<std::uint8_t>(std::clamp(value, 0, 255));
}

void convert_frame(const BinkVideoFrame& frame, std::uint8_t* rgba)
{
	for (std::uint32_t y = 0; y < frame.height; ++y)
	{
		const std::uint8_t* luma =
			frame.planes[0] + y * frame.strides[0];
		const std::uint8_t* blue =
			frame.planes[1] + (y / 2) * frame.strides[1];
		const std::uint8_t* red =
			frame.planes[2] + (y / 2) * frame.strides[2];
		for (std::uint32_t x = 0; x < frame.width; ++x)
		{
			const int c = std::max(0, static_cast<int>(luma[x]) - 16);
			const int d = static_cast<int>(blue[x / 2]) - 128;
			const int e = static_cast<int>(red[x / 2]) - 128;
			*rgba++ = clamp_color((298 * c + 409 * e + 128) >> 8);
			*rgba++ = clamp_color((298 * c - 100 * d - 208 * e + 128) >> 8);
			*rgba++ = clamp_color((298 * c + 516 * d + 128) >> 8);
			*rgba++ = 255;
		}
	}
}

void apply_chroma_key(
	std::uint8_t* rgba,
	std::uint32_t width,
	std::uint32_t height)
{
	constexpr float key_red = 236.0f;
	constexpr float key_green = 190.0f;
	constexpr float key_blue = 0.0f;
	constexpr float transparent_distance = 80.0f;
	constexpr float opaque_distance = 115.0f;
	for (std::uint32_t index = 0; index < width * height; ++index)
	{
		const float red = rgba[0] - key_red;
		const float green = rgba[1] - key_green;
		const float blue = rgba[2] - key_blue;
		const float distance =
			std::sqrt(red * red + green * green + blue * blue);
		const float alpha = std::clamp(
			(distance - transparent_distance)
				/ (opaque_distance - transparent_distance),
			0.0f,
			1.0f);
		rgba[3] = static_cast<std::uint8_t>(alpha * 255.0f + 0.5f);
		rgba += 4;
	}
}

bool begin_frame_read(io::Vfs& vfs, BinkMovie& movie)
{
	if (movie.frame_index >= movie.file.info.frame_count)
	{
		movie.state = BinkMovieState::waiting;
		return true;
	}
	movie.read_size = movie.file.frames[movie.frame_index].size;
	movie.frame_request = {};
	const io::IoStatus status = bink_read_frame(
		vfs,
		movie.file,
		movie.frame_index,
		movie.encoded,
		sizeof(movie.encoded),
		movie.frame_request);
	if (status == io::IoStatus::failed)
	{
		movie.state = BinkMovieState::failed;
		return false;
	}
	movie.state = BinkMovieState::reading;
	return true;
}

bool begin_seek_read(io::Vfs& vfs, BinkMovie& movie)
{
	if (movie.frame_index >= movie.file.info.frame_count)
	{
		movie.state = BinkMovieState::finished;
		movie.skipping_to_end = false;
		return true;
	}

	movie.seek_offset = movie.file.frames[movie.frame_index].offset;
	const std::uint32_t seek_size =
		movie.file.info.file_size - movie.seek_offset;
	if (!movie.seek_data.allocate(seek_size))
	{
		movie.state = BinkMovieState::failed;
		return false;
	}
	movie.seek_request = {};
	const io::IoStatus status = io::vfs_read_at(
		vfs,
		movie.file.file,
		movie.seek_offset,
		movie.seek_data.data,
		seek_size,
		movie.seek_request);
	if (status == io::IoStatus::failed)
	{
		movie.seek_data.reset();
		movie.state = BinkMovieState::failed;
		return false;
	}
	movie.state = BinkMovieState::reading;
	return true;
}

bool decode_frame_packet(
	BinkMovie& movie,
	const std::uint8_t* encoded,
	std::uint32_t encoded_size,
	bool present)
{
	const std::uint8_t* video = encoded;
	std::uint32_t video_size = encoded_size;
	if (movie.has_audio)
	{
		if (video_size < 4)
		{
			return false;
		}
		const std::uint32_t audio_size = io::read_le32(video);
		if (audio_size > video_size - 4)
		{
			return false;
		}
		if (movie.audio_enabled
			&& !queue_audio(video + 4, audio_size, movie))
		{
			return false;
		}
		video += 4 + audio_size;
		video_size -= 4 + audio_size;
	}
	if (video_size == 0
		|| !bink_video_decode(movie.video, video, video_size))
	{
		return false;
	}
	++movie.frame_index;
	if (!present)
	{
		return true;
	}

	convert_frame(movie.video.frame, movie.rgba);
	if (movie.chroma_key)
	{
		apply_chroma_key(
			movie.rgba,
			movie.video.frame.width,
			movie.video.frame.height);
	}
	if (!bgfx::isValid(movie.texture.handle)
		&& bgfx::isValid(movie.held_texture.handle)
		&& movie.held_texture.width == movie.video.frame.width
		&& movie.held_texture.height == movie.video.frame.height)
	{
		movie.texture =
			static_cast<render::FrontendTexture&&>(movie.held_texture);
	}
	if (!render::frontend_movie_texture_init(
			movie.texture,
			static_cast<std::uint16_t>(movie.video.frame.width),
			static_cast<std::uint16_t>(movie.video.frame.height)))
	{
		return false;
	}
	render::frontend_movie_texture_update(movie.texture, movie.rgba);
	render::frontend_texture_shutdown(movie.held_texture);
	movie.frame_visible = true;
	if (!start_audio_if_needed(movie))
	{
		return false;
	}
	return true;
}

bool finish_open(io::Vfs& vfs, std::uint64_t now, BinkMovie& movie)
{
	if (!bink_video_init(
			movie.video,
			movie.file.info.width,
			movie.file.info.height,
			movie.file.info.tag,
			movie.file.info.video_flags))
	{
		return false;
	}
	movie.has_audio = movie.file.info.has_audio;
	movie.audio_enabled =
		movie.has_audio && movie.source != 0 && !movie.skipping_to_end;
	if (movie.audio_enabled
		&& !bink_audio_init(
			movie.audio,
				movie.file.info.audio.sample_rate,
				movie.file.info.audio.channels))
	{
		return false;
	}
	movie.frame_index = 0;
	movie.started_at = now;
	movie.next_frame_at = now;
	return movie.skipping_to_end
		? begin_seek_read(vfs, movie)
		: begin_frame_read(vfs, movie);
}

bool restart_loop(io::Vfs& vfs, BinkMovie& movie)
{
	if (!bink_video_init(
			movie.video,
			movie.file.info.width,
			movie.file.info.height,
			movie.file.info.tag,
			movie.file.info.video_flags))
	{
		return false;
	}
	if (movie.audio_enabled
		&& !bink_audio_init(
			movie.audio,
			movie.file.info.audio.sample_rate,
			movie.file.info.audio.channels))
	{
		return false;
	}
	movie.frame_index = 0;
	movie.started_at = movie.next_frame_at;
	return begin_frame_read(vfs, movie);
}
}

bool bink_movie_open(
	io::Vfs& vfs,
	const char* path,
	ALuint source,
	const ALuint* buffers,
	std::uint64_t now,
	BinkMovie& movie)
{
	bink_movie_stop(vfs, movie);
	movie.source = source;
	movie.free_count = audio::kStreamBufferCount;
	for (std::uint32_t index = 0;
		index < audio::kStreamBufferCount;
		++index)
	{
		movie.free_buffers[index] = buffers[index];
	}
	release_audio(movie);
	const BinkOpenStatus status = bink_open(vfs, path, movie.file);
	if (status == BinkOpenStatus::failed)
	{
		movie.state = BinkMovieState::failed;
		return false;
	}
	movie.next_frame_at = now;
	movie.state = BinkMovieState::opening;
	return true;
}

BinkMovieState bink_movie_update(
	io::Vfs& vfs,
	std::uint64_t now,
	BinkMovie& movie)
{
	if (movie.paused)
	{
		return movie.state;
	}
	collect_audio_buffers(movie);
	if (movie.state == BinkMovieState::opening)
	{
		const BinkOpenStatus status = bink_open_update(vfs, movie.file);
		if (status == BinkOpenStatus::failed)
		{
			movie.state = BinkMovieState::failed;
		}
		else if (status == BinkOpenStatus::ready
			&& !finish_open(vfs, now, movie))
		{
			movie.state = BinkMovieState::failed;
		}
		if (movie.state != BinkMovieState::reading
			|| movie.frame_request.status == io::IoStatus::pending)
		{
			return movie.state;
		}
	}

	if (movie.skipping_to_end)
	{
		if (movie.seek_request.status == io::IoStatus::pending)
		{
			io::vfs_poll(vfs, movie.seek_request);
		}
		if (movie.seek_request.status == io::IoStatus::pending)
		{
			return movie.state;
		}
		const std::size_t expected =
			movie.file.info.file_size - movie.seek_offset;
		if (movie.seek_request.status != io::IoStatus::complete
			|| movie.seek_request.transferred != expected)
		{
			movie.seek_data.reset();
			movie.state = BinkMovieState::failed;
			return movie.state;
		}

		while (movie.frame_index < movie.file.info.frame_count)
		{
			const BinkFrame& frame = movie.file.frames[movie.frame_index];
			const bool present =
				movie.frame_index + 1 >= movie.file.info.frame_count;
			if (!decode_frame_packet(
					movie,
					movie.seek_data.data + frame.offset - movie.seek_offset,
					frame.size,
					present))
			{
				movie.seek_data.reset();
				movie.state = BinkMovieState::failed;
				return movie.state;
			}
		}
		movie.seek_data.reset();
		movie.seek_request = {};
		movie.state = BinkMovieState::finished;
		movie.skipping_to_end = false;
		return movie.state;
	}

	while (true)
	{
		if (movie.state == BinkMovieState::reading)
		{
			if (movie.frame_request.status == io::IoStatus::pending)
			{
				io::vfs_poll(vfs, movie.frame_request);
			}
			if (movie.frame_request.status == io::IoStatus::pending)
			{
				return movie.state;
			}
			if (movie.frame_request.status != io::IoStatus::complete
				|| movie.frame_request.transferred != movie.read_size)
			{
				movie.state = BinkMovieState::failed;
				return movie.state;
			}
			movie.state = BinkMovieState::waiting;
		}

		if (movie.state != BinkMovieState::waiting
			|| (!movie.skipping_to_end && now < movie.next_frame_at))
		{
			return movie.state;
		}
		if (movie.frame_index >= movie.file.info.frame_count)
		{
			if (movie.looping)
			{
				if (!restart_loop(vfs, movie))
				{
					movie.state = BinkMovieState::failed;
				}
				return movie.state;
			}
			if (movie.audio_enabled
				&& movie.free_count != audio::kStreamBufferCount)
			{
				return movie.state;
			}
			movie.state = BinkMovieState::finished;
			movie.skipping_to_end = false;
			return movie.state;
		}
		if (movie.audio_enabled && movie.free_count == 0)
		{
			if (now <= movie.next_frame_at
				+ kAudioStallToleranceMilliseconds)
			{
				return movie.state;
			}
			release_audio(movie);
			movie.audio_enabled = false;
		}

		if (!decode_frame_packet(
				movie, movie.encoded, movie.read_size, true))
		{
			movie.state = BinkMovieState::failed;
			return movie.state;
		}
		movie.next_frame_at =
			movie.started_at
			+ static_cast<std::uint64_t>(movie.frame_index)
				* movie.file.info.fps_denominator * 1000
				/ movie.file.info.fps_numerator;
		begin_frame_read(vfs, movie);
		if (!movie.skipping_to_end)
		{
			return movie.state;
		}
	}
}

void bink_movie_set_looping(BinkMovie& movie, bool looping)
{
	movie.looping = looping;
}

void bink_movie_set_chroma_key(BinkMovie& movie, bool enabled)
{
	movie.chroma_key = enabled;
}

void bink_movie_build(
	const BinkMovie& movie,
	render::FrontendCommands& commands)
{
	render::frontend_commands_begin(commands);
	render::frontend_scissor(
		commands, 0, 0, render::kFrontendWidth, render::kFrontendHeight);
	const render::FrontendTexture* texture = nullptr;
	if (movie.frame_visible)
	{
		texture = &movie.texture;
	}
	else if (bgfx::isValid(movie.held_texture.handle))
	{
		texture = &movie.held_texture;
	}
	if (texture == nullptr)
	{
		return;
	}
	const float x =
		(static_cast<float>(render::kFrontendWidth) - texture->width) * 0.5f;
	const float y =
		(static_cast<float>(render::kFrontendHeight) - texture->height) * 0.5f;
	render::frontend_rgba_quad(
		commands,
		*texture,
		x,
		y,
		static_cast<float>(texture->width),
		static_cast<float>(texture->height));
}

bool bink_movie_is_widescreen(const BinkMovie& movie)
{
	return movie.file.info.width * 3 > movie.file.info.height * 4;
}

void bink_movie_build_fullscreen(
	const BinkMovie& movie,
	render::FrontendCommands& commands,
	std::uint32_t backbuffer_width,
	std::uint32_t backbuffer_height)
{
	render::frontend_commands_begin(commands);
	render::frontend_scissor(
		commands, 0, 0, render::kFrontendWidth, render::kFrontendHeight);
	const render::FrontendTexture* texture = nullptr;
	if (movie.frame_visible)
	{
		texture = &movie.texture;
	}
	else if (bgfx::isValid(movie.held_texture.handle))
	{
		texture = &movie.held_texture;
	}
	if (texture == nullptr || backbuffer_width == 0 || backbuffer_height == 0)
	{
		return;
	}

	const float source_aspect =
		static_cast<float>(texture->width) / texture->height;
	const float backbuffer_aspect =
		static_cast<float>(backbuffer_width) / backbuffer_height;
	float physical_width = static_cast<float>(backbuffer_width);
	float physical_height = static_cast<float>(backbuffer_height);
	if (backbuffer_aspect > source_aspect)
	{
		physical_width = physical_height * source_aspect;
	}
	else
	{
		physical_height = physical_width / source_aspect;
	}

	const float width =
		physical_width * render::kFrontendWidth / backbuffer_width;
	const float height =
		physical_height * render::kFrontendHeight / backbuffer_height;
	const float x = (render::kFrontendWidth - width) * 0.5f;
	const float y = (render::kFrontendHeight - height) * 0.5f;
	render::frontend_rgba_quad(commands, *texture, x, y, width, height);
}

void bink_movie_build_native_fit(
	const BinkMovie& movie,
	render::FrontendCommands& commands,
	std::uint32_t backbuffer_width,
	std::uint32_t backbuffer_height)
{
	render::frontend_commands_begin(commands);
	render::frontend_scissor(
		commands,
		0,
		0,
		static_cast<std::uint16_t>(
			std::min<std::uint32_t>(backbuffer_width, UINT16_MAX)),
		static_cast<std::uint16_t>(
			std::min<std::uint32_t>(backbuffer_height, UINT16_MAX)));
	const render::FrontendTexture* texture = nullptr;
	if (movie.frame_visible)
	{
		texture = &movie.texture;
	}
	else if (bgfx::isValid(movie.held_texture.handle))
	{
		texture = &movie.held_texture;
	}
	if (texture == nullptr || backbuffer_width == 0 || backbuffer_height == 0)
	{
		return;
	}

	const float source_aspect =
		static_cast<float>(texture->width) / texture->height;
	const float backbuffer_aspect =
		static_cast<float>(backbuffer_width) / backbuffer_height;
	float width = static_cast<float>(backbuffer_width);
	float height = static_cast<float>(backbuffer_height);
	if (backbuffer_aspect > source_aspect)
	{
		width = height * source_aspect;
	}
	else
	{
		height = width / source_aspect;
	}
	render::frontend_rgba_quad(
		commands,
		*texture,
		(static_cast<float>(backbuffer_width) - width) * 0.5f,
		(static_cast<float>(backbuffer_height) - height) * 0.5f,
		width,
		height);
}

void bink_movie_append(
	const BinkMovie& movie,
	render::FrontendCommands& commands,
	float x,
	float y)
{
	const render::FrontendTexture* texture = nullptr;
	if (movie.frame_visible)
	{
		texture = &movie.texture;
	}
	else if (bgfx::isValid(movie.held_texture.handle))
	{
		texture = &movie.held_texture;
	}
	if (texture != nullptr)
	{
		render::frontend_rgba_quad(
			commands,
			*texture,
			x,
			y,
			static_cast<float>(texture->width),
			static_cast<float>(texture->height));
	}
}

void bink_movie_pause(BinkMovie& movie, std::uint64_t now)
{
	if (movie.paused
		|| movie.state == BinkMovieState::idle
		|| movie.state == BinkMovieState::finished
		|| movie.state == BinkMovieState::failed)
	{
		return;
	}
	movie.paused = true;
	movie.paused_at = now;
	if (movie.audio_enabled && movie.source != 0)
	{
		alSourcePause(movie.source);
	}
}

void bink_movie_resume(BinkMovie& movie, std::uint64_t now)
{
	if (!movie.paused)
	{
		return;
	}
	const std::uint64_t elapsed =
		now > movie.paused_at ? now - movie.paused_at : 0;
	movie.started_at += elapsed;
	movie.next_frame_at += elapsed;
	movie.paused_at = 0;
	movie.paused = false;
	if (movie.audio_enabled && movie.source != 0)
	{
		ALint queued = 0;
		alGetSourcei(movie.source, AL_BUFFERS_QUEUED, &queued);
		if (queued > 0)
		{
			alSourcePlay(movie.source);
		}
	}
}

void bink_movie_finish(BinkMovie& movie)
{
	if (movie.state == BinkMovieState::idle
		|| movie.state == BinkMovieState::finished
		|| movie.state == BinkMovieState::failed)
	{
		return;
	}

	release_audio(movie);
	movie.seek_data.reset();
	movie.seek_request = {};
	movie.audio_enabled = false;
	movie.looping = false;
	movie.skipping_to_end = false;
	movie.paused = false;
	movie.paused_at = 0;
	movie.frame_index = movie.file.info.frame_count;
	movie.state = BinkMovieState::finished;
}

void bink_movie_skip_to_end(io::Vfs& vfs, BinkMovie& movie)
{
	if (movie.state == BinkMovieState::idle
		|| movie.state == BinkMovieState::finished
		|| movie.state == BinkMovieState::failed
		|| movie.skipping_to_end)
	{
		return;
	}

	release_audio(movie);
	movie.audio_enabled = false;
	movie.looping = false;
	movie.paused = false;
	movie.paused_at = 0;
	movie.skipping_to_end = true;
	if (movie.state == BinkMovieState::opening)
	{
		return;
	}
	begin_seek_read(vfs, movie);
}

void bink_movie_stop(io::Vfs& vfs, BinkMovie& movie)
{
	release_audio(movie);
	if (movie.frame_visible)
	{
		render::frontend_texture_shutdown(movie.held_texture);
		movie.held_texture =
			static_cast<render::FrontendTexture&&>(movie.texture);
	}
	bink_video_shutdown(movie.video);
	bink_close(vfs, movie.file);
	movie.frame_request = {};
	movie.seek_request = {};
	movie.seek_data.reset();
	movie.source = 0;
	movie.started_at = 0;
	movie.next_frame_at = 0;
	movie.paused_at = 0;
	movie.frame_index = 0;
	movie.read_size = 0;
	movie.seek_offset = 0;
	movie.free_count = 0;
	movie.state = BinkMovieState::idle;
	movie.frame_visible = false;
	movie.has_audio = false;
	movie.audio_enabled = false;
	movie.looping = false;
	movie.chroma_key = false;
	movie.skipping_to_end = false;
	movie.paused = false;
}

void bink_movie_shutdown(io::Vfs& vfs, BinkMovie& movie)
{
	bink_movie_stop(vfs, movie);
	render::frontend_texture_shutdown(movie.texture);
	render::frontend_texture_shutdown(movie.held_texture);
}
}

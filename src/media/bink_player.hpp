#pragma once

#include "audio/audio.hpp"
#include "media/bink.hpp"
#include "media/bink_audio.hpp"
#include "media/bink_video.hpp"
#include "render/frontend_renderer.hpp"

#include <AL/al.h>

#include <cstdint>

namespace sl_open::media
{
enum class BinkMovieState : std::uint8_t
{
	idle,
	opening,
	reading,
	waiting,
	finished,
	failed,
};

struct BinkMovie
{
	BinkFile file;
	BinkVideoDecoder video;
	BinkAudioDecoder audio;
	render::FrontendTexture texture;
	render::FrontendTexture held_texture;
	io::ReadRequest frame_request;
	io::ReadRequest seek_request;
	sl_open::Blob seek_data;
	ALuint source{};
	ALuint free_buffers[audio::kStreamBufferCount]{};
	std::uint8_t encoded[kBinkMaxFrameBytes]{};
	std::uint8_t rgba[640 * 480 * 4]{};
	std::int16_t pcm[192 * 1024 / 2]{};
	std::uint64_t started_at{};
	std::uint64_t next_frame_at{};
	std::uint64_t paused_at{};
	std::uint32_t frame_index{};
	std::uint32_t read_size{};
	std::uint32_t seek_offset{};
	std::uint32_t free_count{};
	BinkMovieState state{BinkMovieState::idle};
	bool frame_visible{};
	bool has_audio{};
	bool audio_enabled{};
	bool looping{};
	bool chroma_key{};
	bool skipping_to_end{};
	bool paused{};
};

bool bink_movie_open(
	io::Vfs& vfs,
	const char* path,
	ALuint source,
	const ALuint* buffers,
	std::uint64_t now,
	BinkMovie& movie);
BinkMovieState bink_movie_update(
	io::Vfs& vfs,
	std::uint64_t now,
	BinkMovie& movie);
void bink_movie_set_looping(BinkMovie& movie, bool looping);
void bink_movie_set_chroma_key(BinkMovie& movie, bool enabled);
void bink_movie_build(
	const BinkMovie& movie,
	render::FrontendCommands& commands);
bool bink_movie_is_widescreen(const BinkMovie& movie);
void bink_movie_build_fullscreen(
	const BinkMovie& movie,
	render::FrontendCommands& commands,
	std::uint32_t backbuffer_width,
	std::uint32_t backbuffer_height);
void bink_movie_build_native_fit(
	const BinkMovie& movie,
	render::FrontendCommands& commands,
	std::uint32_t backbuffer_width,
	std::uint32_t backbuffer_height);
void bink_movie_append(
	const BinkMovie& movie,
	render::FrontendCommands& commands,
	float x,
	float y);
void bink_movie_pause(BinkMovie& movie, std::uint64_t now);
void bink_movie_resume(BinkMovie& movie, std::uint64_t now);
void bink_movie_finish(BinkMovie& movie);
void bink_movie_skip_to_end(io::Vfs& vfs, BinkMovie& movie);
void bink_movie_stop(io::Vfs& vfs, BinkMovie& movie);
void bink_movie_shutdown(io::Vfs& vfs, BinkMovie& movie);
}

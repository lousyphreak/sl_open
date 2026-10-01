#include "audio/fat.hpp"

#include "assets/refpack.hpp"
#include "audio/wav.hpp"
#include "io/endian.hpp"

#include <SDL3/SDL.h>

#include <AL/al.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <utility>

namespace sl_open::audio
{
namespace
{
[[noreturn]] void invalid_sample_index(
	std::uint32_t index,
	std::uint32_t count)
{
	SDL_LogCritical(
		SDL_LOG_CATEGORY_AUDIO,
		"FAT sample index %u exceeds bank count %u",
		index,
		count);
	std::quick_exit(EXIT_FAILURE);
}

[[noreturn]] void invalid_voice_slot(std::uint32_t slot)
{
	SDL_LogCritical(
		SDL_LOG_CATEGORY_AUDIO,
		"2D sound voice slot %u exceeds pool size %u",
		slot,
		kEffectVoiceCount);
	std::quick_exit(EXIT_FAILURE);
}

bool parse_bank(sl_open::Blob&& stored, FatBank& bank)
{
	sl_open::Blob image;
	if (!sl_open::assets::unwrap_refpack(static_cast<sl_open::Blob&&>(stored), image)
		|| image.size < 8
		|| std::memcmp(image.data, "2.00", 4) != 0)
	{
		return false;
	}

	const std::uint32_t count = sl_open::io::read_le32(image.data + 4);
	const std::uint64_t directory_end =
		8 + static_cast<std::uint64_t>(count) * 12;
	if (count == 0 || count > kFatMaxSamples
		|| directory_end > image.size)
	{
		SDL_Log(
			"FAT bank has unsupported sample count %u (maximum %u)",
			count,
			kFatMaxSamples);
		return false;
	}

	std::uint64_t previous_end = directory_end;
	std::uint64_t total_pcm = 0;
	for (std::uint32_t index = 0; index < count; ++index)
	{
		const std::uint8_t* entry = image.data + 8 + index * 12;
		const std::uint32_t offset = sl_open::io::read_le32(entry);
		const std::uint32_t size = sl_open::io::read_le32(entry + 4);
		if (offset != previous_end
			|| size > image.size - offset)
		{
			return false;
		}

		WavInfo info;
		if (!parse_wav(image.data + offset, size, size, info))
		{
			return false;
		}
		const std::uint64_t pcm_bytes =
			static_cast<std::uint64_t>(info.total_frames)
			* info.channels * sizeof(std::int16_t);
		if (pcm_bytes > std::numeric_limits<std::uint32_t>::max()
			|| total_pcm > std::numeric_limits<std::uint32_t>::max() - pcm_bytes)
		{
			return false;
		}

		FatSample& sample = bank.samples[index];
		sample.pcm_offset = static_cast<std::uint32_t>(total_pcm);
		sample.pcm_bytes = static_cast<std::uint32_t>(pcm_bytes);
		sample.frame_count = info.total_frames;
		sample.sample_rate = info.sample_rate;
		sample.priority = sl_open::io::read_le32(entry + 8);
		sample.channels = info.channels;
		total_pcm += pcm_bytes;
		previous_end = static_cast<std::uint64_t>(offset) + size;
	}
	if (previous_end != image.size
		|| !bank.pcm.allocate(static_cast<std::size_t>(total_pcm)))
	{
		return false;
	}

	for (std::uint32_t index = 0; index < count; ++index)
	{
		const std::uint8_t* entry = image.data + 8 + index * 12;
		const std::uint32_t offset = sl_open::io::read_le32(entry);
		const std::uint32_t size = sl_open::io::read_le32(entry + 4);
		const FatSample& sample = bank.samples[index];
		WavInfo info;
		if (!decode_wav_into(
				image.data + offset,
				size,
				reinterpret_cast<std::int16_t*>(
					bank.pcm.data + sample.pcm_offset),
				sample.pcm_bytes / sizeof(std::int16_t),
				info)
			|| info.total_frames != sample.frame_count
			|| info.sample_rate != sample.sample_rate
			|| info.channels != sample.channels)
		{
			bank.pcm.reset();
			return false;
		}
	}

	bank.sample_count = count;
	bank.ready = true;
	return true;
}

bool finish_read(sl_open::io::Vfs& vfs, FatBank& bank)
{
	if (bank.request.status != sl_open::io::IoStatus::complete
		|| bank.request.transferred != bank.stored.size)
	{
		fat_bank_close(vfs, bank);
		return false;
	}

	sl_open::io::vfs_close(vfs, bank.file);
	bank.read_pending = false;
	bank.loading = false;
	bank.request = {};
	sl_open::Blob stored = static_cast<sl_open::Blob&&>(bank.stored);
	if (!parse_bank(static_cast<sl_open::Blob&&>(stored), bank))
	{
		fat_bank_close(vfs, bank);
		return false;
	}
	return true;
}

bool begin_bank_read(sl_open::io::Vfs& vfs, FatBank& bank)
{
	if (bank.file.size == 0 || bank.file.size > SIZE_MAX
		|| !bank.stored.allocate(static_cast<std::size_t>(bank.file.size)))
	{
		fat_bank_close(vfs, bank);
		return false;
	}
	bank.loading = true;
	const sl_open::io::IoStatus status = sl_open::io::vfs_read_at(
		vfs,
		bank.file,
		0,
		bank.stored.data,
		bank.stored.size,
		bank.request);
	if (status == sl_open::io::IoStatus::pending)
	{
		bank.read_pending = true;
		return true;
	}
	if (status != sl_open::io::IoStatus::complete)
	{
		fat_bank_close(vfs, bank);
		return false;
	}
	return finish_read(vfs, bank);
}

void clear_al_error()
{
	while (alGetError() != AL_NO_ERROR)
	{
	}
}

bool start_voice(
	Runtime& runtime,
	const FatBank& bank,
	std::uint32_t slot,
	std::uint32_t sample_index,
	int volume,
	int loop_count,
	int pan,
	int pitch_steps)
{
	if (sample_index >= bank.sample_count)
	{
		invalid_sample_index(sample_index, bank.sample_count);
	}
	if (slot >= kEffectVoiceCount || slot >= runtime.voice_count)
	{
		invalid_voice_slot(slot);
	}

	const FatSample& sample = bank.samples[sample_index];
	Voice& voice = runtime.voices[slot];
	const std::int16_t* pcm = fat_sample_pcm(bank, sample_index);
	if (!runtime.ready || !bank.ready || pcm == nullptr
		|| sample.pcm_bytes > static_cast<std::uint32_t>(
			std::numeric_limits<ALsizei>::max()))
	{
		return false;
	}

	alSourceStop(voice.source);
	alSourcei(voice.source, AL_BUFFER, 0);
	clear_al_error();
	alBufferData(
		voice.buffer,
		sample.channels == 1 ? AL_FORMAT_MONO16 : AL_FORMAT_STEREO16,
		pcm,
		static_cast<ALsizei>(sample.pcm_bytes),
		static_cast<ALsizei>(sample.sample_rate));
	alSourcei(voice.source, AL_BUFFER, static_cast<ALint>(voice.buffer));
	if (alGetError() != AL_NO_ERROR)
	{
		return false;
	}

	const int clamped_pan = std::clamp(pan, 0, 127);
	const float pan_x = static_cast<float>(clamped_pan) / 63.5f - 1.0f;
	const float pan_z = -std::sqrt(std::max(0.0f, 1.0f - pan_x * pan_x));
	const int effects_scaled =
		runtime.effects_volume * std::clamp(volume, 0, 127) / 127;
	const int final_volume =
		effects_scaled * runtime.master_volume / 127;
	const float pitch = std::exp2(
		static_cast<float>(std::clamp(pitch_steps, -96, 96)) / 24.0f);

	alSourcei(voice.source, AL_SOURCE_RELATIVE, AL_TRUE);
	alSourcef(voice.source, AL_ROLLOFF_FACTOR, 0.0f);
	alSource3f(voice.source, AL_POSITION, pan_x, 0.0f, pan_z);
	alSourcef(voice.source, AL_GAIN, static_cast<float>(final_volume) / 127.0f);
	alSourcef(voice.source, AL_PITCH, pitch);
	alSourcei(voice.source, AL_LOOPING, loop_count == 0 ? AL_TRUE : AL_FALSE);
	alSourcePlay(voice.source);
	if (alGetError() != AL_NO_ERROR)
	{
		alSourcei(voice.source, AL_BUFFER, 0);
		return false;
	}

	voice.priority = sample.priority;
	voice.requested_volume = std::clamp(volume, 0, 127);
	voice.remaining_plays = std::max(loop_count, 0);
	voice.protected_from_eviction = false;
	return true;
}

ALint voice_state(const Voice& voice)
{
	ALint state = AL_INITIAL;
	alGetSourcei(voice.source, AL_SOURCE_STATE, &state);
	return state;
}
}

bool fat_bank_open(sl_open::io::Vfs& vfs, const char* path, FatBank& bank)
{
	fat_bank_close(vfs, bank);
	if (!sl_open::io::vfs_open(vfs, path, bank.file))
	{
		fat_bank_close(vfs, bank);
		return false;
	}
	return begin_bank_read(vfs, bank);
}

bool fat_bank_open_from_archive(
	sl_open::io::Vfs& vfs,
	const char* archive,
	const char* path,
	FatBank& bank)
{
	fat_bank_close(vfs, bank);
	if (!sl_open::io::vfs_open_from_archive(
			vfs, archive, path, bank.file))
	{
		fat_bank_close(vfs, bank);
		return false;
	}
	return begin_bank_read(vfs, bank);
}

bool fat_bank_update(sl_open::io::Vfs& vfs, FatBank& bank)
{
	if (bank.ready)
	{
		return true;
	}
	if (!bank.loading)
	{
		return false;
	}
	if (bank.read_pending)
	{
		sl_open::io::vfs_poll(vfs, bank.request);
		if (bank.request.status == sl_open::io::IoStatus::pending)
		{
			return true;
		}
	}
	return finish_read(vfs, bank);
}

void fat_bank_close(sl_open::io::Vfs& vfs, FatBank& bank)
{
	sl_open::io::vfs_close(vfs, bank.file);
	bank = {};
}

const std::int16_t* fat_sample_pcm(const FatBank& bank, std::uint32_t index)
{
	if (index >= bank.sample_count)
	{
		invalid_sample_index(index, bank.sample_count);
	}
	const FatSample& sample = bank.samples[index];
	if (!bank.ready || sample.pcm_offset > bank.pcm.size
		|| sample.pcm_bytes > bank.pcm.size - sample.pcm_offset)
	{
		return nullptr;
	}
	return reinterpret_cast<const std::int16_t*>(
		bank.pcm.data + sample.pcm_offset);
}

int fat_play_auto(
	Runtime& runtime,
	const FatBank& bank,
	std::uint32_t sample_index,
	int volume,
	int loop_count,
	int pan,
	int pitch_steps)
{
	if (sample_index >= bank.sample_count)
	{
		invalid_sample_index(sample_index, bank.sample_count);
	}
	if (!runtime.ready || !bank.ready
		|| runtime.voice_count < kEffectVoiceCount)
	{
		return -1;
	}

	std::uint32_t selected = kEffectVoiceCount;
	for (std::uint32_t slot = 1; slot < kEffectVoiceCount; ++slot)
	{
		if (voice_state(runtime.voices[slot]) == AL_STOPPED)
		{
			selected = slot;
			break;
		}
	}
	if (selected == kEffectVoiceCount)
	{
		for (std::uint32_t slot = 0; slot < kEffectVoiceCount; ++slot)
		{
			if (voice_state(runtime.voices[slot]) == AL_INITIAL)
			{
				selected = slot;
				break;
			}
		}
	}
	if (selected == kEffectVoiceCount)
	{
		std::uint32_t lowest_priority = 9999;
		for (std::uint32_t slot = 0; slot < kEffectVoiceCount; ++slot)
		{
			const Voice& voice = runtime.voices[slot];
			if (!voice.protected_from_eviction
				&& voice.priority < lowest_priority)
			{
				lowest_priority = voice.priority;
				selected = slot;
			}
		}
		if (selected == kEffectVoiceCount
			|| bank.samples[sample_index].priority <= lowest_priority)
		{
			return -1;
		}
	}

	return start_voice(
		runtime,
		bank,
		selected,
		sample_index,
		volume,
		loop_count,
		pan,
		pitch_steps)
		? static_cast<int>(selected)
		: -1;
}

int fat_play_in_slot(
	Runtime& runtime,
	const FatBank& bank,
	std::uint32_t slot,
	std::uint32_t sample_index,
	int volume,
	int loop_count,
	int pan,
	int pitch_steps)
{
	if (slot >= kEffectVoiceCount)
	{
		invalid_voice_slot(slot);
	}
	if (sample_index >= bank.sample_count)
	{
		invalid_sample_index(sample_index, bank.sample_count);
	}
	return start_voice(
		runtime,
		bank,
		slot,
		sample_index,
		volume,
		loop_count,
		pan,
		pitch_steps)
		? static_cast<int>(slot)
		: -1;
}

int fat_play_spatial(
	Runtime& runtime,
	const FatBank& bank,
	std::uint32_t sample_index,
	int volume,
	int loop_count,
	const float position[3],
	const float direction[3],
	float minimum_distance,
	float maximum_distance,
	float cone_inner_degrees,
	float cone_outer_degrees,
	float cone_outer_gain)
{
	if (sample_index >= bank.sample_count)
	{
		invalid_sample_index(sample_index, bank.sample_count);
	}
	if (!runtime.ready || !bank.ready
		|| runtime.voice_count < kEffectVoiceCount)
	{
		return -1;
	}
	std::uint32_t selected = kEffectVoiceCount;
	for (std::uint32_t slot = 4; slot < kEffectVoiceCount; ++slot)
	{
		if (voice_state(runtime.voices[slot]) == AL_STOPPED
			|| voice_state(runtime.voices[slot]) == AL_INITIAL)
		{
			selected = slot;
			break;
		}
	}
	if (selected == kEffectVoiceCount)
	{
		std::uint32_t lowest_priority =
			std::numeric_limits<std::uint32_t>::max();
		for (std::uint32_t slot = 4; slot < kEffectVoiceCount; ++slot)
		{
			const Voice& voice = runtime.voices[slot];
			if (!voice.protected_from_eviction
				&& voice.priority < lowest_priority)
			{
				lowest_priority = voice.priority;
				selected = slot;
			}
		}
		if (selected == kEffectVoiceCount
			|| bank.samples[sample_index].priority <= lowest_priority)
		{
			return -1;
		}
	}
	if (!start_voice(
			runtime,
			bank,
			selected,
			sample_index,
			volume,
			loop_count,
			64,
			0))
	{
		return -1;
	}
	Voice& voice = runtime.voices[selected];
	alSourcei(voice.source, AL_SOURCE_RELATIVE, AL_TRUE);
	alSource3f(
		voice.source,
		AL_POSITION,
		position[0],
		position[1],
		position[2]);
	alSource3f(
		voice.source,
		AL_DIRECTION,
		direction[0],
		direction[1],
		direction[2]);
	alSourcef(voice.source, AL_ROLLOFF_FACTOR, 1.0f);
	alSourcef(
		voice.source,
		AL_REFERENCE_DISTANCE,
		std::max(0.001f, minimum_distance));
	alSourcef(
		voice.source,
		AL_MAX_DISTANCE,
		std::max(minimum_distance, maximum_distance));
	alSourcef(voice.source, AL_CONE_INNER_ANGLE, cone_inner_degrees);
	alSourcef(voice.source, AL_CONE_OUTER_ANGLE, cone_outer_degrees);
	alSourcef(
		voice.source,
		AL_CONE_OUTER_GAIN,
		std::clamp(cone_outer_gain, 0.0f, 1.0f));
	return static_cast<int>(selected);
}

void fat_stop(Runtime& runtime, std::uint32_t slot)
{
	if (slot >= kEffectVoiceCount || slot >= runtime.voice_count)
	{
		invalid_voice_slot(slot);
	}
	Voice& voice = runtime.voices[slot];
	alSourceStop(voice.source);
	alSourcei(voice.source, AL_BUFFER, 0);
	const ALuint source = voice.source;
	const ALuint buffer = voice.buffer;
	voice = {};
	voice.source = source;
	voice.buffer = buffer;
}

void fat_update_voices(Runtime& runtime)
{
	if (!runtime.ready)
	{
		return;
	}
	const std::uint32_t count =
		std::min(runtime.voice_count, kEffectVoiceCount);
	for (std::uint32_t slot = 0; slot < count; ++slot)
	{
		Voice& voice = runtime.voices[slot];
		if (voice.remaining_plays == 0
			|| voice_state(voice) != AL_STOPPED)
		{
			continue;
		}
		if (voice.remaining_plays > 1)
		{
			--voice.remaining_plays;
			alSourcePlay(voice.source);
		}
		else
		{
			voice.remaining_plays = 0;
			voice.priority = 0;
			voice.protected_from_eviction = false;
		}
	}
}
}

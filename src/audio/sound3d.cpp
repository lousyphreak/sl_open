#include "audio/sound3d.hpp"

#include <AL/al.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <limits>

namespace sl_open::audio
{
namespace
{
constexpr Sound3DDefinition kDefinitions[kSound3DDefinitionCount] = {
	{67, 1, 1, 0, 4, 80, 120, 210, 100.0f / 127, "GUN01"},
	{68, 1, 1, 0, 4, 80, 120, 210, 100.0f / 127, "GUN02"},
	{69, 1, 1, 0, 4, 80, 120, 210, 100.0f / 127, "GUN03"},
	{70, 1, 1, 0, 4, 80, 120, 210, 100.0f / 127, "GUN04"},
	{71, 1, 1, 0, 4, 80, 120, 210, 100.0f / 127, "GUN05"},
	{72, 1, 1, 0, 4, 80, 120, 210, 100.0f / 127, "GUN06"},
	{73, 1, 1, 0, 4, 80, 120, 210, 100.0f / 127, "GUN07"},
	{74, 1, 1, 0, 4, 80, 120, 210, 100.0f / 127, "GUN08"},
	{75, 1, 1, 0, 4, 80, 120, 210, 100.0f / 127, "GUN09"},
	{0, 1, 1, 0, 2, 120, 360, 360, 1, "GUN10"},
	{66, 1, 1, 0, 8, 60, 360, 360, 1, "FLAK01"},
	{62, 1, 1, 2, 60, 80, 360, 360, 1, "EXPLOSION01"},
	{63, 1, 1, 2, 60, 80, 360, 360, 1, "EXPLOSION02"},
	{64, 1, 1, 1, .4f, 5.6f, 120, 250, 100.0f / 127, "SHIELD01"},
	{65, 1, 1, 1, .4f, 5.6f, 120, 250, 100.0f / 127, "ARMOUR01"},
	{52, 1, 1, 3, .8f, 24, 90, 210, 1, "MISSILE01"},
	{53, 1, 1, 3, .8f, 24, 90, 210, 1, "MISSILE02"},
	{54, 1, 1, 3, .8f, 24, 90, 210, 1, "MISSILE03"},
	{55, 1, 1, 3, .8f, 24, 90, 210, 1, "MISSILE04"},
	{56, 1, 1, 3, .8f, 24, 90, 210, 1, "MISSILE05"},
	{57, 1, 1, 3, .8f, 24, 90, 210, 1, "MISSILE06"},
	{58, 1, 1, 3, .8f, 24, 90, 210, 1, "MISSILE07"},
	{59, 1, 1, 3, .8f, 24, 90, 210, 1, "MISSILE08"},
	{60, 1, 1, 3, .8f, 24, 90, 210, 1, "MISSILE09"},
	{61, 1, 1, 4, 32, 64, 360, 360, 1, "MISSILE10"},
	{47, 1, 1, 4, 18, 100, 90, 210, 100.0f / 127, "JUMPOUT"},
	{48, 1, 1, 4, 18, 100, 90, 210, 100.0f / 127, "JUMPIN"},
	{50, 1, 1, 4, 14, 60, 90, 210, 100.0f / 127, "WARPOUT"},
	{51, 1, 1, 4, 14, 60, 90, 210, 100.0f / 127, "WARPIN"},
	{46, 1, 1, 4, 18, 100, 360, 360, 100.0f / 127, "JUMPONLINE"},
	{49, 1, 1, 4, 14, 40, 360, 360, 100.0f / 127, "WARPPROJECT"},
	{32, 1, 0, 4, 2, 10, 90, 150, 85.0f / 127, "PSHIP01"},
	{33, 1, 0, 4, 2, 10, 90, 150, 85.0f / 127, "PSHIP02"},
	{34, 1, 0, 4, 2, 10, 90, 150, 85.0f / 127, "PSHIP03"},
	{35, 1, 0, 4, 2, 10, 90, 150, 85.0f / 127, "PSHIP04"},
	{36, 1, 0, 4, 2, 10, 90, 150, 85.0f / 127, "PSHIP05"},
	{37, 1, 0, 4, 2, 10, 90, 150, 85.0f / 127, "PSHIP06"},
	{38, 1, 0, 4, 2, 10, 90, 150, 85.0f / 127, "PSHIP07"},
	{39, 1, 0, 4, 2, 10, 90, 150, 85.0f / 127, "PSHIP08"},
	{40, 1, 0, 4, 2, 10, 90, 150, 85.0f / 127, "PSHIP09"},
	{41, 1, 0, 4, 2, 10, 90, 150, 85.0f / 127, "PSHIP10"},
	{42, 1, 0, 4, 2, 10, 90, 150, 85.0f / 127, "PSHIP11"},
	{43, 1, 0, 4, 2, 10, 90, 150, 85.0f / 127, "PSHIP12"},
	{44, 1, 1, 4, .4f, 16, 90, 150, 90.0f / 127, "CLOAK01"},
	{45, 1, 0, 4, 2, 10, 360, 360, 100.0f / 127, "BURNER01"},
	{29, 1, 1, 4, 10, 40, 90, 210, 100.0f / 127, "NANNY01"},
	{30, 1, 1, 1, 2, 20, 90, 210, 100.0f / 127, "NANNY02"},
	{31, 1, 1, 4, 40, 40, 90, 210, 100.0f / 127, "NANNY03"},
	{28, 1, 1, 4, 6, 120, 90, 210, 100.0f / 127, "EJECT01"},
	{12, 1, 1, 4, 80, 240, 90, 210, 1, "GATEOPEN"},
	{13, 1, 1, 4, 80, 240, 90, 210, 1, "GATECLOS"},
	{14, 1, 1, 4, 6, 60, 90, 210, 100.0f / 127, "ESCAPE"},
	{15, 1, 1, 1, 8, 20, 360, 360, 1, "PLATEOFF"},
	{16, 1, 1, 1, 6, 62, 360, 360, 1, "DOOROPEN"},
	{17, 1, 1, 1, 6, 62, 360, 360, 1, "DOORCLOS"},
	{18, 1, 1, 4, 6, 60, 360, 360, 1, "DOCK"},
	{19, 1, 1, 4, 6, 60, 360, 360, 1, "UNDOCK"},
	{20, 1, 1, 1, 10, 60, 90, 210, 100.0f / 127, "SHLDDOWN"},
	{21, 1, 1, 4, 320, 320, 360, 360, 1, "ICHARGE"},
	{22, 1, 1, 4, 320, 320, 360, 360, 1, "ILASER"},
	{23, 1, 1, 4, 1.6f, 12, 90, 210, 100.0f / 127, "RIPGRAB"},
	{24, 1, 1, 4, 1.6f, 12, 90, 210, 100.0f / 127, "TRACTOR"},
	{25, 1, 1, 4, 10, 60, 90, 210, 100.0f / 127, "SHIPLAND"},
	{26, 1, 1, 1, 10, 60, 90, 210, 100.0f / 127, "SHIPPASS"},
	{27, 1, 1, 1, 10, 120, 90, 210, 100.0f / 127, "BIGON"},
	{6, 1, 1, 4, 80, 280, 360, 360, 1, "CAPEXP"},
	{7, 1, 1, 4, 40, 160, 90, 210, 100.0f / 127, "UBEREXP"},
	{8, 1, 1, 4, 1.6f, 12, 90, 210, 100.0f / 127, "DMSPAWN1"},
	{9, 1, 1, 4, 1.6f, 8, 90, 210, 100.0f / 127, "DMSPAWN2"},
	{10, 1, 1, 4, 1.6f, 8, 90, 210, 100.0f / 127, "DMDROP"},
	{11, 1, 1, 4, 1.6f, 8, 90, 210, 100.0f / 127, "DMPICKUP"},
	{5, 1, 1, 1, 6, 24, 360, 360, 1, "COLL02"},
	{3, 1, 1, 4, 2, 12, 90, 210, 100.0f / 127, "PASS01"},
	{4, 1, 1, 4, 2, 12, 90, 210, 100.0f / 127, "PASS02"},
	{2, 1, 1, 4, 1.6f, 5.6f, 360, 360, 1, "PLAYERHIT"},
	{1, 1, 1, 4, .08f, 1.6f, 90, 210, 100.0f / 127, "MISSILESELECT"},
};

constexpr std::uint8_t kVoiceClassTiers[3][kSound3DVoiceCount] = {
	{1, 1, 2, 3, 3, 4, 5},
	{1, 1, 1, 1, 2, 5, 3, 3, 3, 3, 6, 4, 7},
	{1, 1, 1, 1, 1, 1, 2, 2, 3, 3, 3, 3, 3, 3, 3, 3,
		4, 4, 4, 4, 4, 5, 6, 7},
};

constexpr int kEngineRateBase[13] = {
	15000, 15000, 22050, 18000, 20000, 15000, 20000,
	20000, 20000, 20000, 15000, 20000, 10000};
constexpr int kEngineRateScale[13] = {
	15000, 15000, 2000, 4000, 2050, 15000, 2050,
	2050, 2050, 2050, 15000, 5000, 1500};
constexpr int kEngineVolumeBase[13] = {
	30, 15, 30, 30, 20, 30, 30, 30, 30, 30, 30, 50, 60};
constexpr int kEngineVolumeScale[13] = {
	20, 15, 20, 15, 20, 20, 20, 20, 20, 20, 20, 20, 40};

void clear_al_error()
{
	while (alGetError() != AL_NO_ERROR)
	{
	}
}

glm::vec3 listener_relative(
	const Sound3DListener& listener,
	const glm::vec3& world_vector,
	bool position)
{
	glm::vec3 value = glm::transpose(listener.orientation)
		* (position ? world_vector - listener.position : world_vector);
	if (position)
	{
		value *= 0.0004f;
	}
	else
	{
		value = glm::dot(value, value) == 0.0f
			? glm::vec3{0.0f, 0.0f, 1.0f}
			: glm::normalize(value);
	}
	value.y = -value.y;
	return value;
}

float distance_to_listener(
	const Sound3DListener& listener,
	const Sound3DTransform& transform)
{
	return glm::distance(listener.position, transform.position) * 0.0004f;
}

void publish_slot(
	const Sound3DVoice& voice,
	std::int16_t slot,
	Sound3DPublishOwnerSlot callback,
	void* userdata)
{
	if (callback != nullptr
		&& (voice.source_kind == 3 || voice.source_kind == 4))
	{
		callback(userdata, voice.source, slot);
	}
}

void destroy_voice(
	Runtime& audio,
	Sound3DRuntime& runtime,
	std::uint32_t slot,
	Sound3DPublishOwnerSlot callback,
	void* userdata)
{
	if (slot >= runtime.voice_count)
	{
		return;
	}
	Sound3DVoice& voice = runtime.voices[slot];
	if (voice.active)
	{
		publish_slot(voice, -1, callback, userdata);
	}
	const std::uint32_t audio_slot = kSound3DVoiceFirst + slot;
	if (audio_slot < audio.voice_count)
	{
		Voice& output = audio.voices[audio_slot];
		alSourceStop(output.source);
		alSourcei(output.source, AL_BUFFER, 0);
		output.priority = 0;
		output.requested_volume = 0;
		output.remaining_plays = 0;
		output.protected_from_eviction = false;
	}
	voice = {};
	voice.owner_slot = -1;
	voice.definition = -1;
	voice.source_kind = UINT8_MAX;
}

bool reusable(const Sound3DVoice& voice)
{
	return !voice.active || voice.replacement_mark;
}

int exact_class_candidate(
	const Sound3DRuntime& runtime,
	std::uint8_t requested_class)
{
	for (std::uint32_t slot = 0; slot < runtime.voice_count; ++slot)
	{
		if (slot != static_cast<std::uint32_t>(runtime.primary_engine_slot)
			&& runtime.voice_classes[slot] == requested_class
			&& reusable(runtime.voices[slot]))
		{
			return static_cast<int>(slot);
		}
	}
	return -1;
}

int generic_free_candidate(const Sound3DRuntime& runtime)
{
	for (std::uint32_t slot = 0; slot < runtime.voice_count; ++slot)
	{
		if (slot != static_cast<std::uint32_t>(runtime.primary_engine_slot)
			&& slot != static_cast<std::uint32_t>(runtime.secondary_engine_slot)
			&& runtime.voice_classes[slot] != 4
			&& !runtime.voices[slot].active)
		{
			return static_cast<int>(slot);
		}
	}
	return -1;
}

int select_voice(
	Sound3DRuntime& runtime,
	std::uint8_t requested_class)
{
	if (requested_class == 5)
	{
		return runtime.primary_engine_slot;
	}
	if (requested_class == 6)
	{
		return runtime.secondary_engine_slot;
	}

	int selected = exact_class_candidate(runtime, requested_class);
	if (selected < 0 && requested_class != 0)
	{
		selected = exact_class_candidate(runtime, 0);
	}
	if (selected >= 0)
	{
		runtime.voices[selected].replacement_mark = false;
		return selected;
	}
	selected = generic_free_candidate(runtime);
	if (selected >= 0)
	{
		runtime.voices[selected].replacement_mark = requested_class != 4;
	}
	return selected;
}

void set_transform(
	Voice& output,
	const Sound3DDefinition& definition,
	std::uint8_t definition_index,
	const Sound3DTransform& transform,
	const Sound3DListener& listener,
	bool update_position = true,
	bool update_direction = true,
	bool update_velocity = true)
{
	glm::vec3 direction = transform.direction;
	if (definition.source_kind == 2)
	{
		direction = listener.position - transform.position;
	}
	const glm::vec3 position =
		listener_relative(listener, transform.position, true);
	const glm::vec3 facing =
		listener_relative(listener, direction, false);
	glm::vec3 velocity =
		glm::transpose(listener.orientation) * transform.velocity * 0.0004f;
	velocity.y = -velocity.y;
	if (update_position)
	{
		alSource3f(
			output.source,
			AL_POSITION,
			position.x,
			position.y,
			position.z);
	}
	if (update_direction)
	{
		alSource3f(
			output.source,
			AL_DIRECTION,
			facing.x,
			facing.y,
			facing.z);
	}
	if (update_velocity)
	{
		alSource3f(
			output.source,
			AL_VELOCITY,
			velocity.x,
			velocity.y,
			velocity.z);
	}
	if (definition_index == 43)
	{
		// Definition 43 is not one of the retail range-gate exceptions.
		alSourcef(output.source, AL_MAX_DISTANCE, definition.maximum_distance);
	}
}

void set_playback_rate(
	Runtime& audio,
	const FatBank& bank,
	std::uint32_t voice_slot,
	std::uint8_t sample_index,
	int playback_rate)
{
	if (voice_slot >= audio.voice_count || sample_index >= bank.sample_count)
	{
		return;
	}
	const std::uint32_t source_rate =
		std::max<std::uint32_t>(bank.samples[sample_index].sample_rate, 1);
	alSourcef(
		audio.voices[voice_slot].source,
		AL_PITCH,
		static_cast<float>(playback_rate)
			/ static_cast<float>(source_rate));
}

void set_engine_volume(
	Runtime& audio,
	Sound3DRuntime& runtime,
	std::int8_t slot,
	float engine_units)
{
	if (slot < 0 || slot >= runtime.voice_count
		|| !runtime.voices[slot].active)
	{
		return;
	}
	runtime.voices[slot].gain_units = std::max(0.0f, engine_units);
	constexpr float scale = 1.0f / (127.0f * 127.0f * 127.0f);
	alSourcef(
		audio.voices[kSound3DVoiceFirst + slot].source,
		AL_GAIN,
		runtime.voices[slot].gain_units
			* audio.effects_volume * audio.master_volume * scale);
}

int player_engine_table_index(std::uint16_t type)
{
	int normalized = type;
	if (normalized >= 244)
	{
		normalized -= 244;
	}
	return normalized == 45
		? 12
		: std::clamp(normalized, 0, 12);
}

std::uint8_t player_engine_definition(std::uint16_t type)
{
	int normalized = type;
	if (normalized >= 244)
	{
		normalized -= 244;
	}
	if (normalized == 45)
	{
		return 37;
	}
	return normalized >= 0 && normalized < 13
		? static_cast<std::uint8_t>(31 + normalized)
		: 31;
}
}

const Sound3DDefinition* sound3d_definition(std::uint8_t index)
{
	return index < kSound3DDefinitionCount ? &kDefinitions[index] : nullptr;
}

void sound3d_runtime_init(
	const Runtime& audio,
	Sound3DRuntime& runtime)
{
	if (runtime.initialized)
	{
		return;
	}
	runtime = {};
	const std::uint32_t available = audio.voice_count > kSound3DVoiceFirst
		? audio.voice_count - kSound3DVoiceFirst
		: 0;
	runtime.voice_count = static_cast<std::uint8_t>(
		std::min<std::uint32_t>(available, kSound3DVoiceCount));
	const std::uint32_t tier = runtime.voice_count <= 14
		? 0
		: runtime.voice_count <= 30 ? 1 : 2;
	for (std::uint32_t slot = 0; slot < runtime.voice_count; ++slot)
	{
		runtime.voice_classes[slot] = kVoiceClassTiers[tier][slot];
		runtime.voices[slot].owner_slot = -1;
		runtime.voices[slot].definition = -1;
		runtime.voices[slot].source_kind = UINT8_MAX;
		if (runtime.voice_classes[slot] == 5)
		{
			runtime.primary_engine_slot = static_cast<std::int8_t>(slot);
		}
		else if (runtime.voice_classes[slot] == 6)
		{
			runtime.secondary_engine_slot = static_cast<std::int8_t>(slot);
		}
	}
	runtime.initialized = true;
}

void sound3d_destroy_all(
	Runtime& audio,
	Sound3DRuntime& runtime,
	Sound3DPublishOwnerSlot callback,
	void* userdata)
{
	for (std::uint32_t slot = 0; slot < runtime.voice_count; ++slot)
	{
		destroy_voice(audio, runtime, slot, callback, userdata);
	}
	runtime.engine_transition_state = 0;
	runtime.engine_transition_tick = 0;
}

void sound3d_runtime_shutdown(
	Runtime& audio,
	Sound3DRuntime& runtime,
	Sound3DPublishOwnerSlot callback,
	void* userdata)
{
	sound3d_destroy_all(audio, runtime, callback, userdata);
	runtime = {};
}

int sound3d_play(
	Runtime& audio,
	Sound3DRuntime& runtime,
	const FatBank& bank,
	const Sound3DSource& source,
	const Sound3DListener& listener,
	std::uint8_t definition_index,
	std::uint8_t requested_class,
	std::uint32_t tick,
	float gain,
	Sound3DPublishOwnerSlot callback,
	void* userdata)
{
	const Sound3DDefinition* definition =
		sound3d_definition(definition_index);
	if (!audio.ready || !bank.ready || definition == nullptr
		|| definition->sample >= bank.sample_count)
	{
		return -1;
	}
	if (!runtime.initialized)
	{
		sound3d_runtime_init(audio, runtime);
	}
	if (runtime.voice_count == 0)
	{
		return -1;
	}

	Sound3DTransform transform = source.transform;
	const float distance = distance_to_listener(listener, transform);
	if ((definition_index < 31 || definition_index > 42)
		&& definition_index != 44
		&& distance > definition->maximum_distance)
	{
		return -1;
	}

	const int slot = select_voice(runtime, requested_class);
	if (slot < 0)
	{
		return -1;
	}
	if (runtime.voices[slot].active)
	{
		destroy_voice(audio, runtime, slot, callback, userdata);
	}

	const FatSample& sample = bank.samples[definition->sample];
	const std::int16_t* pcm = fat_sample_pcm(bank, definition->sample);
	if (pcm == nullptr
		|| sample.pcm_bytes > static_cast<std::uint32_t>(
			std::numeric_limits<ALsizei>::max()))
	{
		return -1;
	}

	const std::uint32_t audio_slot = kSound3DVoiceFirst + slot;
	Voice& output = audio.voices[audio_slot];
	alSourceStop(output.source);
	alSourcei(output.source, AL_BUFFER, 0);
	clear_al_error();
	alBufferData(
		output.buffer,
		sample.channels == 1 ? AL_FORMAT_MONO16 : AL_FORMAT_STEREO16,
		pcm,
		static_cast<ALsizei>(sample.pcm_bytes),
		static_cast<ALsizei>(sample.sample_rate));
	alSourcei(output.source, AL_BUFFER, static_cast<ALint>(output.buffer));
	if (alGetError() != AL_NO_ERROR)
	{
		return -1;
	}

	alSourcei(output.source, AL_SOURCE_RELATIVE, AL_TRUE);
	alSourcei(
		output.source,
		AL_LOOPING,
		definition->loops == 0 ? AL_TRUE : AL_FALSE);
	alSourcef(output.source, AL_ROLLOFF_FACTOR, 1.0f);
	alSourcef(output.source, AL_REFERENCE_DISTANCE, definition->minimum_distance);
	alSourcef(output.source, AL_MAX_DISTANCE, definition->maximum_distance);
	alSourcef(output.source, AL_CONE_INNER_ANGLE, definition->cone_inner);
	alSourcef(output.source, AL_CONE_OUTER_ANGLE, definition->cone_outer);
	alSourcef(output.source, AL_CONE_OUTER_GAIN, definition->cone_outer_gain);
	constexpr float gain_scale = 1.0f / (127.0f * 127.0f);
	alSourcef(
		output.source,
		AL_GAIN,
		std::max(0.0f, definition->volume * gain)
			* audio.effects_volume * audio.master_volume * gain_scale);
	set_transform(output, *definition, definition_index, transform, listener);

	int playback_rate = 22050;
	if (definition_index == 11 || definition_index == 12)
	{
		playback_rate = 18050 + static_cast<int>(
			static_cast<float>(std::rand()) * (7000.0f / RAND_MAX));
	}
	set_playback_rate(
		audio, bank, audio_slot, definition->sample, playback_rate);
	alSourcePlay(output.source);
	if (alGetError() != AL_NO_ERROR)
	{
		alSourcei(output.source, AL_BUFFER, 0);
		return -1;
	}

	output.priority = sample.priority;
	output.requested_volume = 127;
	output.remaining_plays = definition->loops;
	output.protected_from_eviction = true;
	const bool replacement_mark = runtime.voices[slot].replacement_mark;
	Sound3DVoice& voice = runtime.voices[slot];
	voice = {};
	voice.source = source;
	voice.transform = transform;
	voice.start_tick = tick;
	voice.duration_ticks = sample.sample_rate == 0
		? 0
		: static_cast<std::uint32_t>(
			static_cast<std::uint64_t>(sample.frame_count) * 100
				/ sample.sample_rate);
	voice.maximum_distance = definition->maximum_distance;
	voice.requested_gain = gain;
	voice.gain_units =
		std::max(0.0f, definition->volume * gain * 127.0f);
	voice.owner_slot = static_cast<std::int16_t>(slot);
	voice.definition = definition_index;
	voice.allocation_class = requested_class;
	voice.source_kind = definition->source_kind;
	voice.replacement_mark = replacement_mark;
	voice.active = true;
	if (runtime.stopped)
	{
		alSourcePause(output.source);
		voice.paused = true;
	}
	publish_slot(voice, static_cast<std::int16_t>(slot), callback, userdata);
	return slot;
}

void sound3d_update(
	Runtime& audio,
	Sound3DRuntime& runtime,
	const Sound3DListener& listener,
	std::uint32_t tick,
	Sound3DResolveSource resolve_source,
	Sound3DPublishOwnerSlot callback,
	void* userdata)
{
	if (!runtime.initialized || runtime.stopped)
	{
		return;
	}
	for (std::uint32_t slot = 0; slot < runtime.voice_count; ++slot)
	{
		Sound3DVoice& voice = runtime.voices[slot];
		if (!voice.active)
		{
			continue;
		}
		Voice& output = audio.voices[kSound3DVoiceFirst + slot];
		ALint state = AL_INITIAL;
		alGetSourcei(output.source, AL_SOURCE_STATE, &state);
		const bool reserved =
			slot == static_cast<std::uint32_t>(runtime.primary_engine_slot)
			|| slot == static_cast<std::uint32_t>(runtime.secondary_engine_slot);
		if (state == AL_STOPPED
			|| (!reserved && voice.duration_ticks != 0
				&& tick - voice.start_tick > voice.duration_ticks))
		{
			destroy_voice(audio, runtime, slot, callback, userdata);
			continue;
		}

		// sound_3d_update (LANCER.EXE 0x00481bf0) republishes transforms
		// only for explicit kind-one sources and live kind-four objects.
		// Projectile, listener-facing, and missile sources retain their
		// start transform. Kind one republishes position/orientation but not
		// velocity; kind four republishes all three.
		const bool explicit_source = voice.source_kind == 1;
		const bool object_source = voice.source_kind == 4;
		const bool updates_position = explicit_source || object_source;
		if (object_source && (resolve_source == nullptr
			|| !resolve_source(userdata, voice.source, voice.transform)))
		{
			destroy_voice(audio, runtime, slot, callback, userdata);
			continue;
		}
		const Sound3DDefinition& definition =
			kDefinitions[voice.definition];
		if (updates_position
			&& distance_to_listener(listener, voice.transform)
				> voice.maximum_distance
			&& !reserved)
		{
			destroy_voice(audio, runtime, slot, callback, userdata);
			continue;
		}
		if (updates_position)
		{
			set_transform(
				output,
				definition,
				static_cast<std::uint8_t>(voice.definition),
				voice.transform,
				listener,
				true,
				true,
				object_source);
		}
	}
}

void sound3d_update_player_engine(
	Runtime& audio,
	Sound3DRuntime& runtime,
	const FatBank& bank,
	const Sound3DSource& player_source,
	const Sound3DListener& listener,
	std::uint16_t object_type,
	float throttle,
	bool afterburner,
	bool reverse_thrust,
	std::uint8_t mission_state,
	std::uint32_t tick,
	Sound3DPublishOwnerSlot callback,
	void* userdata)
{
	if (!runtime.initialized)
	{
		sound3d_runtime_init(audio, runtime);
	}
	if (runtime.primary_engine_slot < 0)
	{
		return;
	}
	if (!runtime.voices[runtime.primary_engine_slot].active
		&& runtime.engine_transition_state == 0)
	{
		sound3d_play(
			audio,
			runtime,
			bank,
			player_source,
			listener,
			player_engine_definition(object_type),
			5,
			tick,
			1.0f,
			callback,
			userdata);
	}
	if (!runtime.voices[runtime.primary_engine_slot].active)
	{
		return;
	}

	std::int8_t transition_slot = runtime.primary_engine_slot;
	if (runtime.secondary_engine_slot >= 0)
	{
		set_engine_volume(audio, runtime, runtime.secondary_engine_slot, 70.0f);
		transition_slot = runtime.secondary_engine_slot;
	}
	const bool special = afterburner || reverse_thrust;
	const int table = player_engine_table_index(object_type);
	switch (runtime.engine_transition_state)
	{
	case 0:
		if (!special)
		{
			set_playback_rate(
				audio,
				bank,
				kSound3DVoiceFirst + runtime.primary_engine_slot,
				kDefinitions[player_engine_definition(object_type)].sample,
				kEngineRateBase[table]
					+ static_cast<int>(
						kEngineRateScale[table] * throttle));
			set_engine_volume(
				audio,
				runtime,
				runtime.primary_engine_slot,
				static_cast<float>(
					kEngineVolumeBase[table]
						+ static_cast<int>(
							kEngineVolumeScale[table] * throttle)));
			if (runtime.secondary_engine_slot >= 0
				&& runtime.voices[runtime.secondary_engine_slot].active)
			{
				destroy_voice(
					audio,
					runtime,
					runtime.secondary_engine_slot,
					callback,
					userdata);
			}
			break;
		}
		runtime.engine_transition_tick = tick;
		runtime.engine_transition_state = 1;
		destroy_voice(audio, runtime, transition_slot, callback, userdata);
		sound3d_play(
			audio,
			runtime,
			bank,
			player_source,
			listener,
			44,
			runtime.secondary_engine_slot >= 0 ? 6 : 5,
			tick,
			0.0f,
			callback,
			userdata);
		break;
	case 1:
		if (special)
		{
			const int ramp = std::min(
				70,
				static_cast<int>((tick - runtime.engine_transition_tick)
					* .8f) + 5);
			set_engine_volume(
				audio,
				runtime,
				transition_slot,
				static_cast<float>(ramp + 57));
			set_playback_rate(
				audio,
				bank,
				kSound3DVoiceFirst + transition_slot,
				kDefinitions[44].sample,
				reverse_thrust ? 7000 : 12000 - 90 * ramp);
			break;
		}
		if (runtime.secondary_engine_slot >= 0)
		{
			runtime.engine_transition_state = 3;
			runtime.engine_transition_tick = tick;
			break;
		}
		sound3d_play(
			audio,
			runtime,
			bank,
			player_source,
			listener,
			player_engine_definition(object_type),
			5,
			tick,
			0.0f,
			callback,
			userdata);
		runtime.engine_transition_state = 0;
		break;
	case 3:
		if (tick - runtime.engine_transition_tick > 25)
		{
			destroy_voice(
				audio, runtime, transition_slot, callback, userdata);
			runtime.engine_transition_state = 0;
			runtime.engine_transition_tick = 0;
			break;
		}
		if (!special)
		{
			set_engine_volume(
				audio,
				runtime,
				transition_slot,
				(1.0f - (tick - runtime.engine_transition_tick) * .04f)
					* 127.0f);
			break;
		}
		runtime.engine_transition_state = 0;
		break;
	default:
		runtime.engine_transition_state = 0;
		break;
	}
	if (mission_state == 13)
	{
		set_engine_volume(audio, runtime, runtime.primary_engine_slot, 0.0f);
		set_engine_volume(audio, runtime, runtime.secondary_engine_slot, 0.0f);
	}
}

void sound3d_stop_all(Runtime& audio, Sound3DRuntime& runtime)
{
	if (!runtime.initialized || runtime.stopped)
	{
		return;
	}
	for (std::uint32_t slot = 0; slot < runtime.voice_count; ++slot)
	{
		if (runtime.voices[slot].active)
		{
			alSourcePause(audio.voices[kSound3DVoiceFirst + slot].source);
			runtime.voices[slot].paused = true;
		}
	}
	runtime.stopped = true;
}

void sound3d_resume_all(Runtime& audio, Sound3DRuntime& runtime)
{
	if (!runtime.initialized || !runtime.stopped)
	{
		return;
	}
	for (std::uint32_t slot = 0; slot < runtime.voice_count; ++slot)
	{
		if (runtime.voices[slot].active && runtime.voices[slot].paused)
		{
			alSourcePlay(audio.voices[kSound3DVoiceFirst + slot].source);
			runtime.voices[slot].paused = false;
		}
	}
	runtime.stopped = false;
}

void sound3d_refresh_gains(
	Runtime& audio,
	const Sound3DRuntime& runtime)
{
	constexpr float scale = 1.0f / (127.0f * 127.0f * 127.0f);
	for (std::uint32_t slot = 0; slot < runtime.voice_count; ++slot)
	{
		if (!runtime.voices[slot].active)
		{
			continue;
		}
		alSourcef(
			audio.voices[kSound3DVoiceFirst + slot].source,
			AL_GAIN,
			runtime.voices[slot].gain_units
				* audio.effects_volume
				* audio.master_volume
				* scale);
	}
}
}

#pragma once

#include "audio/audio.hpp"
#include "audio/fat.hpp"

#include <glm/glm.hpp>

#include <cstdint>

namespace sl_open::audio
{
constexpr std::uint32_t kSound3DDefinitionCount = 76;
constexpr std::uint32_t kSound3DVoiceCount = 32;
constexpr std::uint32_t kSound3DVoiceFirst = kEffectVoiceCount;

struct Sound3DDefinition
{
	std::uint8_t sample{};
	float volume{1.0f};
	std::uint8_t loops{1};
	std::uint8_t source_kind{};
	float minimum_distance{};
	float maximum_distance{};
	float cone_inner{};
	float cone_outer{};
	float cone_outer_gain{1.0f};
	const char* name{};
};

enum class Sound3DBinding : std::uint8_t
{
	explicit_transform,
	projectile,
	missile,
	object,
	model_frame,
};

struct Sound3DTransform
{
	glm::vec3 position{0.0f};
	glm::vec3 direction{0.0f, 0.0f, 1.0f};
	glm::vec3 velocity{0.0f};
};

struct Sound3DSource
{
	Sound3DBinding binding{Sound3DBinding::explicit_transform};
	Sound3DTransform transform;
	std::uint16_t index{UINT16_MAX};
	std::uint16_t generation{};
	std::uint32_t serial{};
	std::uint16_t object_index{UINT16_MAX};
	std::uint16_t object_generation{};
	std::int16_t model_reference{-1};
	bool player{};
};

struct Sound3DListener
{
	glm::vec3 position{0.0f};
	glm::mat3 orientation{1.0f};
};

using Sound3DResolveSource = bool (*)(
	void* userdata,
	const Sound3DSource& source,
	Sound3DTransform& transform);
using Sound3DPublishOwnerSlot = void (*)(
	void* userdata,
	const Sound3DSource& source,
	std::int16_t slot);

struct Sound3DVoice
{
	Sound3DSource source;
	Sound3DTransform transform;
	std::uint32_t start_tick{};
	std::uint32_t duration_ticks{};
	float maximum_distance{};
	float requested_gain{1.0f};
	float gain_units{};
	std::int16_t owner_slot{-1};
	std::int16_t definition{-1};
	std::uint8_t allocation_class{};
	std::uint8_t source_kind{UINT8_MAX};
	bool replacement_mark{};
	bool active{};
	bool paused{};
};

struct Sound3DRuntime
{
	Sound3DVoice voices[kSound3DVoiceCount];
	std::uint8_t voice_classes[kSound3DVoiceCount]{};
	std::uint8_t voice_count{};
	std::int8_t primary_engine_slot{-1};
	std::int8_t secondary_engine_slot{-1};
	std::uint8_t engine_transition_state{};
	std::uint32_t engine_transition_tick{};
	bool initialized{};
	bool stopped{};
};

const Sound3DDefinition* sound3d_definition(std::uint8_t index);
void sound3d_runtime_init(
	const Runtime& audio,
	Sound3DRuntime& runtime);
void sound3d_destroy_all(
	Runtime& audio,
	Sound3DRuntime& runtime,
	Sound3DPublishOwnerSlot publish_owner_slot = nullptr,
	void* userdata = nullptr);
void sound3d_runtime_shutdown(
	Runtime& audio,
	Sound3DRuntime& runtime,
	Sound3DPublishOwnerSlot publish_owner_slot = nullptr,
	void* userdata = nullptr);

int sound3d_play(
	Runtime& audio,
	Sound3DRuntime& runtime,
	const FatBank& bank,
	const Sound3DSource& source,
	const Sound3DListener& listener,
	std::uint8_t definition_index,
	std::uint8_t requested_class,
	std::uint32_t tick,
	float gain = 1.0f,
	Sound3DPublishOwnerSlot publish_owner_slot = nullptr,
	void* userdata = nullptr);
void sound3d_update(
	Runtime& audio,
	Sound3DRuntime& runtime,
	const Sound3DListener& listener,
	std::uint32_t tick,
	Sound3DResolveSource resolve_source,
	Sound3DPublishOwnerSlot publish_owner_slot,
	void* userdata);
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
	Sound3DPublishOwnerSlot publish_owner_slot,
	void* userdata);
void sound3d_stop_all(Runtime& audio, Sound3DRuntime& runtime);
void sound3d_resume_all(Runtime& audio, Sound3DRuntime& runtime);
void sound3d_refresh_gains(
	Runtime& audio,
	const Sound3DRuntime& runtime);
}

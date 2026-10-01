#pragma once

#include "core/math.hpp"

#include <cstdint>
#include <vector>

namespace sl_open::assets
{
struct GameplayLocator;
}

namespace sl_open::game
{
struct ObjectModelReference;
struct World;
struct WorldObject;

enum class BuiltinModelSequence : std::uint8_t
{
	startup,
	fire,
	deploy,
};

struct ModelAnimationEventHandler
{
	void* context{};
	void (*fire_type0)(
		void* context,
		World& world,
		WorldObject& object,
		std::uint16_t model_reference,
		std::uint32_t simulation_tick){};
	void (*flash_type2)(
		void* context,
		World& world,
		WorldObject& object,
		std::uint16_t model_reference,
		std::uint32_t simulation_tick){};
};

bool model_animation_start_named(
	WorldObject& object,
	std::uint16_t model_reference,
	const char* name,
	float time,
	std::int16_t mode,
	float rate);
bool model_animation_start_index(
	WorldObject& object,
	std::uint16_t model_reference,
	std::int16_t sequence,
	float time,
	std::int16_t mode,
	float rate);
bool model_animation_start_builtin(
	WorldObject& object,
	std::uint16_t model_reference,
	BuiltinModelSequence sequence,
	float time,
	std::int16_t mode,
	float rate);
void model_animation_set_playback(
	WorldObject& object,
	std::uint16_t model_reference,
	std::int16_t mode,
	float rate);
void model_animation_start_named_direct_children(
	WorldObject& object,
	const char* name);
void model_animation_stop_named_direct_children(
	WorldObject& object,
	const char* name);
void model_animation_reverse_named_direct_children(
	WorldObject& object,
	const char* name);
void model_animation_initialize_object(WorldObject& object);
void model_animation_recompute_pose(
	WorldObject& object,
	std::uint16_t model_reference);
glm::mat4 model_animation_render_transform(
	const WorldObject& object,
	std::uint16_t model_reference,
	float service_fraction);
void model_animation_publish_scene_transforms(
	WorldObject& object,
	float service_fraction,
	std::vector<std::uint8_t>& traversal_state);
const assets::GameplayLocator* model_animation_find_locator(
	const WorldObject& object,
	std::uint16_t model_reference,
	std::int16_t type,
	std::uint16_t ordinal = 0);
glm::mat4 model_animation_locator_transform(
	const WorldObject& object,
	std::uint16_t model_reference,
	const assets::GameplayLocator& locator,
	float service_fraction);
void model_animation_service(
	World& world,
	std::uint32_t simulation_tick,
	const ModelAnimationEventHandler& events);
}

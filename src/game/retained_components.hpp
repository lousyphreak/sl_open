#pragma once

#include "game/world.hpp"

#include <cstddef>
#include <cstdint>

namespace sl_open::game
{
ObjectModelReference* retained_find_named_model(
	WorldObject& object,
	const char* name);
const ObjectModelReference* retained_find_named_model(
	const WorldObject& object,
	const char* name);
bool retained_set_named_model_hidden(
	WorldObject& object,
	const char* name,
	bool hidden);
bool retained_set_named_model_scale(
	WorldObject& object,
	const char* name,
	float scale);
void retained_set_root_model_scale(
	WorldObject& object,
	const glm::vec3& scale);
ObjectModelReference* retained_component_model(
	WorldObject& object,
	std::int16_t component);
const ObjectModelReference* retained_component_model(
	const WorldObject& object,
	std::int16_t component);

std::size_t retained_set_component_disabled(
	WorldObject& object,
	std::int16_t component,
	bool disabled);
std::size_t retained_set_lights_disabled(
	WorldObject& object,
	bool disabled);
bool retained_set_model_light_channel(
	WorldObject& object,
	std::uint16_t model_reference,
	std::uint16_t channel,
	bool enabled);
std::size_t retained_set_model_lights(
	WorldObject& object,
	std::uint16_t model_reference,
	bool enabled);
std::size_t retained_set_all_model_lights(
	WorldObject& object,
	bool enabled);
std::size_t retained_destroy_component_group(
	WorldObject& object,
	std::int16_t component,
	bool keep_authored_hidden_parts);
std::size_t retained_set_turret_target(
	WorldObject& object,
	std::int16_t component,
	std::uint16_t target_object);
bool retained_replace_component(
	WorldObject& source,
	std::int16_t component,
	WorldObject& replacement);
}

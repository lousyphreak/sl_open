#pragma once

#include <cstddef>
#include <cstdint>

namespace sl_open::assets
{
inline constexpr bool object_type_is_planet(std::uint16_t type)
{
	return (type >= 0x5f && type <= 0x69)
		|| (type >= 0xc9 && type <= 0xd3);
}

constexpr std::size_t kObjectTypeResourceCount = 256;

struct ObjectTypeResourceDefinition
{
	const char* model_path{};
	const char* schematic_path{};
};

const ObjectTypeResourceDefinition& object_type_resource(
	std::uint16_t type);
bool object_type_has_model(std::uint16_t type);

// GameObject_create_runtime, LANCER.EXE 0x00466c10. These requested types
// finish construction as the returned base runtime type, sharing its model,
// flight/combat descriptors, and type-specific behavior.
std::uint16_t object_type_runtime_alias(std::uint16_t requested_type);

// MissionObject_instantiate_live_object, LANCER.EXE
// 0x00457d16..0x00457e1f. This is the pre-factory selection applied to
// ordinary mission objects before the central runtime aliases above.
std::uint16_t object_type_for_mission_creation(
	std::uint16_t authored_type,
	std::uint16_t mission_number,
	std::uint8_t group_object_class,
	bool mission_25_alternate);
}

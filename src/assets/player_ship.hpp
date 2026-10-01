#pragma once

#include <cstdint>

namespace sl_open::assets
{
constexpr std::uint16_t kPlayerShipCount = 12;

// LANCER.EXE 0x004ec298, one mask in each 0x22c-byte loadout ship record.
// The capability names are language entries 0x1ef..0x1f6 in bit order.
enum class PlayerShipCapability : std::uint8_t
{
	reverse_thrust = 0x01,
	constant_ecm = 0x02,
	tractor_beam = 0x04,
	nova_cannon = 0x08,
	spectral_shields = 0x10,
	blind_fire = 0x20,
	cloaking_device = 0x40,
	super_charge = 0x80,
};

struct PlayerShipCapabilityDefinition
{
	PlayerShipCapability capability;
	std::uint16_t language_id;
};

inline constexpr PlayerShipCapabilityDefinition
	kPlayerShipCapabilityDefinitions[] = {
		{PlayerShipCapability::reverse_thrust, 0x1ef},
		{PlayerShipCapability::constant_ecm, 0x1f0},
		{PlayerShipCapability::tractor_beam, 0x1f1},
		{PlayerShipCapability::nova_cannon, 0x1f2},
		{PlayerShipCapability::spectral_shields, 0x1f3},
		{PlayerShipCapability::blind_fire, 0x1f4},
		{PlayerShipCapability::cloaking_device, 0x1f5},
		{PlayerShipCapability::super_charge, 0x1f6},
	};

struct PlayerShipDefinition
{
	std::uint8_t ship_class{};
	std::uint8_t access_level{};
	std::uint8_t crew_count{};
	std::uint8_t capability_mask{};
};

// LANCER.EXE 0x004ec276..0x004edac2, the static fields in the twelve
// 0x22c-byte loadout ship records. Predator through Phoenix are in the same
// order as gameplay object types 0..11. Tiger aliases 0xf4..0xff use the
// matching base definition.
inline constexpr PlayerShipDefinition kPlayerShipDefinitions[
	kPlayerShipCount] = {
	{1, 1, 2, 0x20}, {1, 1, 2, 0x10}, {3, 1, 3, 0x00},
	{1, 1, 2, 0x10}, {2, 2, 2, 0x20}, {3, 2, 2, 0x00},
	{4, 2, 2, 0x10}, {3, 3, 2, 0x20}, {5, 3, 3, 0x01},
	{4, 3, 3, 0x20}, {7, 4, 2, 0x71}, {6, 4, 2, 0x29},
};

constexpr std::uint16_t player_ship_index_for_type(std::uint16_t type)
{
	if (type >= 0xf4 && type <= 0xff)
	{
		type = static_cast<std::uint16_t>(type - 0xf4);
	}
	return type < kPlayerShipCount ? type : kPlayerShipCount;
}

constexpr bool player_ship_has_capability(
	std::uint16_t type,
	PlayerShipCapability capability)
{
	const std::uint16_t index = player_ship_index_for_type(type);
	return index < kPlayerShipCount
		&& (kPlayerShipDefinitions[index].capability_mask
			& static_cast<std::uint8_t>(capability)) != 0;
}
}

#pragma once

#include <cstdint>

namespace sl_open::assets
{
struct ShipStatsTable;
}

namespace sl_open::mission
{
struct Runtime;
}

namespace sl_open::game
{
struct World;
struct WorldObject;

// GameObject_scale_damage_by_difficulty, LANCER.EXE 0x00463d70.
// `attacker_index` is the live GameObject slot passed in EDX. Retail compares
// that slot with the local player slot while applying the hostile-target stage.
float scale_damage_by_difficulty(
	const World& world,
	const mission::Runtime& mission,
	const WorldObject& target,
	float raw_damage,
	std::uint16_t attacker_index);

// Publishes the normalized TargetRef state used by damage-owner smart
// targeting. Component -1 selects the root and requests a target-panel
// refresh equivalent to retail's full SetTarget path.
void damage_select_target(
	World& world,
	WorldObject& target,
	std::int16_t component);

// GameObject_apply_primary_bank_damage, LANCER.EXE 0x00463ee0.
// `structural_ratio` is multiplied by the unscaled part of `raw_damage`
// which crossed the primary bank. The structural owner applies its own
// two retail difficulty passes.
bool apply_primary_bank_damage(
	World& world,
	mission::Runtime& mission,
	WorldObject& target,
	const assets::ShipStatsTable& stats,
	std::uint8_t bank,
	float raw_damage,
	float structural_ratio,
	std::uint16_t attacker_index,
	std::uint8_t cause,
	bool impact_feedback_enabled,
	std::uint32_t simulation_tick);

// GameObject_apply_structural_bank_damage, LANCER.EXE 0x004641f0.
// Every structural-damage producer must cross this owner so protection,
// attacker publication, and the synchronous death transition remain one
// indivisible retail operation. Returns false only for one of the owner's
// entry rejections.
bool apply_structural_bank_damage(
	World& world,
	mission::Runtime& mission,
	WorldObject& target,
	const assets::ShipStatsTable& stats,
	std::uint8_t bank,
	float raw_damage,
	std::uint16_t attacker_index,
	std::uint8_t cause,
	bool impact_feedback_enabled,
	std::uint32_t simulation_tick);
}

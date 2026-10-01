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

// ExhaustHazard_register_object (LANCER.EXE 0x004699e0) is called by the
// central object factory only after the retained model tree exists. The
// renderer owns that model instantiation in the reimplementation, so it
// publishes the completed object through this equivalent bridge.
void exhaust_hazard_register_object(
	World& world,
	WorldObject& object);

// ExhaustHazard_registry_invalidate (0x00469840) makes the next service
// rebuild the fixed 400-index cache.
void exhaust_hazard_invalidate(World& world);

// ExhaustHazard_update_player_damage (0x00469850): lazy registry rebuild,
// exact type-2 exhaust-box exposure, bank-2 damage, and local feedback.
void exhaust_hazard_service(
	World& world,
	mission::Runtime& mission,
	const assets::ShipStatsTable& stats,
	bool impact_feedback_enabled,
	std::uint32_t simulation_tick);
}

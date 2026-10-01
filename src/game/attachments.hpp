#pragma once

#include "assets/gameplay_model.hpp"
#include "assets/ship_stats.hpp"
#include "game/world.hpp"

#include <cstdint>
#include <vector>

namespace sl_open::game
{
struct AttachmentDefinition
{
	const char* primary_model{};
	const char* alternate_model{};
	std::int32_t initial_count{1};
};

enum class AttachmentModelVariant : std::uint8_t
{
	primary,
	alternate,
};

const AttachmentDefinition& attachment_definition(
	std::int16_t definition_index);
const char* attachment_definition_model(
	std::int16_t definition_index,
	AttachmentModelVariant variant);
std::int32_t attachment_definition_initial_count(
	std::int16_t definition_index);
void attachments_destroy_live_models(WorldObject& object);
void attachments_build_live_models(
	WorldObject& object,
	bool deathmatch,
	bool force_in_deathmatch);
void attachments_rebuild_selected_loadout(
	WorldObject& object,
	bool deathmatch,
	bool force_in_deathmatch);
void attachments_initialize_hardpoints(
	WorldObject& object,
	const std::vector<assets::GameplayHardpoint>& hardpoints,
	bool deathmatch,
	bool force_in_deathmatch);
void attachments_replenish(
	WorldObject& object,
	const assets::ShipStatsTable& stats,
	bool deathmatch,
	bool force_in_deathmatch);
}

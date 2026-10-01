#include "game/attachments.hpp"

#include <algorithm>
#include <array>
#include <iterator>

namespace sl_open::game
{
namespace
{
constexpr std::array<AttachmentDefinition, kAttachmentDefinitionCount>
make_attachment_catalog()
{
	std::array<AttachmentDefinition, kAttachmentDefinitionCount> catalog{};
	for (AttachmentDefinition& definition : catalog)
	{
		definition.initial_count = 1;
	}
	catalog[0] = {
		"01_screamer_pod.shp", "01_screamer.shp", 20};
	catalog[1] = {
		"02_raptor_pod.shp", "02_raptor.shp", 3};
	catalog[2].primary_model = "03_havoc.shp";
	catalog[3].primary_model = "04_jackhammer.shp";
	catalog[4].primary_model = "05_bandit.shp";
	catalog[5].primary_model = "06_vagabond.shp";
	catalog[6] = {
		"07_solomon_pod.shp", "07_solomon.shp", 4};
	catalog[7].primary_model = "08_imp.shp";
	catalog[8] = {
		"09_hawk_pod.shp", "09_hawk.shp", 4};
	catalog[10].primary_model = "fuel_pod.shp";
	catalog[20].primary_model = "01_lasr_can.shp";
	catalog[21].primary_model = "02_gat_laser.shp";
	catalog[22].primary_model = "03_proton.shp";
	catalog[23].primary_model = "04_pulse_las.shp";
	catalog[24].primary_model = "05_tachyon_gun.shp";
	catalog[25].primary_model = "06_neutron_gun.shp";
	catalog[26].primary_model = "07_mass_ion.shp";
	catalog[27].primary_model = "08_gat_plasma.shp";
	catalog[28].primary_model = "09_rus_btur.shp";
	catalog[29].primary_model = "10_rus_stur.shp";
	catalog[30].primary_model = "11_al_mtur.shp";
	catalog[31].primary_model = "12_light_gun.shp";
	catalog[32].primary_model = "13_al_stur.shp";
	catalog[33].primary_model = "14_al_stur.shp";
	catalog[34].primary_model = "al_mis_tur.shp";
	catalog[35].primary_model = "coal_mis_tur.shp";
	catalog[36].primary_model = "allied_cap_gun.shp";
	catalog[37].primary_model = "col_cap_gun.shp";
	catalog[38].primary_model = "gun_base.shp";
	catalog[39].primary_model = "small gun_base.shp";
	catalog[100].primary_model = "01_cargo_pod.shp";
	catalog[101].primary_model = "02_rus_pod.shp";
	catalog[105].primary_model = "fuel_pod1.shp";
	return catalog;
}

constexpr auto kAttachmentCatalog = make_attachment_catalog();

std::int16_t selected_definition(
	const WorldObject& object,
	const AttachmentSlot& attachment,
	std::uint8_t ordinal)
{
	// GameObject_rebuild_ordnance_definitions (LANCER.EXE 0x0045e500)
	// uses the authored hardpoint column selected by object +0x648.
	// Instant Action's local player takes the separately observed 5,3,1
	// cycle instead of an authored column.
	if (object.multiplayer_player_loadout_valid)
	{
		return object.multiplayer_player_loadout[ordinal];
	}
	if (object.player && object.special_player_loadout)
	{
		constexpr std::int16_t definitions[] = {5, 3, 1};
		return definitions[ordinal % std::size(definitions)];
	}
	return attachment.default_loadout[object.loadout_index];
}
}

const AttachmentDefinition& attachment_definition(
	std::int16_t definition_index)
{
	static constexpr AttachmentDefinition invalid{nullptr, nullptr, 0};
	return definition_index >= 0
			&& static_cast<std::size_t>(definition_index)
				< kAttachmentCatalog.size()
		? kAttachmentCatalog[definition_index]
		: invalid;
}

const char* attachment_definition_model(
	std::int16_t definition_index,
	AttachmentModelVariant variant)
{
	const AttachmentDefinition& definition =
		attachment_definition(definition_index);
	// AttachmentResources_initialize stores two model resources per
	// definition. GameObject_build_attachment_models selects the alternate
	// in deathmatch, while Missile_launch_from_ship_mount independently
	// selects that same alternate as the detached projectile body.
	return variant == AttachmentModelVariant::alternate
		? definition.alternate_model
		: definition.primary_model;
}

std::int32_t attachment_definition_initial_count(
	std::int16_t definition_index)
{
	// AttachmentResources_initialize (LANCER.EXE 0x0045de70) initializes
	// all 180 catalog records to one and overrides only these four missile
	// pod counts. Negative definitions create no attached object.
	return attachment_definition(definition_index).initial_count;
}

void attachments_destroy_live_models(WorldObject& object)
{
	// ReplenishWeapons follows the parent child-array owner to the
	// allocated attachment object, destroys that object, and only then
	// clears the complete twenty-slot array. The retained renderer uses
	// an ownership generation instead of raw child pointers, preserving
	// the same destructive lifetime boundary without dangling GPU state.
	for (AttachmentSlot& attachment : object.attachments)
	{
		attachment.live_model = false;
		attachment.alternate_model = false;
		attachment.cloak_models.clear();
		attachment.cloak_model_order.clear();
		attachment.cloak_install_pending = false;
		++attachment.ownership_generation;
	}
}

void attachments_build_live_models(
	WorldObject& object,
	bool deathmatch,
	bool force_in_deathmatch)
{
	for (std::uint8_t index = 0;
		index < object.attachment_count;
		++index)
	{
		AttachmentSlot& attachment = object.attachments[index];
		attachment.live_model = false;
		attachment.alternate_model = false;
		if (attachment.definition_index < 0
			|| (deathmatch && !force_in_deathmatch))
		{
			continue;
		}
		const char* model = attachment_definition_model(
			attachment.definition_index,
			deathmatch
				? AttachmentModelVariant::alternate
				: AttachmentModelVariant::primary);
		if (model == nullptr)
		{
			continue;
		}
		attachment.live_model = true;
		attachment.alternate_model = deathmatch;
		++attachment.ownership_generation;
	}
}

void attachments_rebuild_selected_loadout(
	WorldObject& object,
	bool deathmatch,
	bool force_in_deathmatch)
{
	// The animated-door Dock state at LANCER.EXE 0x004079c9 destroys
	// every live attachment, reselects the definitions for the existing
	// hardpoints, and reconstructs their models. Unlike ReplenishWeapons,
	// that transition does not restore shields or the other ship resources.
	attachments_destroy_live_models(object);
	for (std::uint8_t index = 0;
		index < object.attachment_count;
		++index)
	{
		AttachmentSlot& attachment = object.attachments[index];
		attachment.definition_index =
			selected_definition(object, attachment, index);
		// GameObject_build_attachment_models writes the selected catalog
		// definition to the first word of the attached ordnance object.
		// AI commands 2/3 later read that retained object-kind word.
		attachment.kind = attachment.definition_index;
		attachment.remaining_count =
			attachment_definition_initial_count(
				attachment.definition_index);
	}
	attachments_build_live_models(
		object, deathmatch, force_in_deathmatch);
}

void attachments_initialize_hardpoints(
	WorldObject& object,
	const std::vector<assets::GameplayHardpoint>& hardpoints,
	bool deathmatch,
	bool force_in_deathmatch)
{
	attachments_destroy_live_models(object);
	object.attachment_count = static_cast<std::uint8_t>(
		std::min<std::size_t>(
			hardpoints.size(),
			std::size(object.attachments)));
	for (std::uint8_t index = 0;
		index < object.attachment_count;
		++index)
	{
		AttachmentSlot& attachment = object.attachments[index];
		const assets::GameplayHardpoint& source = hardpoints[index];
		attachment.local_position = source.position;
		attachment.local_orientation = source.basis;
		std::copy(
			std::begin(source.default_loadout),
			std::end(source.default_loadout),
			std::begin(attachment.default_loadout));
		attachment.definition_index =
			selected_definition(object, attachment, index);
		attachment.kind = attachment.definition_index;
		attachment.remaining_count =
			attachment_definition_initial_count(
				attachment.definition_index);
	}
	for (std::uint8_t index = object.attachment_count;
		index < std::size(object.attachments);
		++index)
	{
		object.attachments[index] = {};
	}
	attachments_build_live_models(
		object, deathmatch, force_in_deathmatch);
}

void attachments_replenish(
	WorldObject& object,
	const assets::ShipStatsTable& stats,
	bool deathmatch,
	bool force_in_deathmatch)
{
	if (!object.active || object.type >= assets::kShipStatsCount)
	{
		return;
	}

	// ReplenishWeapons_command (LANCER.EXE 0x00459fa0) first destroys the
	// live objects mounted in every ordnance slot, then reconstructs the
	// definition list and attached models. This runtime represents those
	// children directly by their retained hardpoint slots, so rebuilding
	// the definitions and counts performs the same ownership transition.
	attachments_destroy_live_models(object);
	if (deathmatch && !force_in_deathmatch)
	{
		// GameObject_build_attachment_models receives false from
		// ReplenishWeapons. In a deathmatch mission, 0x45e1a0 therefore
		// leaves the just-cleared attachment count at zero.
		object.attachment_count = 0;
	}
	else
	{
		for (std::uint8_t index = 0;
			index < object.attachment_count;
			++index)
		{
			AttachmentSlot& attachment = object.attachments[index];
			attachment.definition_index =
				selected_definition(object, attachment, index);
			attachment.kind = attachment.definition_index;
			attachment.remaining_count =
				attachment_definition_initial_count(
					attachment.definition_index);
		}
		attachments_build_live_models(
			object, deathmatch, force_in_deathmatch);
	}

	const assets::ObjectTypeStats& values =
		stats.records[object.type].object;
	object.chaff_count = 29;
	// ReplenishWeapons 0x45a0ad preserves object+0x5e8 in deathmatch.
	if (!deathmatch)
	{
		object.afterburner_fuel = values.afterburner_seconds * 100;
	}
	object.ammunition = values.ammunition;
	object.gun_energy = values.gun_energy_max;
	for (std::uint8_t index = 0;
		index < object.attachment_count;
		++index)
	{
		if (object.attachments[index].definition_index == 10)
		{
			object.afterburner_fuel += 5000;
		}
	}

	const float primary =
		static_cast<float>(values.primary_bank_max * 6 - 1);
	const float structural =
		static_cast<float>(values.structural_bank_max * 6 - 1);
	std::fill(
		std::begin(object.primary_shields),
		std::end(object.primary_shields),
		primary);
	std::fill(
		std::begin(object.secondary_shields),
		std::end(object.secondary_shields),
		structural);
}
}

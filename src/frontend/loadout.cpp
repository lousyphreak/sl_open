#include "frontend/loadout.hpp"

#include "assets/game_stats.hpp"
#include "assets/player_ship.hpp"
#include "assets/gameplay_model.hpp"
#include "campaign/campaign.hpp"
#include "frontend/loadout_layout.hpp"
#include "render/frontend_renderer.hpp"

#include <algorithm>
#include <cmath>
#include <iterator>
#include <limits>

namespace sl_open::frontend
{
namespace
{
constexpr std::uint8_t kProgressionShipCounts[] = {4, 7, 10, 12};
constexpr std::uint8_t kRankShipCounts[] = {4, 5, 6, 7, 8, 9, 10, 11, 12};
constexpr std::uint16_t kMissileUnlockMasks[] = {
	0x021d, 0x00a2, 0x0100, 0x0040};
constexpr std::uint16_t kMissileCapacities[10] = {
	999, 999, 999, 3, 999, 999, 999, 999, 999, 999};
constexpr std::uint64_t kActivationMilliseconds = 2000;
constexpr std::uint64_t kShipSelectionMilliseconds = 1500;
constexpr std::uint64_t kHardpointZoomMilliseconds = 400;
constexpr std::uint64_t kMissileAnimationMilliseconds = 1000;
constexpr float kShipSpinRadiansPerMillisecond =
	0.0015707963611930609f;

struct RatingRange
{
	float minimum{std::numeric_limits<float>::max()};
	float maximum{std::numeric_limits<float>::lowest()};
};

void include_rating(RatingRange& range, float value)
{
	range.minimum = std::min(range.minimum, value);
	range.maximum = std::max(range.maximum, value);
}

std::uint8_t display_rating(
	const RatingRange& range,
	float value,
	std::uint8_t minimum,
	std::uint8_t maximum = 10)
{
	// FUN_004504f0 maps the observed range to minimum..maximum and calls
	// retail __ftol, whose temporary x87 control word truncates the result.
	if (range.minimum == range.maximum)
	{
		return maximum;
	}
	return static_cast<std::uint8_t>(
		static_cast<std::int32_t>(
			static_cast<float>(minimum) + (value - range.minimum)
			/ (range.maximum - range.minimum)
			* static_cast<float>(maximum - minimum)));
}

void build_ship_display_stats(
	LoadoutCatalog& catalog,
	const assets::ShipStatsTable& stats)
{
	// FUN_00426600 visits the twelve Alliance and nine Coalition fighter
	// presentation records in this order before rating the player ships.
	constexpr std::uint8_t kFighterTypes[] = {
		4, 3, 2, 5, 1, 7, 11, 0, 9, 10, 6, 8,
		42, 49, 39, 40, 50, 44, 43, 41, 46,
	};
	RatingRange ranges[6];
	RatingRange agility_ranges[2];
	for (const std::uint8_t type : kFighterTypes)
	{
		const assets::ShipStatsRecord& record = stats.records[type];
		const float values[] = {
			record.flight.max_speed,
			record.flight.yaw_rate,
			record.flight.linear_retention,
			static_cast<float>(record.object.primary_bank_max),
			record.object.primary_recharge_time,
			static_cast<float>(record.object.structural_bank_max),
		};
		for (std::size_t field = 0; field < std::size(values); ++field)
		{
			if (field != 1)
			{
				include_rating(ranges[field], values[field]);
			}
		}
		include_rating(
			agility_ranges[record.object.allegiance_class == 0 ? 0 : 1],
			values[1]);
	}
	for (std::uint8_t type = 0; type < assets::kPlayerShipCount; ++type)
	{
		const assets::ShipStatsRecord& record = stats.records[type];
		const float values[] = {
			record.flight.max_speed,
			record.flight.yaw_rate,
			record.flight.linear_retention,
			static_cast<float>(record.object.primary_bank_max),
			record.object.primary_recharge_time,
			static_cast<float>(record.object.structural_bank_max),
		};
		std::uint8_t ratings[6];
		for (std::size_t field = 0; field < std::size(values); ++field)
		{
			ratings[field] = display_rating(
					field == 1
						? agility_ranges[
							record.object.allegiance_class == 0 ? 0 : 1]
						: ranges[field],
					values[field],
					3);
		}
		LoadoutShipStats& output = catalog.ship_stats[type];
		// FUN_00441aa0 copies the ITAC fighter record's agility and
		// acceleration fields into the loadout record's opposite storage
		// order, matching the loadout label table at 0x004ec024.
		output.max_speed_rating = ratings[0];
		output.acceleration_rating = ratings[2];
		output.agility_rating = ratings[1];
		output.shield_power_rating = ratings[3];
		output.shield_recharge_rating = ratings[4];
		output.armor_rating = ratings[5];
		output.afterburner_seconds =
			static_cast<std::uint16_t>(record.object.afterburner_seconds);
	}
}

void build_missile_display_stats(
	LoadoutCatalog& catalog,
	const assets::MissileStatsTable& stats)
{
	RatingRange speed_range;
	RatingRange travel_range;
	RatingRange damage_range;
	for (std::size_t type = 0; type < std::size(stats.records); ++type)
	{
		const assets::MissileStats& missile = stats.records[type];
		include_rating(speed_range, missile.speed);
		include_rating(
			travel_range,
			missile.speed * static_cast<float>(missile.lifetime_ticks));
		if (type != 3)
		{
			include_rating(
				damage_range, missile.shield_damage + missile.hull_damage);
		}
	}
	for (std::uint8_t display_type = 0; display_type < 10; ++display_type)
	{
		LoadoutMissileStats& output = catalog.missile_stats[display_type];
		// FUN_004456f0 displays derived fields only for types zero through
		// eight. The tenth loadout model is the non-launchable fuel pod.
		if (display_type == 9)
		{
			output = {-1, -1, -1, -1};
			continue;
		}
		const assets::MissileStats& missile = stats.records[display_type];
		output.lock_seconds = display_type == 0 || display_type == 6
			? -1
			: static_cast<std::int8_t>(
				static_cast<std::int32_t>(missile.lock_ticks * 0.01f));
		output.speed_rating = static_cast<std::int8_t>(
			display_rating(speed_range, missile.speed, 3));
		output.travel_rating = static_cast<std::int8_t>(display_rating(
			travel_range,
			missile.speed * static_cast<float>(missile.lifetime_ticks),
			3));
		output.damage_rating = display_type == 3
			? 10
			: static_cast<std::int8_t>(display_rating(
				damage_range,
				missile.shield_damage + missile.hull_damage,
				1,
				8));
	}
}

glm::vec2 project_loadout_point(const glm::vec4& camera)
{
	return {320.0f + camera.x * (639.0f * 0.6f) / camera.z,
		240.0f + camera.y * (479.0f * 0.8f) / camera.z};
}

bool hit_triangle(
	const glm::vec2& point, const glm::vec2& a,
	const glm::vec2& b, const glm::vec2& c, bool double_sided)
{
	const glm::vec2 ab = b - a;
	const glm::vec2 ac = c - a;
	const float determinant = ab.x * ac.y - ac.x * ab.y;
	// Triangle2D_contains_point, LANCER.EXE 0x00427080.
	if (std::abs(determinant) < 0.0001f || (!double_sided && determinant > 0.0f))
	{
		return false;
	}
	const glm::vec2 ap = point - a;
	const float u = (ac.y * ap.x - ac.x * ap.y) / determinant;
	const float v = (ab.x * ap.y - ab.y * ap.x) / determinant;
	return u >= 0.0f && u <= 1.0f && v >= 0.0f && v <= 1.0f && u + v <= 1.0f;
}

bool hit_quad(
	const glm::vec2& point, const glm::mat4& transform, float width, float height)
{
	const glm::vec2 corners[] = {
		project_loadout_point(transform * glm::vec4{-width * 0.5f, -height * 0.5f, 0.0f, 1.0f}),
		project_loadout_point(transform * glm::vec4{width * 0.5f, -height * 0.5f, 0.0f, 1.0f}),
		project_loadout_point(transform * glm::vec4{width * 0.5f, height * 0.5f, 0.0f, 1.0f}),
		project_loadout_point(transform * glm::vec4{-width * 0.5f, height * 0.5f, 0.0f, 1.0f}),
	};
	return hit_triangle(point, corners[0], corners[1], corners[2], true)
		|| hit_triangle(point, corners[0], corners[2], corners[3], true);
}

bool hit_model(
	const render::MissionGpuModel& model, const glm::mat4& transform,
	const glm::vec2& point, std::uint8_t lod_index)
{
	for (const render::MissionGpuNode& node : model.nodes)
	{
		if (node.lods.empty())
		{
			continue;
		}
		const render::MissionGpuLod& lod = node.lods[
			std::min<std::size_t>(lod_index, node.lods.size() - 1)];
		const glm::mat4 node_transform = transform * node.object_transform;
		for (const assets::GameplayFace& face : lod.source_faces)
		{
			const render::MissionGpuSection& section = lod.sections[face.section];
			if (section.suppressed || section.lines)
			{
				continue;
			}
			const auto project_corner = [&](std::uint16_t corner) {
				const assets::GameplayVertex& vertex = lod.source_vertices[
					lod.source_indices[lod.source_face_corners[face.first_corner + corner]]];
				return project_loadout_point(node_transform
					* glm::vec4{vertex.x, vertex.y, vertex.z, 1.0f});
			};
			const glm::vec2 a = project_corner(0);
			glm::vec2 b = project_corner(1);
			for (std::uint16_t corner = 2; corner < face.corner_count; ++corner)
			{
				const glm::vec2 c = project_corner(corner);
				if (hit_triangle(point, a, b, c, section.double_sided))
				{
					return true;
				}
				b = c;
			}
		}
	}
	return false;
}

std::int8_t hit_button(const Loadout& loadout, const glm::vec2& point, std::uint64_t now)
{
	const glm::mat4 view = loadout_view();
	const std::uint8_t count = loadout.page == LoadoutPage::missiles ? 6 : 4;
	for (std::uint8_t index = 0; index < count; ++index)
	{
		const std::int8_t button = index == 0 ? 1 : index == 1 ? 0 : index;
		if (button == 0 && loadout_launch_button_hidden(loadout, now))
		{
			continue;
		}
		const LoadoutButtonDefinition& definition = kLoadoutButtons[index];
		const glm::mat4 transform = view * glm::translate(glm::mat4{1.0f},
			glm::vec3{definition.x, definition.y, loadout.pressed_button == button ? 0.2f : 0.0f});
		if (hit_quad(point, transform, definition.width, definition.height))
		{
			return button;
		}
	}
	return -1;
}

std::int16_t hit_loadout_object(
	const Loadout& loadout, const LoadoutCatalog& catalog,
	const glm::vec2& point, std::uint64_t now)
{
	const std::int8_t button = hit_button(loadout, point, now);
	if (button >= 0)
	{
		return button;
	}
	const glm::mat4 view = loadout_view();
	if (loadout.page != LoadoutPage::missiles)
	{
		for (std::uint8_t ship = 0; ship < loadout.available_ships; ++ship)
		{
			if (ship == loadout.selected_ship || (loadout.available_ship_mask & (1u << ship)) == 0)
			{
				continue;
			}
			const std::uint8_t slot = (12 - loadout.available_ships) / 2 + ship;
			const glm::vec3 position = loadout_selector_position(slot);
			const glm::mat4 transform = view * math::model_transform(
				loadout_selector_orientation(position), kLoadoutShipScales[ship] * 0.1944444444f, position);
			if (hit_model(catalog.ships[ship], transform, point, loadout.selector_lod))
			{
				return 16 + ship;
			}
		}
		return -1;
	}
	const render::MissionGpuModel& ship = catalog.ships[loadout.selected_ship];
	const glm::mat3 orientation = math::rotation_from_euler(
		{-glm::half_pi<float>(), glm::pi<float>(), 0.0f});
	const float scale = kLoadoutShipScales[loadout.selected_ship];
	constexpr glm::vec3 center{3.296874f, -2.250003f, 3.246874f};
	for (std::uint8_t index = 0; index < ship.hardpoints.size(); ++index)
	{
		if (loadout.missile_animation_active[index])
		{
			continue;
		}
		const assets::GameplayHardpoint& hardpoint = ship.hardpoints[index];
		const glm::vec3 position = center + orientation * hardpoint.position * scale;
		const glm::mat3 basis = orientation * hardpoint.basis;
		const std::int16_t missile = loadout.mounted_loadout[index];
		if (missile >= 0)
		{
			const glm::mat4 transform = view * math::model_transform(
				math::postrotate(basis, -glm::pi<float>(), {0.0f, 1.0f, 0.0f}), scale * 0.8f, position);
			if (hit_model(catalog.missiles[missile], transform, point, 0))
			{
				return 48 + index;
			}
		}
		else if (hit_quad(point, view * glm::translate(glm::mat4{1.0f},
			position + basis[1] * (0.05f * scale)), 0.7f, 1.12f))
		{
			return 48 + index;
		}
	}
	for (std::uint8_t missile = 0; missile < 10; ++missile)
	{
		if (!loadout_missile_available(loadout, missile))
		{
			continue;
		}
		const std::uint8_t slot = static_cast<std::uint8_t>(kLoadoutMissileSlots[loadout.missile_layout_tier][missile] - 1);
		const glm::vec3 position = loadout_selector_position(slot);
		const glm::mat4 transform = view * math::model_transform(
			loadout_selector_orientation(position), 0.007f, position);
		if (hit_model(catalog.missiles[missile], transform, point, 0))
		{
			return 32 + missile;
		}
	}
	return -1;
}

bool missile_available_impl(
	const Loadout& loadout,
	std::uint8_t missile)
{
	if (missile >= 10
		|| (loadout.available_missile_mask & (1u << missile)) == 0)
	{
		return false;
	}
	std::uint16_t mounted = 0;
	for (const std::int16_t item : loadout.mounted_loadout)
	{
		if (item == missile)
		{
			++mounted;
		}
	}
	for (std::uint8_t hardpoint = 0; hardpoint < 20; ++hardpoint)
	{
		if (loadout.missile_animation_active[hardpoint]
			&& loadout.missile_animation_item[hardpoint] == missile)
		{
			++mounted;
		}
	}
	return mounted < kMissileCapacities[missile];
}

void begin_missile_animation(
	Loadout& loadout,
	std::uint8_t hardpoint,
	std::uint8_t missile,
	bool removing,
	std::uint64_t now)
{
	loadout.missile_animation_active[hardpoint] = true;
	loadout.missile_animation_removing[hardpoint] = removing;
	loadout.missile_animation_item[hardpoint] = missile;
	loadout.missile_animation_at[hardpoint] = now;
}

bool missile_animation_running(const Loadout& loadout)
{
	return std::any_of(
		std::begin(loadout.missile_animation_active),
		std::end(loadout.missile_animation_active),
		[](bool active) { return active; });
}

void begin_page_transition(
	Loadout& loadout,
	LoadoutPage page,
	std::uint64_t now)
{
	loadout.requested_page = page;
	if (loadout.page == page)
	{
		return;
	}
	loadout.previous_page = loadout.page;
	// Retail routes gun <-> missile navigation through the ship view.
	loadout.page = loadout.page != LoadoutPage::ships
		? LoadoutPage::ships : page;
	loadout.page_transition_active = true;
	loadout.page_transition_at = now;
	if (loadout.previous_page != LoadoutPage::missiles)
	{
		loadout.previous_spin = static_cast<float>(now - loadout.spin_at)
			* kShipSpinRadiansPerMillisecond;
		loadout.hovered_missile = -1;
	}
	loadout.hovered_object = -1;
}

void begin_ship_selection(
	Loadout& loadout, const LoadoutCatalog& catalog,
	std::uint8_t ship, std::uint64_t now)
{
	loadout.previous_ship = loadout.selected_ship;
	loadout.selected_ship = ship;
	loadout.ship_selection_active = true;
	loadout.ship_selection_at = now;
	loadout.previous_spin = static_cast<float>(now - loadout.spin_at)
		* kShipSpinRadiansPerMillisecond;
	loadout.spin_at = now + kShipSelectionMilliseconds;
	std::fill(std::begin(loadout.mounted_loadout),
		std::end(loadout.mounted_loadout), static_cast<std::int16_t>(-1));
	std::fill(std::begin(loadout.missile_animation_active),
		std::end(loadout.missile_animation_active), false);
	const render::MissionGpuModel& definition = catalog.ships[ship];
	for (std::uint32_t hardpoint = 0; hardpoint < definition.hardpoints.size(); ++hardpoint)
	{
		loadout.mounted_loadout[hardpoint] = static_cast<std::int16_t>(
			definition.hardpoints[hardpoint].default_loadout[loadout.missile_layout_tier]);
	}
}

std::uint64_t page_transition_duration(
	const Loadout& loadout, const LoadoutCatalog& catalog)
{
	if (loadout.page == LoadoutPage::guns
		|| loadout.previous_page == LoadoutPage::guns)
	{
		return 1500;
	}
	const std::uint64_t count = catalog.ships[loadout.selected_ship].hardpoints.size();
	const std::uint64_t zoom = count == 0 ? 0 : kHardpointZoomMilliseconds + (count - 1) * 80;
	return loadout.page == LoadoutPage::missiles
		? std::max<std::uint64_t>(1000 + zoom, 800 + (loadout.available_ships + 8) * 56ull)
		: std::max<std::uint64_t>(zoom + 1000, 1600 + (loadout.available_ships - 2) * 56ull);
}

}

bool loadout_missile_available(
	const Loadout& loadout,
	std::uint8_t missile)
{
	return missile_available_impl(loadout, missile);
}

void loadout_catalog_init(
	LoadoutCatalog& catalog, const render::LoadoutRenderer& models,
	const assets::GameStats& stats)
{
	catalog.ships = models.ship_models;
	catalog.missiles = models.missile_models;
	build_ship_display_stats(catalog, stats.ships);
	build_missile_display_stats(catalog, stats.missiles);
}

void loadout_reset(
	Loadout& loadout,
	const campaign::CampaignState& campaign,
	const LoadoutCatalog& catalog,
	std::uint64_t now,
	std::uint8_t graphics_detail)
{
	loadout = {};
	loadout.selector_lod = 2 - graphics_detail;
	std::fill(
		std::begin(loadout.mounted_loadout),
		std::end(loadout.mounted_loadout),
		static_cast<std::int16_t>(-1));
	loadout.animation_at = now;
	loadout.entered_at = now;
	loadout.spin_at = now;
	loadout.mission = static_cast<std::uint8_t>(
		std::clamp<std::uint16_t>(campaign.mission, 1, 28));
	loadout.late_campaign = loadout.mission >= 19;
	const std::uint8_t progression = std::min<std::uint8_t>(
		campaign.progression, 3);
	loadout.missile_layout_tier = progression;
	const std::uint8_t rank = std::min<std::uint8_t>(campaign.rank, 8);
	loadout.available_ships = std::max(
		kProgressionShipCounts[progression],
		kRankShipCounts[rank]);
	loadout.available_ship_mask = static_cast<std::uint16_t>(
		(1u << loadout.available_ships) - 1u);
	for (std::uint8_t tier = 0; tier <= progression; ++tier)
	{
		loadout.available_missile_mask |= kMissileUnlockMasks[tier];
	}
	if (loadout.mission == 1)
	{
		loadout.selected_ship = 0;
	}
	else if (loadout.mission == 23)
	{
		// Retail keeps the normal twelve-ship layout but makes every selector
		// except the Shroud invisible and non-pickable.
		loadout.available_ships = 12;
		loadout.available_ship_mask = 1u << 10;
		loadout.selected_ship = 10;
	}
	else
	{
		loadout.selected_ship = static_cast<std::uint8_t>(
			std::clamp<std::int16_t>(
				campaign.selected_ship,
				0,
				static_cast<std::int16_t>(loadout.available_ships - 1)));
	}
	loadout.previous_ship = loadout.selected_ship;
	const render::MissionGpuModel& ship =
		catalog.ships[loadout.selected_ship];
	if (loadout.mission == 1 || loadout.mission == 23)
	{
		for (std::uint32_t hardpoint = 0;
			hardpoint < ship.hardpoints.size(); ++hardpoint)
		{
			loadout.mounted_loadout[hardpoint] = static_cast<std::int16_t>(
				ship.hardpoints[hardpoint].default_loadout[progression]);
		}
	}
	else
	{
		std::copy_n(campaign.loadout, ship.hardpoints.size(),
			loadout.mounted_loadout);
	}
}

void loadout_begin_exit(Loadout& loadout, std::uint64_t now)
{
	if (loadout.phase != LoadoutPhase::active
		|| loadout.ship_selection_active
		|| loadout.page_transition_active
		|| missile_animation_running(loadout))
	{
		return;
	}
	if (loadout.page == LoadoutPage::guns)
	{
		begin_page_transition(loadout, LoadoutPage::ships, now);
		return;
	}
	loadout.phase = LoadoutPhase::exiting;
	loadout.animation_at = now;
}

bool loadout_update(
	Loadout& loadout, const LoadoutCatalog& catalog, std::uint64_t now)
{
	for (std::uint8_t hardpoint = 0; hardpoint < 20; ++hardpoint)
	{
		if (!loadout.missile_animation_active[hardpoint])
		{
			continue;
		}
		if (now - loadout.missile_animation_at[hardpoint] < kMissileAnimationMilliseconds)
		{
			continue;
		}
		if (!loadout.missile_animation_removing[hardpoint])
		{
			loadout.mounted_loadout[hardpoint] =
				loadout.missile_animation_item[hardpoint];
		}
		loadout.missile_animation_active[hardpoint] = false;
	}
	if (loadout.ship_selection_active
		&& now - loadout.ship_selection_at >= kShipSelectionMilliseconds)
	{
		loadout.ship_selection_active = false;
		loadout.previous_ship = loadout.selected_ship;
		if (loadout.page != loadout.requested_page)
		{
			begin_page_transition(loadout, loadout.requested_page, now);
		}
	}
	if (loadout.page_transition_active
		&& now - loadout.page_transition_at >= page_transition_duration(loadout, catalog))
	{
		loadout.page_transition_active = false;
		if (loadout.previous_page == LoadoutPage::missiles)
		{
			loadout.spin_at = now;
		}
		loadout.previous_page = loadout.page;
		if (loadout.requested_ship >= 0)
		{
			const std::uint8_t ship = static_cast<std::uint8_t>(loadout.requested_ship);
			loadout.requested_ship = -1;
			begin_ship_selection(loadout, catalog, ship, now);
		}
		else if (loadout.page != loadout.requested_page)
		{
			begin_page_transition(loadout, loadout.requested_page, now);
		}
		else if (loadout.launch_requested)
		{
			loadout_begin_exit(loadout, now);
		}
	}
	if (loadout.phase == LoadoutPhase::entering
		&& now - loadout.animation_at >= kActivationMilliseconds)
	{
		loadout.phase = LoadoutPhase::active;
	}
	else if (loadout.phase == LoadoutPhase::exiting
		&& now - loadout.animation_at >= kActivationMilliseconds)
	{
		loadout.phase = LoadoutPhase::complete;
		return true;
	}
	return loadout.phase == LoadoutPhase::complete;
}

bool loadout_pointer(
	Loadout& loadout,
	const LoadoutCatalog& catalog,
	float x,
	float y,
	std::uint64_t now)
{
	const std::int16_t previous_object = loadout.hovered_object;
	loadout.pointer_x = x;
	loadout.pointer_y = y;
	loadout.hovered_object = -1;
	if (loadout.phase != LoadoutPhase::active
		|| loadout.page_transition_active
		|| loadout.ship_selection_active)
	{
		return false;
	}

	loadout.hovered_object = hit_loadout_object(loadout, catalog, {x, y}, now);
	if (loadout.hovered_object >= 32 && loadout.hovered_object < 42)
	{
		loadout.hovered_missile = static_cast<std::int8_t>(loadout.hovered_object - 32);
	}
	else if (loadout.hovered_object >= 48)
	{
		const std::int16_t missile = loadout.mounted_loadout[loadout.hovered_object - 48];
		if (missile >= 0)
		{
			loadout.hovered_missile = static_cast<std::int8_t>(missile);
		}
	}
	return loadout.hovered_object >= 0
		&& loadout.hovered_object != previous_object;
}

bool loadout_launch_button_hidden(const Loadout& loadout, std::uint64_t now)
{
	const std::uint64_t elapsed = now - loadout.entered_at;
	return loadout.mission == 1 && elapsed > 15000 && elapsed < 25000 && now % 500 < 250;
}

LoadoutClickResult loadout_launch(
	Loadout& loadout, campaign::CampaignState& campaign, std::uint64_t now)
{
	if (loadout.phase != LoadoutPhase::active
		|| loadout.ship_selection_active || loadout.page_transition_active
		|| loadout_launch_button_hidden(loadout, now))
	{
		return {};
	}
	LoadoutClickResult result{loadout_sound_button, false};
	if (missile_animation_running(loadout))
	{
		return result;
	}
	campaign.selected_ship = loadout.selected_ship;
	std::copy(std::begin(loadout.mounted_loadout), std::end(loadout.mounted_loadout),
		std::begin(campaign.loadout));
	loadout.launch_requested = true;
	if (loadout.page == LoadoutPage::guns)
	{
		result.sounds |= loadout_sound_info_flip;
	}
	result.launch = true;
	return result;
}

namespace
{
LoadoutClickResult activate_loadout_object(
	Loadout& loadout,
	campaign::CampaignState& campaign,
	const LoadoutCatalog& catalog,
	std::int16_t object,
	std::uint64_t now)
{
	LoadoutClickResult result;
	if (loadout.phase != LoadoutPhase::active
		|| loadout.ship_selection_active
		|| loadout.page_transition_active)
	{
		return result;
	}
	if (object == 2)
	{
		result.sounds |= loadout_sound_button;
		if (loadout.page == LoadoutPage::guns)
		{
			result.sounds |= loadout_sound_info_flip;
		}
		begin_page_transition(loadout, LoadoutPage::ships, now);
		return result;
	}
	if (object == 1)
	{
		result.sounds |= loadout_sound_button;
		if (loadout.page == LoadoutPage::guns)
		{
			result.sounds |= loadout_sound_info_flip;
		}
		begin_page_transition(loadout, LoadoutPage::missiles, now);
		return result;
	}
	if (object == 3)
	{
		result.sounds |= loadout_sound_button;
		if (loadout.page != LoadoutPage::missiles)
		{
			result.sounds |= loadout_sound_info_flip;
		}
		begin_page_transition(loadout,
			loadout.page == LoadoutPage::guns
				? LoadoutPage::ships : LoadoutPage::guns, now);
		return result;
	}
	if (object == 0)
	{
		return loadout_launch(loadout, campaign, now);
	}

	if (loadout.page == LoadoutPage::missiles
		&& object == 4)
	{
		result.sounds |= loadout_sound_button;
		if (missile_animation_running(loadout))
		{
			return result;
		}
		std::fill(
			std::begin(loadout.mounted_loadout),
			std::end(loadout.mounted_loadout),
			static_cast<std::int16_t>(-1));
		const render::MissionGpuModel& ship = catalog.ships[loadout.selected_ship];
		for (std::uint8_t hardpoint = 0; hardpoint < ship.hardpoints.size(); ++hardpoint)
		{
			const std::int16_t missile = static_cast<std::int16_t>(
				ship.hardpoints[hardpoint].default_loadout[0]);
			if (missile < 0 || missile >= 10)
			{
				continue;
			}
			loadout.hovered_missile = static_cast<std::int8_t>(missile);
			begin_missile_animation(
				loadout,
				hardpoint,
				static_cast<std::uint8_t>(missile),
				false, now);
		}
		return result;
	}
	if (loadout.page == LoadoutPage::missiles
		&& object == 5)
	{
		result.sounds |= loadout_sound_button;
		for (std::int16_t& item : loadout.mounted_loadout)
		{
			item = -1;
		}
		std::fill(
			std::begin(loadout.missile_animation_active),
			std::end(loadout.missile_animation_active),
			false);
		return result;
	}
	if (loadout.page == LoadoutPage::ships || loadout.page == LoadoutPage::guns)
	{
		if (object >= 16 && object < 28)
		{
			const std::uint8_t closest_ship = static_cast<std::uint8_t>(object - 16);
			campaign.selected_ship = closest_ship;
			if (loadout.page == LoadoutPage::guns)
			{
				result.sounds |= loadout_sound_info_flip;
				begin_page_transition(loadout, LoadoutPage::ships, now);
				loadout.requested_page = LoadoutPage::guns;
				loadout.requested_ship = static_cast<std::int8_t>(closest_ship);
			}
			else
			{
				result.sounds |= loadout_sound_ship;
				begin_ship_selection(loadout, catalog, closest_ship, now);
			}
		}
		return result;
	}
	else if (loadout.page == LoadoutPage::missiles)
	{
		const render::MissionGpuModel& ship =
			catalog.ships[loadout.selected_ship];
		const std::int32_t closest_hardpoint = object >= 48 ? object - 48 : -1;
		if (closest_hardpoint >= 0)
		{
			const std::int16_t mounted =
				loadout.mounted_loadout[closest_hardpoint];
			if (mounted >= 0)
			{
				result.sounds |= loadout_sound_hardpoint;
				loadout.mounted_loadout[closest_hardpoint] = -1;
				begin_missile_animation(
					loadout,
					static_cast<std::uint8_t>(closest_hardpoint),
					static_cast<std::uint8_t>(mounted),
					true, now);
			}
			else if (loadout.hovered_missile >= 0
				&& loadout_missile_available(
					loadout,
					static_cast<std::uint8_t>(
						loadout.hovered_missile)))
			{
				result.sounds |= loadout_sound_hardpoint;
				const std::uint8_t missile =
					static_cast<std::uint8_t>(
						loadout.hovered_missile);
				begin_missile_animation(
					loadout,
					static_cast<std::uint8_t>(closest_hardpoint),
					missile,
					false, now);
			}
			return result;
		}

		const std::int8_t closest_missile = object >= 32 && object < 42
			? static_cast<std::int8_t>(object - 32) : -1;
		if (closest_missile >= 0)
		{
			const std::uint8_t missile =
				static_cast<std::uint8_t>(closest_missile);
			loadout.hovered_missile = closest_missile;
			bool attached = false;
			for (std::uint8_t hardpoint = 0;
				hardpoint < ship.hardpoints.size();
				++hardpoint)
			{
				if (loadout.mounted_loadout[hardpoint] < 0
					&& !loadout.missile_animation_active[hardpoint])
				{
					begin_missile_animation(
						loadout,
						hardpoint,
						missile,
						false, now);
					attached = true;
					break;
				}
			}
			if (attached)
			{
				result.sounds |= loadout_sound_missile;
			}
			return result;
		}
	}
	return result;
}
}

LoadoutClickResult loadout_press(
	Loadout& loadout, campaign::CampaignState& campaign,
	const LoadoutCatalog& catalog, float x, float y, std::uint64_t now)
{
	if (loadout.phase != LoadoutPhase::active || loadout.page_transition_active
		|| loadout.ship_selection_active)
	{
		return {};
	}
	const std::int8_t button = hit_button(loadout, {x, y}, now);
	if (button >= 0)
	{
		loadout.pressed_button = button;
		return {loadout_sound_button, false};
	}
	return activate_loadout_object(loadout, campaign, catalog,
		hit_loadout_object(loadout, catalog, {x, y}, now), now);
}

LoadoutClickResult loadout_release(
	Loadout& loadout, campaign::CampaignState& campaign,
	const LoadoutCatalog& catalog, float x, float y, std::uint64_t now)
{
	const std::int8_t button = loadout.pressed_button;
	const std::int8_t released = hit_button(loadout, {x, y}, now);
	loadout.pressed_button = -1;
	if (button < 0 || released != button)
	{
		return {};
	}
	LoadoutClickResult result = activate_loadout_object(loadout, campaign, catalog, button, now);
	result.sounds &= ~loadout_sound_button;
	return result;
}

}

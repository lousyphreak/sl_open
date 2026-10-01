#include "frontend/loadout.hpp"

#include "assets/game_stats.hpp"
#include "assets/player_ship.hpp"
#include "assets/ship_model.hpp"
#include "campaign/campaign.hpp"
#include "frontend/gui.hpp"

#include <algorithm>
#include <bit>
#include <chrono>
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
// Retail table at 0x004ea480. Values index the 13-point layout directly;
// zero is the enlarged center position, so carousel slots start at one.
constexpr std::int8_t kMissileCarouselSlots[4][10] = {
	{3, -1, 4, 5, 7, -1, -1, -1, -1, 6},
	{2, 9, 3, 4, 8, 6, -1, 7, -1, 5},
	{1, 8, 2, 3, 7, 5, -1, 6, 9, 4},
	{1, 8, 2, 3, 7, 5, 10, 6, 9, 4},
};
constexpr std::uint16_t kMissileCapacities[10] = {
	999, 999, 999, 3, 999, 999, 999, 999, 999, 999};
constexpr std::uint64_t kActivationMilliseconds = 2000;
constexpr std::uint64_t kShipSelectionMilliseconds = 1500;
constexpr std::uint64_t kPageTransitionMilliseconds = 1000;
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

std::uint64_t animation_clock_milliseconds()
{
	return static_cast<std::uint64_t>(
		std::chrono::duration_cast<std::chrono::milliseconds>(
			std::chrono::steady_clock::now().time_since_epoch()).count());
}
constexpr float kShipDisplayScales[12] = {
	0.009f, 0.010f, 0.008f, 0.009f,
	0.0084f, 0.010f, 0.007f, 0.0067f,
	0.006f, 0.0071f, 0.006f, 0.008f,
};
constexpr glm::vec2 kShipSelectorCenters[12] = {
	{235.9f, 267.2f}, {227.4f, 289.5f}, {236.9f, 314.8f},
	{264.2f, 339.5f}, {312.7f, 359.9f}, {380.7f, 373.8f},
	{456.3f, 369.7f}, {507.9f, 350.0f}, {531.4f, 327.0f},
	{535.7f, 302.1f}, {519.8f, 278.4f}, {493.6f, 258.3f},
};
constexpr gui::Rect kShipsPageRegion{570.0f, 132.0f, 66.0f, 61.0f};
constexpr gui::Rect kMissilesPageRegion{570.0f, 73.0f, 66.0f, 61.0f};
constexpr gui::Rect kGunsPageRegion{570.0f, 191.0f, 66.0f, 61.0f};
constexpr gui::Rect kLaunchRegion{570.0f, 14.0f, 66.0f, 61.0f};
constexpr gui::Rect kDefaultLoadoutRegion{437.0f, 14.0f, 56.0f, 61.0f};
constexpr gui::Rect kClearLoadoutRegion{497.0f, 14.0f, 55.0f, 61.0f};

std::int8_t missile_carousel_index(
	const Loadout& loadout,
	std::uint8_t missile)
{
	if (missile >= 10)
	{
		return -1;
	}
	const std::int8_t slot =
		kMissileCarouselSlots[loadout.missile_layout_tier][missile];
	return slot > 0
			&& (loadout.available_missile_mask & (1u << missile)) != 0
		? static_cast<std::int8_t>(slot - 1)
		: -1;
}

bool project_missile_hardpoint(
	const LoadoutHardpointDefinition& hardpoint,
	std::uint8_t ship,
	float& screen_x,
	float& screen_y)
{
	constexpr glm::vec3 center{
		3.296874f, -2.250003f, 3.246874f};
	const float scale = kShipDisplayScales[std::min<std::uint8_t>(ship, 11)];
	// Retail raises the screen-facing hardpoint panel 0.05 units along
	// the hardpoint's local Y axis before applying the ship transform.
	const glm::vec3 panel =
		hardpoint.position + hardpoint.basis[1] * 0.05f;
	// Retail's missile page target is Euler (-pi/2, pi, 0).
	const glm::vec3 world{
		center.x - panel.x * scale,
		center.y - panel.z * scale,
		center.z + panel.y * scale,
	};
	constexpr glm::vec3 eye{-2.8f, -6.5f, -16.85f};
	constexpr glm::vec3 right{
		0.99776034f, 0.0f, -0.06699589f};
	constexpr glm::vec3 up{
		-0.02404717f, 0.93335575f, -0.35813984f};
	constexpr glm::vec3 forward{
		0.06694987f, 0.35894290f, 0.93095529f};
	const glm::vec3 relative = world - eye;
	const float view_x = glm::dot(relative, right);
	const float view_y = glm::dot(relative, up);
	const float view_z = glm::dot(relative, forward);
	if (view_z <= 0.0f)
	{
		return false;
	}
	const float ndc_x =
		view_x * ((639.0f * 0.6f) / 320.0f) / view_z;
	const float ndc_y =
		-view_y * ((479.0f * 0.8f) / 240.0f) / view_z;
	screen_x = (ndc_x + 1.0f) * 320.0f;
	screen_y = (1.0f - ndc_y) * 240.0f;
	return true;
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
	bool removing)
{
	loadout.missile_animation_active[hardpoint] = true;
	loadout.missile_animation_removing[hardpoint] = removing;
	loadout.missile_animation_item[hardpoint] = missile;
	loadout.missile_animation_at[hardpoint] =
		animation_clock_milliseconds();
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
	if (loadout.page == page)
	{
		return;
	}
	loadout.previous_page = loadout.page;
	loadout.page = page;
	loadout.page_transition_active = true;
	loadout.page_transition_at = now;
}


}

bool loadout_missile_available(
	const Loadout& loadout,
	std::uint8_t missile)
{
	return missile_available_impl(loadout, missile);
}

void loadout_catalog_init(
	LoadoutCatalog& catalog,
	const assets::ShipModel (&ships)[12],
	const assets::GameStats& stats)
{
	catalog = {};
	for (std::uint32_t ship_index = 0; ship_index < 12; ++ship_index)
	{
		const assets::ShipModel& source = ships[ship_index];
		LoadoutShipDefinition& destination = catalog.ships[ship_index];
		destination.hardpoint_count = std::min<std::uint32_t>(
			source.hardpoint_count, 20);
		for (std::uint32_t hardpoint = 0;
			hardpoint < destination.hardpoint_count;
			++hardpoint)
		{
			LoadoutHardpointDefinition& output =
				destination.hardpoints[hardpoint];
			output.position = source.hardpoints[hardpoint].position;
			output.basis = source.hardpoints[hardpoint].basis;
			std::copy(
				std::begin(source.hardpoints[hardpoint].default_loadout),
				std::end(source.hardpoints[hardpoint].default_loadout),
				std::begin(output.default_loadout));
		}
	}
	build_ship_display_stats(catalog, stats.ships);
	build_missile_display_stats(catalog, stats.missiles);
}

void loadout_reset(
	Loadout& loadout,
	const campaign::CampaignState& campaign,
	const LoadoutCatalog& catalog,
	std::uint64_t now)
{
	loadout = {};
	std::fill(
		std::begin(loadout.default_loadout),
		std::end(loadout.default_loadout),
		static_cast<std::int16_t>(-1));
	std::fill(
		std::begin(loadout.mounted_loadout),
		std::end(loadout.mounted_loadout),
		static_cast<std::int16_t>(-1));
	loadout.animation_at = now;
	loadout.spin_at = now;
	loadout.mission = static_cast<std::uint8_t>(
		std::clamp<std::uint16_t>(campaign.mission, 1, 28));
	loadout.late_campaign = loadout.mission >= 19;
	const std::uint8_t difficulty = std::min<std::uint8_t>(
		static_cast<std::uint8_t>(campaign.difficulty), 2);
	loadout.difficulty = difficulty;
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
	loadout.available_missiles = static_cast<std::uint8_t>(
		std::popcount(loadout.available_missile_mask));
	for (std::uint8_t missile = 0; missile < 10; ++missile)
	{
		if ((loadout.available_missile_mask & (1u << missile)) != 0)
		{
			loadout.selected_missile = missile;
			break;
		}
	}
	if (loadout.mission == 1)
	{
		loadout.selected_ship = 0;
		loadout.use_default_loadout = true;
	}
	else if (loadout.mission == 23)
	{
		// Retail keeps the normal twelve-ship layout but makes every selector
		// except the Shroud invisible and non-pickable.
		loadout.available_ships = 12;
		loadout.available_ship_mask = 1u << 10;
		loadout.selected_ship = 10;
		loadout.use_default_loadout = true;
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
	const LoadoutShipDefinition& ship =
		catalog.ships[loadout.selected_ship];
	for (std::uint32_t hardpoint = 0;
		hardpoint < ship.hardpoint_count;
		++hardpoint)
	{
		loadout.default_loadout[hardpoint] =
			static_cast<std::int16_t>(
				ship.hardpoints[hardpoint].default_loadout[
					loadout.missile_layout_tier]);
	}
	if (loadout.use_default_loadout)
	{
		for (std::uint32_t hardpoint = 0;
			hardpoint < ship.hardpoint_count;
			++hardpoint)
		{
			loadout.mounted_loadout[hardpoint] =
				loadout.default_loadout[hardpoint];
		}
		loadout.use_default_loadout = false;
	}
	else
	{
		std::copy(
			std::begin(campaign.loadout),
			std::end(campaign.loadout),
			std::begin(loadout.mounted_loadout));
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
	loadout.phase = LoadoutPhase::exiting;
	loadout.animation_at = now;
}

bool loadout_update(Loadout& loadout, std::uint64_t now)
{
	const std::uint64_t animation_now = animation_clock_milliseconds();
	for (std::uint8_t hardpoint = 0; hardpoint < 20; ++hardpoint)
	{
		if (!loadout.missile_animation_active[hardpoint])
		{
			continue;
		}
		if (animation_now <= loadout.missile_animation_at[hardpoint]
			|| animation_now - loadout.missile_animation_at[hardpoint]
				< kMissileAnimationMilliseconds)
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
	}
	if (loadout.page_transition_active
		&& now - loadout.page_transition_at >= kPageTransitionMilliseconds)
	{
		loadout.page_transition_active = false;
		loadout.previous_page = loadout.page;
	}
	if (loadout.phase == LoadoutPhase::entering
		&& now - loadout.animation_at >= kActivationMilliseconds)
	{
		loadout.phase = LoadoutPhase::active;
		loadout.animation_at = now;
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
	float y)
{
	const std::int16_t previous_object = loadout.hovered_object;
	loadout.pointer_x = x;
	loadout.pointer_y = y;
	loadout.hovered_hardpoint = -1;
	loadout.hovered_object = -1;
	if (loadout.phase != LoadoutPhase::active
		|| loadout.page_transition_active)
	{
		return false;
	}

	const gui::Rect button_regions[] = {
		kLaunchRegion,
		kMissilesPageRegion,
		kShipsPageRegion,
		kGunsPageRegion,
		kDefaultLoadoutRegion,
		kClearLoadoutRegion,
	};
	const std::uint8_t button_count =
		loadout.page == LoadoutPage::missiles ? 6 : 4;
	for (std::uint8_t button = 0; button < button_count; ++button)
	{
		if (gui::hit_half_open(button_regions[button], x, y))
		{
			loadout.hovered_object = button;
			return loadout.hovered_object != previous_object;
		}
	}

	if (loadout.page == LoadoutPage::ships)
	{
		float closest_distance = 45.0f * 45.0f;
		for (std::uint8_t ship = 0; ship < loadout.available_ships; ++ship)
		{
			if (ship == loadout.selected_ship
				|| (loadout.available_ship_mask & (1u << ship)) == 0)
			{
				continue;
			}
			const std::uint8_t ring_index = static_cast<std::uint8_t>(
				(12 - loadout.available_ships) / 2 + ship);
			const glm::vec2 delta =
				glm::vec2{x, y} - kShipSelectorCenters[ring_index];
			const float distance = glm::dot(delta, delta);
			if (distance < closest_distance)
			{
				closest_distance = distance;
				loadout.hovered_object =
					static_cast<std::int16_t>(16 + ship);
			}
		}
		return loadout.hovered_object >= 0
			&& loadout.hovered_object != previous_object;
	}

	if (loadout.page != LoadoutPage::missiles)
	{
		return false;
	}
	const LoadoutShipDefinition& ship =
		catalog.ships[loadout.selected_ship];
	float closest_distance = 18.0f * 18.0f;
	std::int8_t hovered = -1;
	for (std::uint32_t hardpoint = 0;
		hardpoint < ship.hardpoint_count;
		++hardpoint)
	{
		const std::int16_t missile = loadout.mounted_loadout[hardpoint];
		float hardpoint_x;
		float hardpoint_y;
		if (!project_missile_hardpoint(
				ship.hardpoints[hardpoint],
				loadout.selected_ship,
				hardpoint_x,
				hardpoint_y))
		{
			continue;
		}
		const float dx = x - hardpoint_x;
		const float dy = y - hardpoint_y;
		const float distance = dx * dx + dy * dy;
		if (distance < closest_distance)
		{
			closest_distance = distance;
			loadout.hovered_object =
				static_cast<std::int16_t>(48 + hardpoint);
			if (missile >= 0 && missile < 10)
			{
				hovered = static_cast<std::int8_t>(missile);
				loadout.hovered_hardpoint =
					static_cast<std::int8_t>(hardpoint);
			}
		}
	}
	for (std::uint8_t missile = 0; missile < 10; ++missile)
	{
		if (!loadout_missile_available(loadout, missile))
		{
			continue;
		}
		const std::int8_t ring_index =
			missile_carousel_index(loadout, missile);
		if (ring_index < 0)
		{
			continue;
		}
		const glm::vec2 delta =
			glm::vec2{x, y} - kShipSelectorCenters[ring_index];
		const float distance = glm::dot(delta, delta);
		if (distance < closest_distance && distance < 45.0f * 45.0f)
		{
			closest_distance = distance;
			hovered = static_cast<std::int8_t>(missile);
			loadout.hovered_hardpoint = -1;
			loadout.hovered_object =
				static_cast<std::int16_t>(32 + missile);
		}
	}
	if (hovered >= 0)
	{
		loadout.hovered_missile = hovered;
	}
	return loadout.hovered_object >= 0
		&& loadout.hovered_object != previous_object;
}

LoadoutClickResult loadout_click(
	Loadout& loadout,
	campaign::CampaignState& campaign,
	const LoadoutCatalog& catalog,
	float x,
	float y,
	std::uint64_t now)
{
	LoadoutClickResult result;
	if (loadout.phase != LoadoutPhase::active
		|| loadout.ship_selection_active
		|| loadout.page_transition_active)
	{
		return result;
	}
	if (gui::hit_half_open(kShipsPageRegion, x, y))
	{
		result.sounds |= loadout_sound_button;
		if (loadout.page == LoadoutPage::guns)
		{
			result.sounds |= loadout_sound_info_flip;
		}
		begin_page_transition(loadout, LoadoutPage::ships, now);
		return result;
	}
	if (gui::hit_half_open(kMissilesPageRegion, x, y))
	{
		result.sounds |= loadout_sound_button;
		if (loadout.page == LoadoutPage::guns)
		{
			result.sounds |= loadout_sound_info_flip;
		}
		begin_page_transition(loadout, LoadoutPage::missiles, now);
		return result;
	}
	if (gui::hit_half_open(kGunsPageRegion, x, y))
	{
		result.sounds |= loadout_sound_button;
		if (loadout.page != LoadoutPage::guns)
		{
			result.sounds |= loadout_sound_info_flip;
		}
		begin_page_transition(loadout, LoadoutPage::guns, now);
		return result;
	}
	if (gui::hit_half_open(kLaunchRegion, x, y))
	{
		result.sounds |= loadout_sound_button;
		if (missile_animation_running(loadout))
		{
			return result;
		}
		campaign.selected_ship = loadout.selected_ship;
		std::copy(
			std::begin(loadout.mounted_loadout),
			std::end(loadout.mounted_loadout),
			std::begin(campaign.loadout));
		loadout.launch_requested = true;
		result.launch = true;
		return result;
	}
	if (loadout.page == LoadoutPage::missiles
		&& gui::hit_half_open(kDefaultLoadoutRegion, x, y))
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
		std::fill(
			std::begin(loadout.missile_animation_active),
			std::end(loadout.missile_animation_active),
			false);
		for (std::uint8_t hardpoint = 0; hardpoint < 20; ++hardpoint)
		{
			const std::int16_t missile =
				loadout.default_loadout[hardpoint];
			if (missile < 0 || missile >= 10)
			{
				continue;
			}
			begin_missile_animation(
				loadout,
				hardpoint,
				static_cast<std::uint8_t>(missile),
				false);
		}
		loadout.use_default_loadout = false;
		return result;
	}
	if (loadout.page == LoadoutPage::missiles
		&& gui::hit_half_open(kClearLoadoutRegion, x, y))
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
	if (loadout.page == LoadoutPage::ships)
	{
		float closest_distance = 45.0f * 45.0f;
		std::uint8_t closest_ship = loadout.selected_ship;
		for (std::uint8_t ship = 0; ship < loadout.available_ships; ++ship)
		{
			if (ship == loadout.selected_ship
				|| (loadout.available_ship_mask & (1u << ship)) == 0)
			{
				continue;
			}
			const std::uint8_t ring_index = static_cast<std::uint8_t>(
				(12 - loadout.available_ships) / 2 + ship);
			const glm::vec2 delta =
				glm::vec2{x, y} - kShipSelectorCenters[ring_index];
			const float distance = glm::dot(delta, delta);
			if (distance < closest_distance)
			{
				closest_distance = distance;
				closest_ship = ship;
			}
		}
		if (closest_ship != loadout.selected_ship)
		{
			result.sounds |= loadout_sound_ship;
			loadout.previous_ship = loadout.selected_ship;
			loadout.selected_ship = closest_ship;
			loadout.ship_selection_active = true;
			loadout.ship_selection_at = now;
			loadout.previous_spin = static_cast<float>(
				now - loadout.spin_at) * kShipSpinRadiansPerMillisecond;
			loadout.spin_at = now + kShipSelectionMilliseconds;
			campaign.selected_ship = closest_ship;
			std::fill(
				std::begin(loadout.default_loadout),
				std::end(loadout.default_loadout),
				static_cast<std::int16_t>(-1));
			std::fill(
				std::begin(loadout.mounted_loadout),
				std::end(loadout.mounted_loadout),
				static_cast<std::int16_t>(-1));
			std::fill(
				std::begin(loadout.missile_animation_active),
				std::end(loadout.missile_animation_active),
				false);
			const LoadoutShipDefinition& ship =
				catalog.ships[closest_ship];
			for (std::uint32_t hardpoint = 0;
				hardpoint < ship.hardpoint_count;
				++hardpoint)
			{
				const std::int16_t item = static_cast<std::int16_t>(
					ship.hardpoints[hardpoint].default_loadout[
						loadout.missile_layout_tier]);
				loadout.default_loadout[hardpoint] = item;
				loadout.mounted_loadout[hardpoint] = item;
			}
		}
		return result;
	}
	else if (loadout.page == LoadoutPage::missiles)
	{
		const LoadoutShipDefinition& ship =
			catalog.ships[loadout.selected_ship];
		float closest_hardpoint_distance = 18.0f * 18.0f;
		std::int32_t closest_hardpoint = -1;
		for (std::uint32_t hardpoint = 0;
			hardpoint < ship.hardpoint_count;
			++hardpoint)
		{
			float hardpoint_x;
			float hardpoint_y;
			if (!project_missile_hardpoint(
					ship.hardpoints[hardpoint],
					loadout.selected_ship,
					hardpoint_x,
					hardpoint_y))
			{
				continue;
			}
			const float dx = x - hardpoint_x;
			const float dy = y - hardpoint_y;
			const float distance = dx * dx + dy * dy;
			if (distance < closest_hardpoint_distance)
			{
				closest_hardpoint_distance = distance;
				closest_hardpoint =
					static_cast<std::int32_t>(hardpoint);
			}
		}
		if (closest_hardpoint >= 0)
		{
			if (loadout.missile_animation_active[closest_hardpoint])
			{
				return result;
			}
			const std::int16_t mounted =
				loadout.mounted_loadout[closest_hardpoint];
			if (mounted >= 0)
			{
				result.sounds |= loadout_sound_hardpoint;
				loadout.selected_missile =
					static_cast<std::uint8_t>(mounted);
				loadout.mounted_loadout[closest_hardpoint] = -1;
				begin_missile_animation(
					loadout,
					static_cast<std::uint8_t>(closest_hardpoint),
					static_cast<std::uint8_t>(mounted),
					true);
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
					false);
			}
			loadout.use_default_loadout = false;
			return result;
		}

		float closest_missile_distance = 45.0f * 45.0f;
		std::int8_t closest_missile = -1;
		for (std::uint8_t missile = 0; missile < 10; ++missile)
		{
			if (!loadout_missile_available(loadout, missile))
			{
				continue;
			}
			const std::int8_t ring_index =
				missile_carousel_index(loadout, missile);
			if (ring_index < 0)
			{
				continue;
			}
			const glm::vec2 delta =
				glm::vec2{x, y} - kShipSelectorCenters[ring_index];
			const float distance = glm::dot(delta, delta);
			if (distance >= closest_missile_distance)
			{
				continue;
			}
			closest_missile_distance = distance;
			closest_missile = static_cast<std::int8_t>(missile);
		}
		if (closest_missile >= 0)
		{
			const std::uint8_t missile =
				static_cast<std::uint8_t>(closest_missile);
			loadout.selected_missile = missile;
			bool attached = false;
			for (std::uint8_t hardpoint = 0;
				hardpoint < 20;
				++hardpoint)
			{
				if (loadout.mounted_loadout[hardpoint] < 0
					&& !loadout.missile_animation_active[hardpoint])
				{
					begin_missile_animation(
						loadout,
						hardpoint,
						missile,
						false);
					loadout.use_default_loadout = false;
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

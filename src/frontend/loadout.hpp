#pragma once

#include "core/math.hpp"

#include <cstdint>

namespace sl_open
{
struct LanguageTable;
}

namespace sl_open::assets
{
struct GameStats;
}

namespace sl_open::campaign
{
struct CampaignState;
}

namespace sl_open::render
{
struct FrontendCommands;
struct FrontendRenderer;
struct LoadoutRenderer;
struct MissionGpuModel;
}

namespace sl_open::frontend
{
enum class LoadoutPage : std::uint8_t
{
	ships,
	missiles,
	guns,
};

enum class LoadoutPhase : std::uint8_t
{
	entering,
	active,
	exiting,
	complete,
};

enum LoadoutSound : std::uint16_t
{
	loadout_sound_none = 0,
	loadout_sound_button = 1 << 0,
	loadout_sound_missile = 1 << 1,
	loadout_sound_hardpoint = 1 << 2,
	loadout_sound_ship = 1 << 3,
	loadout_sound_info_flip = 1 << 4,
};

struct LoadoutClickResult
{
	std::uint16_t sounds{};
	bool launch{};
};

struct Loadout
{
	LoadoutPhase phase{LoadoutPhase::entering};
	LoadoutPage page{LoadoutPage::ships};
	LoadoutPage previous_page{LoadoutPage::ships};
	LoadoutPage requested_page{LoadoutPage::ships};
	std::uint8_t mission{1};
	std::uint8_t selected_ship{};
	std::uint8_t previous_ship{};
	std::int8_t hovered_missile{-1};
	std::int16_t hovered_object{-1};
	std::int8_t pressed_button{-1};
	std::int8_t requested_ship{-1};
	std::uint8_t available_ships{4};
	std::uint16_t available_ship_mask{0x000f};
	std::uint8_t missile_layout_tier{};
	std::uint8_t selector_lod{};
	std::uint16_t available_missile_mask{};
	float pointer_x{};
	float pointer_y{};
	bool late_campaign{};
	bool launch_requested{};
	bool ship_selection_active{};
	bool page_transition_active{};
	std::uint64_t animation_at{};
	std::uint64_t entered_at{};
	std::uint64_t ship_selection_at{};
	std::uint64_t page_transition_at{};
	std::uint64_t spin_at{};
	float previous_spin{};
	std::int16_t mounted_loadout[20]{};
	bool missile_animation_active[20]{};
	bool missile_animation_removing[20]{};
	std::uint8_t missile_animation_item[20]{};
	std::uint64_t missile_animation_at[20]{};
};

struct LoadoutShipStats
{
	std::uint8_t max_speed_rating{};
	std::uint8_t acceleration_rating{};
	std::uint8_t agility_rating{};
	std::uint8_t shield_power_rating{};
	std::uint8_t shield_recharge_rating{};
	std::uint8_t armor_rating{};
	std::uint16_t afterburner_seconds{};
};

struct LoadoutMissileStats
{
	std::int8_t lock_seconds{};
	std::int8_t speed_rating{};
	std::int8_t travel_rating{};
	std::int8_t damage_rating{};
};

struct LoadoutCatalog
{
	const render::MissionGpuModel* ships{};
	const render::MissionGpuModel* missiles{};
	LoadoutShipStats ship_stats[12];
	LoadoutMissileStats missile_stats[10];
};

void loadout_catalog_init(
	LoadoutCatalog& catalog,
	const render::LoadoutRenderer& models,
	const assets::GameStats& stats);
void loadout_reset(
	Loadout& loadout,
	const campaign::CampaignState& campaign,
	const LoadoutCatalog& catalog,
	std::uint64_t now,
	std::uint8_t graphics_detail);
void loadout_begin_exit(Loadout& loadout, std::uint64_t now);
bool loadout_update(
	Loadout& loadout, const LoadoutCatalog& catalog, std::uint64_t now);
bool loadout_missile_available(
	const Loadout& loadout,
	std::uint8_t missile);
bool loadout_pointer(
	Loadout& loadout,
	const LoadoutCatalog& catalog,
	float x,
	float y,
	std::uint64_t now);
bool loadout_launch_button_hidden(const Loadout& loadout, std::uint64_t now);
LoadoutClickResult loadout_launch(
	Loadout& loadout, campaign::CampaignState& campaign, std::uint64_t now);
LoadoutClickResult loadout_press(
	Loadout& loadout,
	campaign::CampaignState& campaign,
	const LoadoutCatalog& catalog,
	float x,
	float y,
	std::uint64_t now);
LoadoutClickResult loadout_release(
	Loadout& loadout,
	campaign::CampaignState& campaign,
	const LoadoutCatalog& catalog,
	float x,
	float y,
	std::uint64_t now);
void loadout_build(
	const Loadout& loadout,
	const LoadoutCatalog& catalog,
	const LanguageTable& language,
	const render::FrontendRenderer& renderer,
	render::FrontendCommands& commands,
	std::uint64_t now);
}

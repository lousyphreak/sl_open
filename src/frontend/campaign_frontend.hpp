#pragma once

#include "campaign/campaign.hpp"

#include <cstddef>
#include <cstdint>

namespace sl_open
{
struct LanguageTable;
}

namespace sl_open::render
{
struct FrontendCommands;
struct FrontendRenderer;
}

namespace sl_open::frontend
{
enum class CampaignScreen : std::uint8_t
{
	single_player,
	save_load,
	campaign_ready,
};

enum class CampaignModal : std::uint8_t
{
	none,
	difficulty,
	quit,
};

enum class CampaignSelection : std::uint8_t
{
	none,
	main_menu,
	quit,
	single_player,
	save_load,
	new_campaign,
	campaign_ready,
	save_complete,
};

enum class SaveLoadMode : std::uint8_t
{
	load,
	save,
};

struct CampaignFrontend
{
	CampaignScreen screen{CampaignScreen::single_player};
	CampaignModal modal{CampaignModal::none};
	float pointer_x{320.0f};
	float pointer_y{200.0f};
	std::int32_t hovered{-1};
	std::int32_t selected_campaign{-1};
	std::int32_t selected_save{-1};
	std::uint32_t save_origin{};
	std::uint64_t entered_at{};
	char callsign[campaign::kCallsignBytes]{};
	campaign::Pilot pilot{campaign::Pilot::female};
	campaign::Difficulty difficulty{campaign::Difficulty::medium};
	bool callsign_editing{true};
	bool campaign_list_open{};
	bool save_load_in_game{};
	bool save_name_editing{};
	SaveLoadMode save_load_mode{SaveLoadMode::load};
	char save_name[campaign::kSaveNameBytes]{};
	campaign::SaveList saves;
};

void campaign_frontend_init(
	CampaignFrontend& frontend,
	const campaign::CampaignStore& store,
	const char* default_callsign,
	std::uint64_t now);
void campaign_frontend_enter_single_player(
	CampaignFrontend& frontend,
	const campaign::CampaignStore& store,
	const char* default_callsign,
	std::uint64_t now);
void campaign_frontend_enter_save_load(
	CampaignFrontend& frontend,
	const campaign::CampaignStore& store,
	const campaign::CampaignState& active,
	SaveLoadMode mode,
	bool in_game,
	std::uint64_t now);
void campaign_frontend_set_pointer(
	CampaignFrontend& frontend,
	float x,
	float y,
	bool inside);
void campaign_frontend_text(
	CampaignFrontend& frontend,
	const char* text);
void campaign_frontend_backspace(CampaignFrontend& frontend);
CampaignSelection campaign_frontend_back(CampaignFrontend& frontend);
CampaignSelection campaign_frontend_select(
	CampaignFrontend& frontend,
	campaign::CampaignStore& store,
	campaign::CampaignState& campaign);
void campaign_frontend_build(
	const CampaignFrontend& frontend,
	const campaign::CampaignStore& store,
	const campaign::CampaignState& campaign,
	const LanguageTable& language,
	const render::FrontendRenderer& renderer,
	render::FrontendCommands& commands,
	std::uint64_t now);
}

#include "frontend/campaign_frontend.hpp"

#include "frontend/campaign_save_browser.hpp"
#include "frontend/gui.hpp"
#include "frontend/quit_dialog.hpp"

#include <cstdio>
#include <cstring>

namespace sl_open::frontend
{
namespace
{
struct Region
{
	float x;
	float y;
	float width;
	float height;
};

constexpr Region kSingleRegions[] = {
	{62.0f, 145.0f, 111.0f, 228.0f},
	{239.0f, 151.0f, 102.0f, 275.0f},
	{397.0f, 295.0f, 133.0f, 20.0f},
	{397.0f, 249.0f, 145.0f, 20.0f},
	{292.0f, 441.0f, 25.0f, 16.0f},
	{397.0f, 177.0f, 138.0f, 45.0f},
	{324.0f, 441.0f, 60.0f, 16.0f},
	{543.0f, 200.0f, 27.0f, 15.0f},
};

constexpr Region kDifficultyRegions[] = {
	{239.0f, 264.0f, 63.0f, 22.0f},
	{337.0f, 264.0f, 60.0f, 22.0f},
	{253.0f, 222.0f, 16.0f, 26.0f},
	{270.0f, 222.0f, 16.0f, 26.0f},
};

void copy_text(char* destination, std::size_t capacity, const char* source)
{
	if (capacity != 0)
	{
		std::snprintf(
			destination, capacity, "%s", source == nullptr ? "" : source);
	}
}
}

void campaign_frontend_init(
	CampaignFrontend& frontend,
	const campaign::CampaignStore& store,
	const char* default_callsign,
	std::uint64_t now)
{
	campaign_frontend_enter_single_player(
		frontend, store, default_callsign, now);
}

void campaign_frontend_enter_single_player(
	CampaignFrontend& frontend,
	const campaign::CampaignStore& store,
	const char* default_callsign,
	std::uint64_t now)
{
	frontend = {};
	frontend.screen = CampaignScreen::single_player;
	frontend.entered_at = now;
	frontend.callsign_editing = true;
	if (store.campaign_count != 0)
	{
		frontend.selected_campaign =
			static_cast<std::int32_t>(store.campaign_count - 1);
		copy_text(
			frontend.callsign,
			sizeof(frontend.callsign),
			store.campaigns[store.campaign_count - 1].callsign);
	}
	else
	{
		copy_text(
			frontend.callsign,
			sizeof(frontend.callsign),
			default_callsign);
	}
}

void campaign_frontend_enter_save_load(
	CampaignFrontend& frontend,
	const campaign::CampaignStore& store,
	const campaign::CampaignState& active,
	SaveLoadMode mode,
	bool in_game,
	std::uint64_t now)
{
	frontend.screen = CampaignScreen::save_load;
	frontend.modal = CampaignModal::none;
	frontend.hovered = -1;
	frontend.save_origin = 0;
	frontend.entered_at = now;
	frontend.save_load_mode = mode;
	frontend.save_load_in_game = in_game;
	frontend.save_name_editing = false;
	frontend.save_name[0] = '\0';
	campaign::campaign_list_saves(store, active.id, frontend.saves);
	frontend.selected_save =
		mode == SaveLoadMode::load && frontend.saves.count != 0 ? 0 : -1;
}

void campaign_frontend_set_pointer(
	CampaignFrontend& frontend,
	float x,
	float y,
	bool inside)
{
	frontend.pointer_x = x;
	frontend.pointer_y = y;
	frontend.hovered = -1;
	if (!inside)
	{
		return;
	}
	if (frontend.modal == CampaignModal::quit)
	{
		if (gui::hit_open(
			{kQuitDialogRegions[0].x,
				kQuitDialogRegions[0].y,
				kQuitDialogRegions[0].width,
				kQuitDialogRegions[0].height},
			x,
			y))
		{
			frontend.hovered = 0;
		}
		else if (gui::hit_open(
			{kQuitDialogRegions[1].x,
				kQuitDialogRegions[1].y,
				kQuitDialogRegions[1].width,
				kQuitDialogRegions[1].height},
			x,
			y))
		{
			frontend.hovered = 1;
		}
		return;
	}
	if (frontend.screen == CampaignScreen::single_player)
	{
		if (frontend.modal == CampaignModal::difficulty)
		{
			for (std::uint32_t index = 0; index < 4; ++index)
			{
				if (gui::hit_open(kDifficultyRegions[index], x, y))
				{
					frontend.hovered = static_cast<std::int32_t>(index);
					return;
				}
			}
			return;
		}
		if (frontend.campaign_list_open)
		{
			for (std::uint32_t index = 0; index < campaign::kMaxCampaigns; ++index)
			{
				if (gui::hit_open(
					{400.0f, 223.0f + index * 25.0f, 136.0f, 20.0f},
					x,
					y))
				{
					frontend.hovered = static_cast<std::int32_t>(10 + index);
					return;
				}
			}
		}
		for (std::uint32_t index = 0;
			index < sizeof(kSingleRegions) / sizeof(kSingleRegions[0]);
			++index)
		{
			if (gui::hit_open(kSingleRegions[index], x, y))
			{
				frontend.hovered = static_cast<std::int32_t>(index);
				return;
			}
		}
	}
	else if (frontend.screen == CampaignScreen::save_load)
	{
		campaign_save_browser_set_pointer(frontend, x, y);
		if (frontend.save_load_mode == SaveLoadMode::save
			&& frontend.save_name_editing
			&& gui::hit_open({562.0f, 384.0f, 32.0f, 20.0f}, x, y))
		{
			frontend.hovered = 16;
		}
	}
}

void campaign_frontend_text(
	CampaignFrontend& frontend,
	const char* value)
{
	if (frontend.modal != CampaignModal::none || value == nullptr)
	{
		return;
	}
	char* destination = nullptr;
	std::size_t capacity = 0;
	if (frontend.screen == CampaignScreen::single_player
		&& frontend.callsign_editing)
	{
		destination = frontend.callsign;
		capacity = sizeof(frontend.callsign);
	}
	else if (frontend.screen == CampaignScreen::save_load
		&& frontend.save_name_editing)
	{
		destination = frontend.save_name;
		capacity = sizeof(frontend.save_name);
	}
	else
	{
		return;
	}
	std::size_t length = std::strlen(destination);
	for (const unsigned char* it =
			reinterpret_cast<const unsigned char*>(value);
		*it != 0 && length + 1 < capacity;
		++it)
	{
		if (*it >= 0x20 && *it < 0x7f)
		{
			destination[length++] = static_cast<char>(*it);
		}
	}
	destination[length] = '\0';
}

void campaign_frontend_backspace(CampaignFrontend& frontend)
{
	char* destination = nullptr;
	if (frontend.screen == CampaignScreen::single_player
		&& frontend.callsign_editing)
	{
		destination = frontend.callsign;
	}
	else if (frontend.screen == CampaignScreen::save_load
		&& frontend.save_name_editing)
	{
		destination = frontend.save_name;
	}
	if (destination == nullptr)
	{
		return;
	}
	const std::size_t length = std::strlen(destination);
	if (length != 0)
	{
		destination[length - 1] = '\0';
	}
}

CampaignSelection campaign_frontend_back(CampaignFrontend& frontend)
{
	if (frontend.modal != CampaignModal::none)
	{
		frontend.modal = CampaignModal::none;
		frontend.hovered = -1;
		return CampaignSelection::none;
	}
	if (frontend.campaign_list_open)
	{
		frontend.campaign_list_open = false;
		frontend.hovered = -1;
		return CampaignSelection::none;
	}
	if (frontend.screen == CampaignScreen::save_load)
	{
		return campaign_save_browser_back(frontend);
	}
	if (frontend.screen == CampaignScreen::campaign_ready)
	{
		return CampaignSelection::main_menu;
	}
	return CampaignSelection::main_menu;
}

CampaignSelection campaign_frontend_select(
	CampaignFrontend& frontend,
	campaign::CampaignStore& store,
	campaign::CampaignState& active)
{
	if (frontend.modal == CampaignModal::quit)
	{
		if (frontend.hovered == 0) return CampaignSelection::quit;
		if (frontend.hovered == 1)
		{
			frontend.modal = CampaignModal::none;
			frontend.hovered = -1;
		}
		return CampaignSelection::none;
	}
	if (frontend.screen == CampaignScreen::single_player)
	{
		if (frontend.modal == CampaignModal::difficulty)
		{
			if (frontend.hovered == 0)
			{
				campaign::campaign_defaults(
					active,
					frontend.callsign,
					frontend.difficulty,
					frontend.pilot);
				campaign::campaign_create(store, active);
				frontend.modal = CampaignModal::none;
				frontend.screen = CampaignScreen::campaign_ready;
				frontend.hovered = -1;
				return CampaignSelection::new_campaign;
			}
			else if (frontend.hovered == 1)
			{
				frontend.modal = CampaignModal::none;
			}
			else if (frontend.hovered == 2)
			{
				const std::uint32_t value =
					(static_cast<std::uint32_t>(frontend.difficulty) + 2) % 3;
				frontend.difficulty = static_cast<campaign::Difficulty>(value);
			}
			else if (frontend.hovered == 3)
			{
				const std::uint32_t value =
					(static_cast<std::uint32_t>(frontend.difficulty) + 1) % 3;
				frontend.difficulty = static_cast<campaign::Difficulty>(value);
			}
			return CampaignSelection::none;
		}
		if (frontend.campaign_list_open && frontend.hovered >= 10)
		{
			const std::uint32_t index =
				static_cast<std::uint32_t>(frontend.hovered - 10);
			if (index < store.campaign_count)
			{
				frontend.selected_campaign = static_cast<std::int32_t>(index);
				copy_text(
					frontend.callsign,
					sizeof(frontend.callsign),
					store.campaigns[index].callsign);
			}
			frontend.campaign_list_open = false;
			frontend.hovered = -1;
			return CampaignSelection::none;
		}
		switch (frontend.hovered)
		{
		case 0:
			frontend.pilot = campaign::Pilot::female;
			break;
		case 1:
			frontend.pilot = campaign::Pilot::male;
			break;
		case 2:
			if (frontend.selected_campaign >= 0
				&& static_cast<std::uint32_t>(frontend.selected_campaign)
					< store.campaign_count)
			{
				const campaign::CampaignSummary& summary =
					store.campaigns[frontend.selected_campaign];
				if (campaign::campaign_profile_load(store, summary.id, active))
				{
					campaign::campaign_list_saves(store, active.id, frontend.saves);
					frontend.selected_save =
						frontend.saves.count == 0 ? -1 : 0;
					frontend.save_origin = 0;
					frontend.screen = CampaignScreen::save_load;
					frontend.hovered = -1;
					return CampaignSelection::save_load;
				}
			}
			break;
		case 3:
			if (frontend.callsign[0] != '\0')
			{
				frontend.callsign_editing = false;
				frontend.modal = CampaignModal::difficulty;
				frontend.difficulty = campaign::Difficulty::medium;
			}
			break;
		case 4:
			return CampaignSelection::main_menu;
		case 5:
			frontend.callsign_editing = true;
			frontend.campaign_list_open = false;
			break;
		case 6:
			frontend.modal = CampaignModal::quit;
			frontend.hovered = -1;
			break;
		case 7:
			frontend.campaign_list_open =
				store.campaign_count != 0 && !frontend.campaign_list_open;
			frontend.callsign_editing = !frontend.campaign_list_open;
			break;
		default:
			break;
		}
		return CampaignSelection::none;
	}

	if (frontend.screen == CampaignScreen::save_load)
	{
		return campaign_save_browser_select(frontend, store, active);
	}
	return CampaignSelection::none;
}

}

#include "frontend/campaign_save_browser.hpp"

#include "frontend/gui.hpp"

#include <cstdio>

namespace sl_open::frontend
{
namespace
{
constexpr gui::Rect kBack{292.0f, 421.0f, 25.0f, 16.0f};
constexpr gui::Rect kLoad{324.0f, 421.0f, 25.0f, 16.0f};
constexpr gui::Rect kMain{292.0f, 441.0f, 25.0f, 16.0f};
constexpr gui::Rect kQuit{324.0f, 441.0f, 25.0f, 16.0f};
constexpr gui::Rect kUp{579.0f, 250.0f, 26.0f, 16.0f};
constexpr gui::Rect kDown{579.0f, 268.0f, 26.0f, 16.0f};

CampaignSelection load_selected(
	CampaignFrontend& frontend,
	campaign::CampaignStore& store,
	campaign::CampaignState& active)
{
	if (frontend.selected_save < 0
		|| static_cast<std::uint32_t>(frontend.selected_save)
			>= frontend.saves.count)
	{
		return CampaignSelection::none;
	}
	const campaign::SaveSlot& slot =
		frontend.saves.slots[frontend.selected_save];
	char name[campaign::kSaveNameBytes];
	if (!campaign::campaign_load_slot(
		store,
		active.id,
		slot.slot,
		active,
		name,
		sizeof(name)))
	{
		return CampaignSelection::none;
	}
	frontend.screen = CampaignScreen::campaign_ready;
	frontend.hovered = -1;
	return CampaignSelection::campaign_ready;
}

void begin_save_name_edit(
	CampaignFrontend& frontend,
	const campaign::SaveSlot& slot)
{
	std::snprintf(
		frontend.save_name,
		sizeof(frontend.save_name),
		"%s",
		slot.name);
	frontend.save_name_editing = true;
}

CampaignSelection commit_save(
	CampaignFrontend& frontend,
	campaign::CampaignStore& store,
	const campaign::CampaignState& active)
{
	if (frontend.selected_save < 0
		|| static_cast<std::uint32_t>(frontend.selected_save)
			>= frontend.saves.count
		|| frontend.save_name[0] == '\0')
	{
		return CampaignSelection::none;
	}
	const campaign::SaveSlot& selected =
		frontend.saves.slots[frontend.selected_save];
	if (!campaign::campaign_save_slot(
			store,
			active,
			selected.slot,
			frontend.save_name))
	{
		return CampaignSelection::none;
	}
	campaign::campaign_list_saves(store, active.id, frontend.saves);
	frontend.save_name_editing = false;
	return CampaignSelection::save_complete;
}
}

void campaign_save_browser_set_pointer(
	CampaignFrontend& frontend,
	float x,
	float y)
{
	for (std::uint32_t row = 0; row < 10; ++row)
	{
		if (gui::hit_open(
			{400.0f, 126.0f + row * 17.0f, 49.0f, 17.0f}, x, y))
		{
			frontend.hovered = static_cast<std::int32_t>(row);
			return;
		}
	}
	if (gui::hit_open(kBack, x, y)) frontend.hovered = 10;
	else if (gui::hit_open(kLoad, x, y)) frontend.hovered = 11;
	else if (gui::hit_open(kMain, x, y)) frontend.hovered = 12;
	else if (gui::hit_open(kQuit, x, y)) frontend.hovered = 13;
	else if (gui::hit_open(kUp, x, y)) frontend.hovered = 14;
	else if (gui::hit_open(kDown, x, y)) frontend.hovered = 15;
}

CampaignSelection campaign_save_browser_back(CampaignFrontend& frontend)
{
	frontend.screen = CampaignScreen::single_player;
	frontend.hovered = -1;
	return CampaignSelection::single_player;
}

CampaignSelection campaign_save_browser_select(
	CampaignFrontend& frontend,
	campaign::CampaignStore& store,
	campaign::CampaignState& active)
{
	if (frontend.hovered >= 0 && frontend.hovered < 10)
	{
		const std::uint32_t index =
			frontend.save_origin + static_cast<std::uint32_t>(frontend.hovered);
		if (index < frontend.saves.count)
		{
			if (frontend.save_load_mode == SaveLoadMode::load
				&& frontend.selected_save == static_cast<std::int32_t>(index))
			{
				const CampaignSelection selection =
					load_selected(frontend, store, active);
				if (selection != CampaignSelection::none)
				{
					return selection;
				}
			}
			frontend.selected_save = static_cast<std::int32_t>(index);
			if (frontend.save_load_mode == SaveLoadMode::save)
			{
				begin_save_name_edit(
					frontend, frontend.saves.slots[index]);
			}
		}
	}
	else if (frontend.hovered == 10)
	{
		return campaign_save_browser_back(frontend);
	}
	else if (frontend.hovered == 11)
	{
		if (frontend.save_load_mode == SaveLoadMode::load)
		{
			return load_selected(frontend, store, active);
		}
		if (frontend.selected_save < 0)
		{
			const std::uint16_t slot =
				static_cast<std::uint16_t>(frontend.saves.count);
			if (!campaign::campaign_save_slot(
					store, active, slot, "Empty Save Game"))
			{
				return CampaignSelection::none;
			}
			campaign::campaign_list_saves(store, active.id, frontend.saves);
			if (frontend.saves.count == 0)
			{
				return CampaignSelection::none;
			}
			frontend.selected_save =
				static_cast<std::int32_t>(frontend.saves.count - 1);
			if (frontend.saves.count > 10)
			{
				frontend.save_origin = frontend.saves.count - 10;
			}
			begin_save_name_edit(
				frontend,
				frontend.saves.slots[frontend.selected_save]);
			return CampaignSelection::none;
		}
		return commit_save(frontend, store, active);
	}
	else if (frontend.hovered == 12)
	{
		return CampaignSelection::main_menu;
	}
	else if (frontend.hovered == 13)
	{
		frontend.modal = CampaignModal::quit;
		frontend.hovered = -1;
	}
	else if (frontend.hovered == 14 && frontend.save_origin > 0)
	{
		--frontend.save_origin;
	}
	else if (frontend.hovered == 15
		&& frontend.save_origin + 10 < frontend.saves.count)
	{
		++frontend.save_origin;
	}
	else if (frontend.hovered == 16
		&& frontend.save_load_mode == SaveLoadMode::save)
	{
		return commit_save(frontend, store, active);
	}
	return CampaignSelection::none;
}
}

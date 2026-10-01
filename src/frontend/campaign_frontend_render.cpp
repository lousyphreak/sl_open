#include "frontend/campaign_frontend.hpp"

#include "frontend/gui_render.hpp"
#include "frontend/quit_dialog.hpp"
#include "localization/language.hpp"
#include "render/frontend_renderer.hpp"

#include <algorithm>
#include <cstdio>

namespace sl_open::frontend
{
namespace
{
void panel(
	render::FrontendCommands& commands,
	const render::FrontendRenderer& renderer,
	float x,
	float y,
	float width,
	float height,
	bool active = false)
{
	render::frontend_rgba_quad(
		commands,
		renderer.white,
		x,
		y,
		width,
		height,
		active ? 0x1f7897dd : 0x07141ed8);
	render::frontend_rgba_quad(
		commands,
		renderer.white,
		x + 2.0f,
		y + 2.0f,
		std::max(0.0f, width - 4.0f),
		std::max(0.0f, height - 4.0f),
		active ? 0x154b65ff : 0x102b3aff);
}

void text(
	render::FrontendCommands& commands,
	const render::FrontendRenderer& renderer,
	const char* value,
	float x,
	float y,
	bool active = false,
	float scale = 0.72f)
{
	render::frontend_text(
		commands,
		renderer,
		value,
		x,
		y,
		active ? renderer.shell.font_blue_palette : renderer.shell.font_gold_palette,
		0xffffffff,
		scale);
}

void right_text(
	render::FrontendCommands& commands,
	const render::FrontendRenderer& renderer,
	const char* value,
	float right,
	float y,
	bool active,
	float scale)
{
	text(
		commands,
		renderer,
		value,
		gui::aligned_x(
			right,
			gui::text_width(
				renderer.shell.glyphs,
				renderer.shell.glyph_count,
				value,
				scale),
			gui::TextAlign::right),
		y,
		active,
		scale);
}

void centered_text(
	render::FrontendCommands& commands,
	const render::FrontendRenderer& renderer,
	const char* value,
	float center,
	float y,
	bool active,
	float scale)
{
	text(
		commands,
		renderer,
		value,
		gui::aligned_x(
			center,
			gui::text_width(
				renderer.shell.glyphs,
				renderer.shell.glyph_count,
				value,
				scale),
			gui::TextAlign::center),
		y,
		active,
		scale);
}

void cursor(
	const CampaignFrontend& frontend,
	const render::FrontendRenderer& renderer,
	render::FrontendCommands& commands,
	std::uint64_t now)
{
	const std::uint32_t frame =
		static_cast<std::uint32_t>(((now - frontend.entered_at) / 40) % 16);
	render::frontend_indexed_quad(
		commands,
		renderer.campaign.cursor[frame],
		renderer.campaign.cursor_palette,
		frontend.pointer_x,
		frontend.pointer_y);
}

void begin_frame(
	const render::FrontendTexture& background,
	render::FrontendCommands& commands)
{
	gui::begin_screen(commands);
	render::frontend_rgba_quad(
		commands,
		background,
		0.0f,
		0.0f,
		render::kFrontendWidth,
		render::kFrontendHeight,
		0xffffffff);
}

void control(
	render::FrontendCommands& commands,
	const render::FrontendRenderer& renderer,
	std::uint32_t shape,
	float x,
	float y)
{
	render::frontend_indexed_quad(
		commands,
		renderer.campaign.controls[shape - 22],
		renderer.campaign.control_palette,
		x,
		y);
}

void build_quit_confirmation(
	const CampaignFrontend& frontend,
	const LanguageTable& language,
	const render::FrontendRenderer& renderer,
	render::FrontendCommands& commands)
{
	quit_dialog_build(language, renderer, commands, frontend.hovered);
}

void build_single(
	const CampaignFrontend& frontend,
	const campaign::CampaignStore& store,
	const LanguageTable& language,
	const render::FrontendRenderer& renderer,
	render::FrontendCommands& commands,
	std::uint64_t now)
{
	const std::uint32_t portrait =
		frontend.pilot == campaign::Pilot::female ? 0 : 1;
	const float portrait_x[] = {34.0f, 200.0f};
	render::frontend_indexed_quad(
		commands,
		renderer.campaign.pilots[portrait],
		renderer.campaign.pilot_palette,
		portrait_x[portrait],
		114.0f);

	text(commands, renderer, language_text(language, 0xb7), 217.0f, 105.0f);
	text(commands, renderer, language_text(language, 0xb8), 396.0f, 168.0f);
	text(commands, renderer, language_text(language, 0xb9), 433.0f, 247.0f);
	text(commands, renderer, language_text(language, 0xba), 433.0f, 298.0f);
	right_text(
		commands,
		renderer,
		language_text(language, 0xbb),
		288.0f,
		440.0f,
		frontend.hovered == 4,
		0.62f);
	text(commands, renderer, language_text(language, 0xbc), 353.0f, 440.0f,
		frontend.hovered == 6, 0.62f);

	char callsign[campaign::kCallsignBytes + 2];
	std::snprintf(
		callsign,
		sizeof(callsign),
		frontend.callsign_editing
			&& ((now - frontend.entered_at) / 250) % 2 == 0
			? "%s%s"
			: "%s",
		frontend.callsign,
		frontend.callsign_editing ? "_" : "");
	text(commands, renderer, callsign, 402.0f, 197.0f,
		frontend.callsign_editing, 0.62f);

	control(commands, renderer, frontend.hovered == 7 ? 25 : 24, 543.0f, 200.0f);
	control(commands, renderer, frontend.hovered == 3 ? 23 : 22, 398.0f, 249.0f);
	control(commands, renderer, frontend.hovered == 2 ? 23 : 22, 398.0f, 300.0f);
	control(commands, renderer, frontend.hovered == 4 ? 27 : 26, 292.0f, 441.0f);
	control(commands, renderer, frontend.hovered == 6 ? 27 : 26, 324.0f, 441.0f);

	if (frontend.campaign_list_open)
	{
		for (std::uint32_t index = 0;
			index < store.campaign_count;
			++index)
		{
			const float y = 223.0f + index * 25.0f;
			const bool active =
				frontend.hovered == 10 + static_cast<std::int32_t>(index);
			panel(commands, renderer, 400.0f, y, 136.0f, 20.0f, active);
			text(
				commands,
				renderer,
				store.campaigns[index].callsign,
				404.0f,
				y + 2.0f,
				active,
				0.58f);
		}
	}

	if (frontend.modal == CampaignModal::difficulty)
	{
		render::frontend_indexed_quad(
			commands,
			renderer.campaign.difficulty_panels[0],
			renderer.campaign.difficulty_palette,
			114.0f,
			177.0f);
		control(commands, renderer, 30, 253.0f, 222.0f);
		control(commands, renderer, frontend.hovered == 0 ? 27 : 26, 276.0f, 269.0f);
		control(commands, renderer, frontend.hovered == 1 ? 27 : 26, 338.0f, 269.0f);
		if (frontend.hovered == 2)
		{
			control(commands, renderer, 31, 253.0f, 222.0f);
		}
		else if (frontend.hovered == 3)
		{
			control(commands, renderer, 32, 270.0f, 222.0f);
		}
		const std::uint32_t difficulty_labels[] = {0x2a7, 0x11c, 0x2a8};
		centered_text(
			commands,
			renderer,
			language_text(language, 0x2a6),
			320.0f,
			185.0f,
			false,
			1.0f);
		text(
			commands,
			renderer,
			language_text(
				language,
				difficulty_labels[static_cast<std::uint32_t>(frontend.difficulty)]),
			289.0f,
			222.0f);
		text(commands, renderer, language_text(language, 0xf7), 370.0f, 264.0f,
			frontend.hovered == 1, 0.62f);
		right_text(
			commands,
			renderer,
			language_text(language, 0x14a),
			268.0f,
			264.0f,
			frontend.hovered == 0,
			0.62f);
	}
	else if (frontend.modal == CampaignModal::quit)
	{
		build_quit_confirmation(
			frontend, language, renderer, commands);
	}
}

void build_saves(
	const CampaignFrontend& frontend,
	const campaign::CampaignState& campaign,
	const LanguageTable& language,
	const render::FrontendRenderer& renderer,
	render::FrontendCommands& commands,
	std::uint64_t now)
{
	char title[128];
	std::snprintf(
		title,
		sizeof(title),
		"%s %s",
		language_text(
			language,
			frontend.save_load_mode == SaveLoadMode::save
				? 0x560
				: 0x55f),
		campaign.callsign);
	centered_text(commands, renderer, title, 320.0f, 86.0f, false, 1.0f);
	text(commands, renderer, language_text(language, 0xe3), 45.0f, 107.0f);
	centered_text(
		commands,
		renderer,
		language_text(language, 0xe4),
		320.0f,
		107.0f,
		false,
		1.0f);
	right_text(
		commands,
		renderer,
		language_text(language, 0xe5),
		575.0f,
		107.0f,
		false,
		1.0f);
	for (std::uint32_t row = 0; row < 10; ++row)
	{
		const std::uint32_t index = frontend.save_origin + row;
		if (index >= frontend.saves.count)
		{
			break;
		}
		const campaign::SaveSlot& save = frontend.saves.slots[index];
		const float y = 126.0f + row * 17.0f;
		const bool selected =
			frontend.selected_save == static_cast<std::int32_t>(index);
		const bool hovered =
			frontend.hovered == static_cast<std::int32_t>(row);
		if (selected || hovered)
		{
			render::frontend_rgba_quad(
				commands,
				renderer.white,
				49.0f,
				y,
				524.0f,
				18.0f,
				selected ? 0x154b65cc : 0x1f789799);
		}
		text(commands, renderer, save.name, 49.0f, y, hovered, 0.52f);
		centered_text(
			commands, renderer, save.callsign, 320.0f, y, hovered, 0.52f);
		char mission[16];
		std::snprintf(mission, sizeof(mission), "%u", save.mission);
		right_text(commands, renderer, mission, 570.0f, y, hovered, 0.52f);
	}

	text(commands, renderer, language_text(language, 0xe6), 49.0f, 302.0f);
	text(commands, renderer, language_text(language, 0xe7), 49.0f, 322.0f,
		false, 0.62f);
	text(commands, renderer, language_text(language, 0xe8), 49.0f, 339.0f,
		false, 0.62f);
	text(commands, renderer, language_text(language, 0xe9), 290.0f, 322.0f,
		false, 0.62f);
	text(commands, renderer, language_text(language, 0xeb), 290.0f, 339.0f,
		false, 0.62f);
	if (frontend.selected_save >= 0
		&& static_cast<std::uint32_t>(frontend.selected_save)
			< frontend.saves.count)
	{
		const campaign::SaveSlot& selected =
			frontend.saves.slots[frontend.selected_save];
		text(commands, renderer, selected.callsign, 113.0f, 322.0f,
			false, 0.62f);
		const std::uint32_t rank_ids[] = {
			0x55c, 0xed, 0xee, 0x1bd, 0x1be, 0x1bf, 0x1c0, 0xf0, 0x1c1};
		const std::uint32_t rank =
			std::min<std::uint32_t>(campaign.rank, 8);
		text(
			commands,
			renderer,
			language_text(language, rank_ids[rank]),
			113.0f,
			339.0f,
			false,
			0.62f);
		const std::uint32_t level_ids[] = {0xfa, 0xf9, 0xf8, 0xfb};
		const std::uint32_t level =
			std::min<std::uint32_t>(campaign.progression, 3);
		text(
			commands,
			renderer,
			language_text(language, level_ids[level]),
			350.0f,
			322.0f,
			false,
			0.62f);
	}
	if (frontend.save_load_mode == SaveLoadMode::save
		&& frontend.save_name_editing)
	{
		text(
			commands,
			renderer,
			frontend.save_name,
			400.0f,
			384.0f,
			frontend.hovered == 16,
			0.62f);
		if (((now / 250) & 1) == 0)
		{
			const float width = gui::text_width(
				renderer.shell.glyphs,
				renderer.shell.glyph_count,
				frontend.save_name,
				0.62f);
			text(
				commands,
				renderer,
				"_",
				400.0f + width,
				384.0f,
				true,
				0.62f);
		}
	}
	control(commands, renderer, frontend.hovered == 10 ? 27 : 26, 292.0f, 421.0f);
	control(commands, renderer, frontend.hovered == 11 ? 27 : 26, 324.0f, 421.0f);
	control(commands, renderer, frontend.hovered == 12 ? 27 : 26, 292.0f, 441.0f);
	control(commands, renderer, frontend.hovered == 13 ? 27 : 26, 324.0f, 441.0f);
	control(commands, renderer, 28, 579.0f, 250.0f);
	if (frontend.hovered == 14)
	{
		control(commands, renderer, 29, 579.0f, 250.0f);
	}
	right_text(
		commands,
		renderer,
		language_text(language, 0xf7),
		268.0f,
		420.0f,
		frontend.hovered == 10,
		0.62f);
	const std::uint32_t action_label =
		frontend.save_load_mode == SaveLoadMode::load
			? 0xba
			: (frontend.save_name_editing ? 0x178 : 0x177);
	text(commands, renderer, language_text(language, action_label), 350.0f, 420.0f,
		frontend.hovered == 11, 0.62f);
	right_text(
		commands,
		renderer,
		language_text(language, 0xbb),
		268.0f,
		440.0f,
		frontend.hovered == 12,
		0.62f);
	text(commands, renderer, language_text(language, 0xbc), 350.0f, 440.0f,
		frontend.hovered == 13, 0.62f);
}

void build_ready(
	const campaign::CampaignState& campaign,
	const render::FrontendRenderer& renderer,
	render::FrontendCommands& commands)
{
	text(commands, renderer, "CAMPAIGN READY", 55.0f, 62.0f, false, 1.0f);
	panel(commands, renderer, 110.0f, 135.0f, 420.0f, 220.0f);
	char line[128];
	std::snprintf(line, sizeof(line), "PILOT: %s", campaign.callsign);
	text(commands, renderer, line, 155.0f, 175.0f);
	std::snprintf(line, sizeof(line), "MISSION: %u", campaign.mission);
	text(commands, renderer, line, 155.0f, 210.0f);
	std::snprintf(line, sizeof(line), "SCORE: %d", campaign.score);
	text(commands, renderer, line, 155.0f, 245.0f);
	text(commands, renderer, "VR HUB INITIALIZING", 155.0f, 300.0f, true);
}
}

void campaign_frontend_build(
	const CampaignFrontend& frontend,
	const campaign::CampaignStore& store,
	const campaign::CampaignState& campaign,
	const LanguageTable& language,
	const render::FrontendRenderer& renderer,
	render::FrontendCommands& commands,
	std::uint64_t now)
{
	switch (frontend.screen)
	{
	case CampaignScreen::single_player:
		begin_frame(renderer.campaign.background, commands);
		build_single(frontend, store, language, renderer, commands, now);
		break;
	case CampaignScreen::save_load:
		begin_frame(renderer.campaign.save_load_background, commands);
		build_saves(
			frontend, campaign, language, renderer, commands, now);
		break;
	case CampaignScreen::campaign_ready:
		begin_frame(renderer.campaign.save_load_background, commands);
		build_ready(campaign, renderer, commands);
		break;
	}
	if (frontend.modal == CampaignModal::quit
		&& frontend.screen != CampaignScreen::single_player)
	{
		build_quit_confirmation(
			frontend, language, renderer, commands);
	}
	cursor(frontend, renderer, commands, now);
}
}

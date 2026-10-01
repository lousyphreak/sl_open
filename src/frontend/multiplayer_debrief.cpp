#include "frontend/multiplayer_debrief.hpp"

#include "frontend/gui_render.hpp"
#include "localization/language.hpp"
#include "render/frontend_renderer.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <limits>

namespace sl_open::frontend
{
namespace
{
#include "frontend/itac_archive_data.inc"

// FUN_004296a0 builds these six hit records in this order and
// FUN_0043eb30 performs strict-edge comparisons.
constexpr gui::Rect kRegions[] = {
	{293.0f, 199.0f, 27.0f, 28.0f},
	{320.0f, 199.0f, 27.0f, 28.0f},
	{528.0f, 362.0f, 37.0f, 27.0f},
	{571.0f, 422.0f, 58.0f, 54.0f},
	{528.0f, 398.0f, 37.0f, 27.0f},
	{528.0f, 438.0f, 37.0f, 27.0f},
};
// FUN_004409f0 uses a second, one-pixel-inset pair for scroll clicks.
constexpr gui::Rect kScrollClickRegions[] = {
	{294.0f, 200.0f, 27.0f, 27.0f},
	{321.0f, 200.0f, 27.0f, 27.0f},
};

constexpr std::uint32_t kTitleTint = 0xffbc40ff;
constexpr std::uint32_t kLabelTint = 0xffbd82ff;
constexpr std::uint32_t kValueTint = 0xff923aff;
constexpr std::uint32_t kReportHeaderTint = 0xffd13aff;
constexpr float kReportWidth = 563.0f;
constexpr std::uint16_t kVisibleReportLines = 6;
constexpr float kReportLineHeight = 15.0f;
constexpr std::size_t kCallsignCapacity = 32;
constexpr std::uint16_t kReportMissions[] = {
	1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11,
	14, 15, 16, 18, 19, 20, 21, 23, 24, 25, 26, 27,
};
constexpr std::uint16_t kRetryWarningText[] = {
	0, 227, 228, 229,
};
constexpr std::uint16_t kPromotionText[] = {
	230, 230, 231, 232, 233, 234, 235, 236, 237,
};

const char* safe_text(const char* text)
{
	return text == nullptr ? "" : text;
}

float text_width(
	const render::FrontendRenderer& renderer,
	const char* text)
{
	return gui::text_width(
		renderer.shell.glyphs,
		renderer.shell.glyph_count,
		safe_text(text));
}

void draw_text(
	render::FrontendCommands& commands,
	const render::FrontendRenderer& renderer,
	const char* text,
	float x,
	float y,
	std::uint32_t tint)
{
	render::frontend_text(
		commands,
		renderer,
		safe_text(text),
		x,
		y,
		renderer.shell.font_white_palette,
		tint);
}

void draw_right_text(
	render::FrontendCommands& commands,
	const render::FrontendRenderer& renderer,
	const char* text,
	float x,
	float y,
	std::uint32_t tint)
{
	draw_text(
		commands,
		renderer,
		text,
		x - text_width(renderer, text),
		y,
		tint);
}

std::uint16_t report_line_count(
	const MultiplayerDebriefView& view,
	const render::FrontendRenderer& renderer)
{
	gui::TextWrapCursor cursor{safe_text(view.report_body)};
	char line[256];
	std::uint32_t count = 0;
	while (gui::next_wrapped_line(
		cursor,
		renderer.shell.glyphs,
		renderer.shell.glyph_count,
		1.0f,
		kReportWidth,
		line))
	{
		if (count != std::numeric_limits<std::uint16_t>::max())
		{
			++count;
		}
	}
	return static_cast<std::uint16_t>(count);
}

void draw_report(
	MultiplayerDebrief& debrief,
	const MultiplayerDebriefView& view,
	const LanguageTable& itac_language,
	const render::FrontendRenderer& renderer,
	render::FrontendCommands& commands)
{
	draw_text(
		commands,
		renderer,
		language_text(itac_language, 0x8c),
		35.0f,
		65.0f,
		kReportHeaderTint);
	draw_text(
		commands,
		renderer,
		view.report_title,
		80.0f,
		65.0f,
		kReportHeaderTint);
	draw_text(
		commands,
		renderer,
		language_text(itac_language, 0x8d),
		35.0f,
		80.0f,
		kReportHeaderTint);
	draw_text(
		commands,
		renderer,
		view.report_recipient,
		80.0f,
		80.0f,
		kReportHeaderTint);

	const std::uint16_t line_count = report_line_count(view, renderer);
	debrief.report_scroll_max = line_count > kVisibleReportLines
		? static_cast<std::int16_t>(std::min<std::uint32_t>(
			line_count - kVisibleReportLines,
			std::numeric_limits<std::int16_t>::max()))
		: 0;
	debrief.report_scroll = std::clamp<std::int16_t>(
		debrief.report_scroll, 0, debrief.report_scroll_max);

	render::frontend_scissor(commands, 35, 107, 563, 90);
	gui::TextWrapCursor cursor{safe_text(view.report_body)};
	char line[256];
	std::uint16_t logical_line = 0;
	std::uint16_t drawn = 0;
	while (drawn < kVisibleReportLines
		&& gui::next_wrapped_line(
			cursor,
			renderer.shell.glyphs,
			renderer.shell.glyph_count,
			1.0f,
			kReportWidth,
			line))
	{
		if (logical_line >= debrief.report_scroll)
		{
			draw_text(
				commands,
				renderer,
				line,
				35.0f,
				107.0f + drawn * kReportLineHeight,
				kValueTint);
			++drawn;
		}
		++logical_line;
	}
	render::frontend_scissor(commands, 0, 0, 640, 480);
}

void draw_chat(
	const MultiplayerDebriefView& view,
	const render::FrontendRenderer& renderer,
	render::FrontendCommands& commands,
	std::uint64_t now)
{
	const std::uint8_t line_count = std::min<std::uint8_t>(
		view.chat_line_count,
		kMultiplayerDebriefChatLineCapacity);
	for (std::uint8_t index = 0; index < line_count; ++index)
	{
		if (view.chat_lines[index] == nullptr
			|| *view.chat_lines[index] == '\0')
		{
			break;
		}
		draw_text(
			commands,
			renderer,
			view.chat_lines[index],
			38.0f,
			282.0f - index * 12.0f,
			kValueTint);
	}

	// FUN_004296a0 deliberately measures the authored English literal even
	// though FUN_00429dc0 draws the localized label.
	const float entry_x =
		42.0f + text_width(renderer, "MESSAGE:");
	draw_text(
		commands,
		renderer,
		view.chat_entry,
		entry_x,
		339.0f,
		kValueTint);
	if (((now / 250) & 1u) != 0)
	{
		draw_text(
			commands,
			renderer,
			"_",
			entry_x + text_width(renderer, view.chat_entry),
			339.0f,
			kValueTint);
	}
}

bool all_ready_to_continue(const MultiplayerDebriefView& view)
{
	const std::uint8_t count = std::min<std::uint8_t>(
		view.player_count,
		kMultiplayerDebriefPlayerCapacity);
	for (std::uint8_t index = 0; index < count; ++index)
	{
		const MultiplayerDebriefPlayer& player = view.players[index];
		if (!player.restarting && !player.local && !player.ready)
		{
			return false;
		}
	}
	return true;
}

bool all_ready_to_replay(const MultiplayerDebriefView& view)
{
	const std::uint8_t count = std::min<std::uint8_t>(
		view.player_count,
		kMultiplayerDebriefPlayerCapacity);
	for (std::uint8_t index = 0; index < count; ++index)
	{
		const MultiplayerDebriefPlayer& player = view.players[index];
		if (!player.local && !player.ready)
		{
			return false;
		}
	}
	return true;
}

void copy_truncated_callsign(
	char (&destination)[kCallsignCapacity + 1],
	const char* callsign,
	const char* restart_text,
	const render::FrontendRenderer& renderer)
{
	callsign = safe_text(callsign);
	std::size_t length = 0;
	while (length < kCallsignCapacity && callsign[length] != '\0')
	{
		++length;
	}
	std::memcpy(destination, callsign, length);
	destination[length] = '\0';

	const float available = 145.0f - text_width(renderer, restart_text);
	while (length != 0
		&& text_width(renderer, destination) >= available)
	{
		destination[--length] = '\0';
	}
}

void uppercase_copy(char (&destination)[64], const char* source)
{
	source = safe_text(source);
	std::size_t index = 0;
	for (; index + 1 < std::size(destination) && source[index] != '\0';
		++index)
	{
		destination[index] = static_cast<char>(
			std::toupper(static_cast<unsigned char>(source[index])));
	}
	destination[index] = '\0';
}

MultiplayerDebriefAction simple_action(
	MultiplayerDebriefActionType type)
{
	MultiplayerDebriefAction action;
	action.type = type;
	return action;
}

std::int8_t report_mission_index(std::uint16_t mission)
{
	// The alternate second leg of mission 25 is numbered 252 by the retail
	// executable but deliberately reuses mission 25's report record.
	if (mission == 252)
	{
		return 20;
	}
	for (std::uint8_t index = 0; index < std::size(kReportMissions);
		++index)
	{
		if (kReportMissions[index] == mission)
		{
			return static_cast<std::int8_t>(index);
		}
	}
	return -1;
}

bool copy_bounded(
	char* destination,
	std::size_t capacity,
	const char* source)
{
	if (destination == nullptr || capacity == 0 || source == nullptr)
	{
		return false;
	}
	const std::size_t length = std::strlen(source);
	if (length >= capacity)
	{
		destination[0] = '\0';
		return false;
	}
	std::memcpy(destination, source, length + 1);
	return true;
}

bool append_bounded(
	char* destination,
	std::size_t capacity,
	const char* source)
{
	if (destination == nullptr || capacity == 0 || source == nullptr)
	{
		return false;
	}
	std::size_t destination_length = 0;
	while (destination_length < capacity
		&& destination[destination_length] != '\0')
	{
		++destination_length;
	}
	if (destination_length == capacity)
	{
		return false;
	}
	const std::size_t source_length = std::strlen(source);
	if (source_length >= capacity - destination_length)
	{
		return false;
	}
	std::memcpy(
		destination + destination_length,
		source,
		source_length + 1);
	return true;
}

bool append_language_paragraph(
	char* destination,
	std::size_t capacity,
	const LanguageTable& language,
	std::uint16_t id)
{
	const char* text = language_text(language, id);
	if (text[0] == '\0')
	{
		return false;
	}
	if (destination[0] != '\0'
		&& !append_bounded(destination, capacity, "\n\n"))
	{
		return false;
	}
	return append_bounded(destination, capacity, text);
}

std::uint16_t terminal_outcome_text(
	campaign::MissionCoordinatorResult outcome)
{
	switch (outcome)
	{
	case campaign::MissionCoordinatorResult::destroyed:
		return 0x53f;
	case campaign::MissionCoordinatorResult::interrupted:
		return 0x540;
	case campaign::MissionCoordinatorResult::executed:
		return 0x541;
	case campaign::MissionCoordinatorResult::transfer:
	case campaign::MissionCoordinatorResult::cooperative_transfer:
		return 0x566;
	default:
		return 0;
	}
}
}

void multiplayer_debrief_reset(
	MultiplayerDebrief& debrief,
	std::uint64_t now)
{
	debrief = {};
	debrief.pointer_x = 320.0f;
	debrief.pointer_y = 240.0f;
	debrief.hovered = -1;
	debrief.entered_at = now;
}

void multiplayer_debrief_set_pointer(
	MultiplayerDebrief& debrief,
	float x,
	float y,
	bool inside)
{
	debrief.pointer_x = x;
	debrief.pointer_y = y;
	debrief.hovered = -1;
	if (!inside)
	{
		return;
	}
	for (std::uint8_t index = 0; index < std::size(kRegions); ++index)
	{
		if (gui::hit_open(kRegions[index], x, y))
		{
			debrief.hovered = static_cast<std::int8_t>(index);
			return;
		}
	}
}

MultiplayerDebriefAction multiplayer_debrief_select(
	MultiplayerDebrief& debrief,
	const MultiplayerDebriefView& view)
{
	if (gui::hit_open(
		kScrollClickRegions[0], debrief.pointer_x, debrief.pointer_y))
	{
		if (debrief.report_scroll < debrief.report_scroll_max)
		{
			++debrief.report_scroll;
		}
		return {};
	}
	if (gui::hit_open(
		kScrollClickRegions[1], debrief.pointer_x, debrief.pointer_y))
	{
		if (debrief.report_scroll > 0)
		{
			--debrief.report_scroll;
		}
		return {};
	}
	switch (debrief.hovered)
	{
	case 2:
		if (view.local_is_leader && view.leader_valid)
		{
			return all_ready_to_continue(view)
				? simple_action(
					MultiplayerDebriefActionType::continue_mission)
				: MultiplayerDebriefAction{};
		}
		if (!debrief.ready_sent)
		{
			debrief.ready_sent = true;
			return simple_action(MultiplayerDebriefActionType::ready);
		}
		return {};
	case 3:
		return simple_action(MultiplayerDebriefActionType::leave);
	case 4:
		return view.local_is_leader && all_ready_to_replay(view)
			? simple_action(
				MultiplayerDebriefActionType::replay_mission)
			: MultiplayerDebriefAction{};
	case 5:
		return simple_action(MultiplayerDebriefActionType::save_game);
	default:
		return {};
	}
}

MultiplayerDebriefAction multiplayer_debrief_text(
	const MultiplayerDebriefView& view,
	const char* text)
{
	if (text == nullptr)
	{
		return {};
	}
	const char* current = safe_text(view.chat_entry);
	const std::size_t current_length = std::min(
		std::strlen(current),
		kMultiplayerDebriefChatBytes - 1);
	if (current_length + 1 >= kMultiplayerDebriefChatBytes)
	{
		return {};
	}
	MultiplayerDebriefAction action = simple_action(
		MultiplayerDebriefActionType::edit_chat);
	std::size_t output = 0;
	for (const unsigned char* character =
			reinterpret_cast<const unsigned char*>(text);
		*character != 0
			&& current_length + output + 1
				< kMultiplayerDebriefChatBytes
			&& output + 1 < sizeof(action.text);
		++character)
	{
		if (*character >= 0x20 && *character < 0x7f)
		{
			action.text[output++] = static_cast<char>(*character);
		}
	}
	action.text[output] = '\0';
	return output == 0 ? MultiplayerDebriefAction{} : action;
}

MultiplayerDebriefAction multiplayer_debrief_backspace(
	const MultiplayerDebriefView& view)
{
	return safe_text(view.chat_entry)[0] == '\0'
		? MultiplayerDebriefAction{}
		: simple_action(MultiplayerDebriefActionType::backspace_chat);
}

MultiplayerDebriefAction multiplayer_debrief_submit_chat(
	const MultiplayerDebriefView& view)
{
	MultiplayerDebriefAction action = simple_action(
		MultiplayerDebriefActionType::send_chat);
	std::snprintf(
		action.text,
		sizeof(action.text),
		"%s",
		safe_text(view.chat_entry));
	return action;
}

bool multiplayer_debrief_compose_report(
	const MultiplayerDebriefReportInput& input,
	const LanguageTable& language,
	const LanguageTable& itac_language,
	MultiplayerDebriefReport& report)
{
	report = {};
	const std::int8_t mission_index =
		report_mission_index(input.mission);
	const std::int16_t grade_value =
		static_cast<std::int16_t>(input.grade);
	const std::uint8_t outcome_value =
		static_cast<std::uint8_t>(input.outcome);
	if (mission_index < 0 || grade_value < -1 || grade_value > 4
		|| outcome_value > static_cast<std::uint8_t>(
			campaign::MissionCoordinatorResult::network_abort)
		|| input.new_medal < -1 || input.new_medal > 5
		|| input.new_bar < -1 || input.new_bar > 4
		|| (input.new_rank != -1
			&& (input.new_rank < 1 || input.new_rank > 8))
		|| (input.new_progression != -1
			&& (input.new_progression < 1
				|| input.new_progression > 3))
		|| !copy_bounded(
			report.recipient,
			sizeof(report.recipient),
			safe_text(input.recipient_callsign)))
	{
		return false;
	}
	const std::uint8_t matrix = grade_value < 0
		? 0
		: static_cast<std::uint8_t>(4 - grade_value);
	const std::uint16_t* debrief =
		kArchiveDebriefs[matrix][mission_index];
	const char* title = language_text(itac_language, debrief[0]);
	if (title[0] == '\0'
		|| !copy_bounded(report.title, sizeof(report.title), title))
	{
		report = {};
		return false;
	}

	const std::uint16_t terminal_text =
		terminal_outcome_text(input.outcome);
	if (terminal_text != 0)
	{
		if (!append_language_paragraph(
			report.body,
			sizeof(report.body),
			language,
			terminal_text))
		{
			report = {};
			return false;
		}
		return true;
	}

	if (input.retry_history != 0)
	{
		if (input.retry_history >= std::size(kRetryWarningText)
			|| !append_language_paragraph(
				report.body,
				sizeof(report.body),
				itac_language,
				input.objectives_completed_before_ejection
					? 0x720
					: 0x721)
			|| !append_language_paragraph(
				report.body,
				sizeof(report.body),
				itac_language,
				kRetryWarningText[input.retry_history]))
		{
			report = {};
			return false;
		}
		return true;
	}

	for (std::uint8_t paragraph = 1;
		paragraph < 9 && debrief[paragraph] != 9;
		++paragraph)
	{
		if (!append_language_paragraph(
			report.body,
			sizeof(report.body),
			itac_language,
			debrief[paragraph]))
		{
			report = {};
			return false;
		}
	}
	if (input.new_medal >= 0
		&& input.new_medal < 6
		&& input.grade == campaign::MissionGrade::perfect
		&& !append_language_paragraph(
			report.body,
			sizeof(report.body),
			itac_language,
			static_cast<std::uint16_t>(205 + input.new_medal)))
	{
		report = {};
		return false;
	}
	if (input.new_rank >= 1
		&& input.new_rank <= 8
		&& !append_language_paragraph(
			report.body,
			sizeof(report.body),
			itac_language,
			kPromotionText[input.new_rank]))
	{
		report = {};
		return false;
	}
	if (input.new_bar >= 0
		&& input.new_bar < 5
		&& !append_language_paragraph(
			report.body,
			sizeof(report.body),
			itac_language,
			static_cast<std::uint16_t>(211 + input.new_bar)))
	{
		report = {};
		return false;
	}
	if (input.new_progression >= 1
		&& input.new_progression <= 3
		&& !append_language_paragraph(
			report.body,
			sizeof(report.body),
			itac_language,
			static_cast<std::uint16_t>(
				237 + input.new_progression)))
	{
		report = {};
		return false;
	}
	return true;
}

void multiplayer_debrief_build(
	MultiplayerDebrief& debrief,
	const MultiplayerDebriefView& view,
	const LanguageTable& language,
	const LanguageTable& itac_language,
	const render::FrontendRenderer& renderer,
	render::FrontendCommands& commands,
	std::uint64_t now)
{
	gui::begin_screen(commands);
	render::frontend_rgba_quad(
		commands,
		renderer.debrief.background,
		0.0f,
		0.0f,
		render::kFrontendWidth,
		render::kFrontendHeight);

	draw_chat(view, renderer, commands, now);
	draw_text(
		commands,
		renderer,
		language_text(language, 0x313),
		167.0f,
		22.0f,
		kTitleTint);

	constexpr std::uint16_t statistic_labels[] = {
		0x1da, 0x1db, 0x1dc, 0x1dd};
	constexpr float statistic_y[] = {
		376.0f, 393.0f, 410.0f, 427.0f};
	for (std::uint8_t index = 0; index < std::size(statistic_labels);
		++index)
	{
		draw_text(
			commands,
			renderer,
			language_text(language, statistic_labels[index]),
			36.0f,
			statistic_y[index],
			kLabelTint);
	}
	char numeric_value[16];
	std::snprintf(
		numeric_value,
		sizeof(numeric_value),
		"%u",
		static_cast<unsigned>(view.mission_kills));
	draw_right_text(
		commands, renderer, numeric_value, 202.0f, 376.0f, kValueTint);
	std::snprintf(
		numeric_value,
		sizeof(numeric_value),
		"%u",
		static_cast<unsigned>(view.overall_kills));
	draw_right_text(
		commands, renderer, numeric_value, 202.0f, 393.0f, kValueTint);
	constexpr std::uint16_t rank_ids[] = {
		0x55c, 0xed, 0xee, 0x1bd, 0x1be,
		0x1bf, 0x1c0, 0xf0, 0x1c1};
	constexpr std::uint16_t level_ids[] = {0xfa, 0xf9, 0xf8, 0xfb};
	draw_right_text(
		commands,
		renderer,
		language_text(
			language,
			rank_ids[std::min<std::uint8_t>(view.rank, 8)]),
		202.0f,
		410.0f,
		kValueTint);
	draw_right_text(
		commands,
		renderer,
		language_text(
			language,
			level_ids[std::min<std::uint8_t>(view.level, 3)]),
		202.0f,
		427.0f,
		kValueTint);

	draw_text(
		commands,
		renderer,
		language_text(language, 0x178),
		517.0f,
		442.0f,
		kValueTint);
	draw_report(
		debrief,
		view,
		itac_language,
		renderer,
		commands);

	if (view.local_is_leader && view.leader_valid)
	{
		draw_text(
			commands,
			renderer,
			language_text(language, 0x39b),
			517.0f,
			368.0f,
			kValueTint);
	}
	else if (!view.local_is_leader)
	{
		draw_text(
			commands,
			renderer,
			language_text(language, 0x145),
			517.0f,
			368.0f,
			kValueTint);
	}
	if (view.local_is_leader)
	{
		draw_text(
			commands,
			renderer,
			language_text(language, 0x296),
			517.0f,
			403.0f,
			kValueTint);
	}

	if (debrief.hovered == 0 || debrief.hovered == 1)
	{
		const std::uint8_t index =
			static_cast<std::uint8_t>(debrief.hovered);
		render::frontend_indexed_quad(
			commands,
			renderer.debrief.scroll_arrow[index],
			renderer.debrief.control_palette,
			index == 0 ? 295.0f : 321.0f,
			201.0f);
	}
	auto draw_action_button = [&](
		std::uint8_t region,
		float y,
		bool visible)
	{
		if (!visible)
		{
			return;
		}
		render::frontend_indexed_quad(
			commands,
			renderer.debrief.action_button[
				debrief.hovered == static_cast<std::int8_t>(region)
					? 0
					: 1],
			renderer.debrief.control_palette,
			530.0f,
			y);
	};
	draw_action_button(
		2,
		364.0f,
		!view.local_is_leader || view.leader_valid);
	draw_action_button(4, 400.0f, view.local_is_leader);
	draw_action_button(5, 438.0f, true);

	draw_text(
		commands,
		renderer,
		language_text(language, 0x295),
		38.0f,
		339.0f,
		kValueTint);
	draw_text(
		commands,
		renderer,
		language_text(language, 0x141),
		225.0f,
		364.0f,
		kValueTint);

	const char* restart_text = language_text(language, 0x181);
	const char* ready_text = language_text(language, 0x145);
	const std::uint8_t player_count = std::min<std::uint8_t>(
		view.player_count,
		kMultiplayerDebriefPlayerCapacity);
	for (std::uint8_t index = 0; index < player_count; ++index)
	{
		const MultiplayerDebriefPlayer& player = view.players[index];
		const float y = 381.0f + index * 13.0f;
		char callsign[kCallsignCapacity + 1];
		copy_truncated_callsign(
			callsign, player.callsign, restart_text, renderer);
		draw_text(
			commands, renderer, callsign, 225.0f, y, kValueTint);
		if (player.ready || player.leader)
		{
			char status[64];
			uppercase_copy(
				status,
				player.restarting ? restart_text : ready_text);
			draw_right_text(
				commands, renderer, status, 379.0f, y, kValueTint);
		}
	}

	const std::uint64_t elapsed =
		now > debrief.entered_at ? now - debrief.entered_at : 0;
	gui::animated_cursor(
		commands,
		renderer.debrief.cursor,
		renderer.debrief.cursor_palette,
		debrief.pointer_x,
		debrief.pointer_y,
		elapsed,
		40);
}
}

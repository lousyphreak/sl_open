#pragma once

#include <cstdint>

namespace sl_open
{
struct LanguageTable;
}

namespace sl_open::campaign
{
struct CampaignState;
}

namespace sl_open::render
{
struct FrontendCommands;
struct FrontendRenderer;
}

namespace sl_open::frontend
{
constexpr std::uint8_t kItacTabCount = 9;
constexpr std::uint8_t kItacExitTab = 8;

enum class ItacPhase : std::uint8_t
{
	prelude,
	initializing,
	entering,
	active,
	leaving,
	report,
	exiting,
	complete,
};

struct ItacShell
{
	ItacPhase phase{ItacPhase::prelude};
	std::uint8_t active_tab{};
	std::uint8_t requested_tab{};
	std::uint8_t scenario_id{1};
	std::uint8_t news_count{1};
	std::uint8_t news_selection{};
	std::uint8_t news_viewport{};
	std::uint8_t news_visible_rows{};
	std::int8_t news_hovered_row{-1};
	std::int16_t news_body_scroll{};
	std::int16_t news_body_scroll_max{};
	std::int16_t news_row_y[12]{};
	std::int16_t news_row_height[12]{};
	std::uint8_t movie_count{1};
	std::uint8_t movie_selection{};
	std::uint8_t movie_visible_rows{};
	std::int8_t movie_hovered_row{-1};
	std::int16_t movie_body_scroll{};
	std::int16_t movie_body_scroll_max{};
	std::int16_t movie_row_y[6]{};
	std::int16_t movie_row_height[6]{};
	std::uint8_t squad_faction{};
	std::uint8_t squad_selection{};
	std::uint8_t squad_viewport{};
	std::uint8_t squad_visible_rows{};
	std::int8_t squad_hovered_row{-1};
	std::int16_t squad_body_scroll{};
	std::int16_t squad_body_scroll_max{};
	std::int16_t squad_row_y[13]{};
	std::int16_t squad_row_height[13]{};
	std::uint8_t pilot_faction{};
	std::uint8_t pilot_selection{};
	std::uint8_t pilot_viewport{};
	std::uint8_t pilot_visible_rows{};
	std::int8_t pilot_hovered_row{-1};
	std::int16_t pilot_body_scroll{};
	std::int16_t pilot_body_scroll_max{};
	std::int16_t pilot_row_y[13]{};
	std::int16_t pilot_row_height[13]{};
	std::uint8_t capital_faction{};
	std::uint8_t capital_selection{};
	std::uint8_t capital_viewport{};
	std::uint8_t capital_visible_rows{};
	std::int8_t capital_hovered_row{-1};
	std::int16_t capital_body_scroll{};
	std::int16_t capital_body_scroll_max{};
	std::int16_t capital_row_y[13]{};
	std::int16_t capital_row_height[13]{};
	std::uint8_t fighter_faction{};
	std::uint8_t fighter_selection{};
	std::uint8_t fighter_visible_rows{};
	std::int8_t fighter_hovered_row{-1};
	std::int16_t fighter_row_y[12]{};
	std::int16_t fighter_row_height[12]{};
	std::uint8_t archive_count{1};
	std::uint8_t archive_selection{};
	std::uint8_t archive_viewport{};
	std::uint8_t archive_visible_rows{};
	std::int8_t archive_hovered_row{-1};
	std::int16_t archive_body_scroll{};
	std::int16_t archive_body_scroll_max{};
	std::int16_t archive_row_y[13]{};
	std::int16_t archive_row_height[13]{};
	std::int8_t archive_grades[23]{};
	std::uint8_t archive_mission_ids[23]{};
	std::uint8_t archive_best_ranks[23]{};
	std::uint16_t archive_retry_history[23]{};
	std::uint16_t archive_mission_kills[23]{};
	std::uint16_t archive_overall_kills{};
	std::uint8_t archive_rank{};
	std::uint8_t archive_level{};
	std::uint8_t kills_count{};
	std::uint8_t kills_viewport{};
	std::uint8_t kills_order[20]{};
	std::uint16_t kills_values[20]{};
	char kills_callsign[50]{};
	std::int8_t hovered_tab{-1};
	float pointer_x{320.0f};
	float pointer_y{240.0f};
	std::uint64_t hover_started_at{};
	bool late_campaign{};
	bool opened_from_news{};
};

void itac_shell_reset(
	ItacShell& shell,
	bool late_campaign,
	bool post_mission,
	std::uint8_t scenario_id,
	const campaign::CampaignState& campaign);
const char* itac_shell_movie(const ItacShell& shell);
bool itac_shell_movie_finished(ItacShell& shell);
void itac_shell_set_pointer(
	ItacShell& shell,
	float x,
	float y,
	bool inside,
	std::uint64_t now);
bool itac_shell_select(ItacShell& shell);
bool itac_shell_exit(ItacShell& shell);
void itac_shell_build(
	ItacShell& shell,
	const LanguageTable& language,
	const LanguageTable& itac_language,
	const render::FrontendRenderer& renderer,
	render::FrontendCommands& commands,
	std::uint64_t now);
}

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
constexpr std::uint8_t kMultiplayerDebriefPlayerCapacity = 8;
constexpr std::uint8_t kMultiplayerDebriefChatLineCapacity = 6;
// FUN_004ae2d0 configures the retail editor with a 0x23-byte field, so the
// terminating zero leaves 34 message characters.
constexpr std::size_t kMultiplayerDebriefChatBytes = 0x23;
// FUN_00425240 writes its composed text at 0x00520844.  The next allocation
// starts after a 10,000-byte buffer and four bytes of alignment padding.
constexpr std::size_t kMultiplayerDebriefReportBodyBytes = 10000;
constexpr std::size_t kMultiplayerDebriefReportTitleBytes = 256;
constexpr std::size_t kMultiplayerDebriefReportRecipientBytes =
	campaign::kCallsignBytes;

struct MultiplayerDebriefPlayer
{
	const char* callsign{};
	bool local{};
	bool ready{};
	bool restarting{};
	bool leader{};
};

// The network/session layer owns these strings and participant flags.  Chat
// line zero is the newest line, matching the retail history traversal.
struct MultiplayerDebriefView
{
	const char* report_title{};
	const char* report_body{};
	const char* report_recipient{};
	const char* chat_lines[kMultiplayerDebriefChatLineCapacity]{};
	const char* chat_entry{};
	MultiplayerDebriefPlayer
		players[kMultiplayerDebriefPlayerCapacity]{};
	std::uint32_t overall_kills{};
	std::uint16_t mission_kills{};
	std::uint8_t chat_line_count{};
	std::uint8_t player_count{};
	std::uint8_t rank{};
	std::uint8_t level{};
	bool local_is_leader{};
	bool leader_valid{};
};

struct MultiplayerDebriefReportInput
{
	const char* recipient_callsign{};
	std::uint16_t mission{1};
	std::uint16_t retry_history{};
	campaign::MissionGrade grade{campaign::MissionGrade::none};
	campaign::MissionCoordinatorResult outcome{
		campaign::MissionCoordinatorResult::ordinary};
	// 0x0052a460 selects whether the objectives were complete before the
	// ejection whose cumulative warning is stored in retry_history.
	bool objectives_completed_before_ejection{};
	// Award indices use CampaignState/AdvanceResult conventions.  Medal and
	// bar are zero-based; rank is 1..8; progression is the new tier 1..3.
	std::int8_t new_medal{-1};
	std::int8_t new_bar{-1};
	std::int8_t new_rank{-1};
	std::int8_t new_progression{-1};
};

struct MultiplayerDebriefReport
{
	char title[kMultiplayerDebriefReportTitleBytes]{};
	char recipient[kMultiplayerDebriefReportRecipientBytes]{};
	char body[kMultiplayerDebriefReportBodyBytes]{};
};

enum class MultiplayerDebriefActionType : std::uint8_t
{
	none,
	ready,
	continue_mission,
	replay_mission,
	leave,
	save_game,
	edit_chat,
	backspace_chat,
	send_chat,
};

struct MultiplayerDebriefAction
{
	MultiplayerDebriefActionType type{
		MultiplayerDebriefActionType::none};
	char text[kMultiplayerDebriefChatBytes]{};
};

struct MultiplayerDebrief
{
	float pointer_x{320.0f};
	float pointer_y{240.0f};
	std::int16_t report_scroll{};
	std::int16_t report_scroll_max{};
	std::int8_t hovered{-1};
	std::uint64_t entered_at{};
	bool ready_sent{};
};

void multiplayer_debrief_reset(
	MultiplayerDebrief& debrief,
	std::uint64_t now);
void multiplayer_debrief_set_pointer(
	MultiplayerDebrief& debrief,
	float x,
	float y,
	bool inside);
MultiplayerDebriefAction multiplayer_debrief_select(
	MultiplayerDebrief& debrief,
	const MultiplayerDebriefView& view);
MultiplayerDebriefAction multiplayer_debrief_text(
	const MultiplayerDebriefView& view,
	const char* text);
MultiplayerDebriefAction multiplayer_debrief_backspace(
	const MultiplayerDebriefView& view);
MultiplayerDebriefAction multiplayer_debrief_submit_chat(
	const MultiplayerDebriefView& view);
bool multiplayer_debrief_compose_report(
	const MultiplayerDebriefReportInput& input,
	const LanguageTable& language,
	const LanguageTable& itac_language,
	MultiplayerDebriefReport& report);
void multiplayer_debrief_build(
	MultiplayerDebrief& debrief,
	const MultiplayerDebriefView& view,
	const LanguageTable& language,
	const LanguageTable& itac_language,
	const render::FrontendRenderer& renderer,
	render::FrontendCommands& commands,
	std::uint64_t now);
}

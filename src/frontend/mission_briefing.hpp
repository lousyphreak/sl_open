#pragma once

#include <cstdint>

namespace sl_open
{
struct LanguageTable;
}

namespace sl_open::media
{
struct BinkMovie;
}

namespace sl_open::render
{
struct FrontendCommands;
struct FrontendRenderer;
}

namespace sl_open::frontend
{
enum class MissionBriefingPhase : std::uint8_t
{
	awaiting_clearance,
	late_amon,
	room_entry,
	mission_video,
	to_hologram,
	hologram,
	from_hologram,
	exit_room,
	complete,
};

struct MissionBriefing
{
	MissionBriefingPhase phase{MissionBriefingPhase::awaiting_clearance};
	std::uint8_t mission{1};
	std::uint8_t animation_phase{};
	std::uint16_t animation_frame{};
	bool late_campaign{};
	bool exit_speech_triggered{};
	float pointer_x{320.0f};
	float pointer_y{200.0f};
	std::uint64_t entered_at{};
	std::uint64_t animation_at{};
};

void mission_briefing_reset(
	MissionBriefing& briefing,
	std::uint8_t mission,
	std::uint64_t now);
bool mission_briefing_wait_finished(
	MissionBriefing& briefing,
	std::uint64_t now);
const char* mission_briefing_movie(const MissionBriefing& briefing);
bool mission_briefing_movie_finished(
	MissionBriefing& briefing,
	std::uint64_t now);
bool mission_briefing_begin_hologram_exit(
	MissionBriefing& briefing,
	std::uint64_t now);
bool mission_briefing_exit_finished(
	MissionBriefing& briefing,
	std::uint64_t now);
bool mission_briefing_trigger_exit_speech(MissionBriefing& briefing);
void mission_briefing_pointer(
	MissionBriefing& briefing,
	float x,
	float y);
void mission_briefing_update_animation(
	MissionBriefing& briefing,
	std::uint64_t now);
void mission_briefing_build(
	const MissionBriefing& briefing,
	const LanguageTable& language,
	const render::FrontendRenderer& renderer,
	const media::BinkMovie& movie,
	render::FrontendCommands& commands,
	std::uint64_t now);
}

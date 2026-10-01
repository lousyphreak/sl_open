#pragma once

#include "platform/app_internal.hpp"

namespace sl_open::platform
{
int play_fat_sample(
	App& app,
	const audio::FatBank& bank,
	std::uint32_t sample,
	int volume = 127,
	int loop_count = 1);
void play_frontend_click(App& app);
void play_walk_sound(App& app, std::uint32_t sample);
void start_hub_actor(App& app, const char* transition);
void start_hub_ambience(App& app);
void stop_hub_ambience(App& app);
void stop_briefing_voice(App& app, int& voice);
void stop_briefing_room_chatter(App& app);

void enter_main_menu(App& app, std::uint64_t now);
void enter_options(App& app, std::uint64_t now);
void enter_campaign(App& app, std::uint64_t now);
void enter_multiplayer(App& app, std::uint64_t now);
void prepare_campaign_hub(App& app);
void enter_campaign_hub(
	App& app,
	std::uint8_t node_index,
	std::uint64_t now);
void advance_startup_logos(App& app, std::uint64_t now);
void enter_transition_destination(App& app, std::uint64_t now);
void begin_transition(
	App& app,
	const char* path,
	TransitionDestination destination,
	std::uint64_t now,
	bool always_play = false,
	bool skip_directly = false,
	TransitionAudio audio = TransitionAudio::primary);
}

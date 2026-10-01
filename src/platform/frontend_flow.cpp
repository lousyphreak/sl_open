#include "platform/frontend_flow.hpp"

#include <bgfx/bgfx.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iterator>

namespace sl_open::platform
{
namespace
{
struct HubActorRecord
{
	const char* sprite;
	std::uint16_t start_frame;
	std::uint16_t frame_count;
	const char* sound;
};

const HubActorRecord& choose_hub_actor(
	const campaign::CampaignState& campaign)
{
	static constexpr HubActorRecord kEarly[3][6] = {
		{
			{"rovh", 93, 89, "rm06p"},
			{"rovi", 93, 93, "rm07p"},
			{"rovk", 93, 96, "rm10p"},
			{"rovl", 93, 106, "rm11p"},
		},
		{
			{"rovm", 93, 98, "rm02n"},
			{"rovn", 93, 89, "rm04n"},
			{"rovo", 93, 106, "rm06n"},
			{"rovp", 93, 90, "rm09n"},
			{"rovq", 93, 98, "rm14n"},
			{"rovr", 93, 96, "rm15n"},
		},
		{
			{"rovc", 93, 100, "rm01g"},
			{"rovd", 93, 92, "rm02g"},
			{"rove", 93, 93, "rm03g"},
			{"rovf", 93, 97, "rm04g"},
			{"rovg", 93, 105, "rm06g"},
		},
	};
	static constexpr std::uint8_t kEarlyCount[3] = {4, 6, 5};
	static constexpr HubActorRecord kLate[3][5] = {
		{
			{"yovg", 2, 64, "ym19p"},
			{"yovh", 2, 75, "ym20p"},
			{"yovj", 2, 82, "ym22p"},
			{"yovk", 2, 66, "ym23p"},
			{"yovl", 2, 82, "ym24p"},
		},
		{
			{"yovm", 2, 71, "ym19n"},
			{"yovn", 2, 80, "ym20n"},
			{"yovp", 2, 78, "ym22n"},
			{"yovq", 2, 101, "ym23n"},
			{"yovr", 2, 86, "ym24n"},
		},
		{
			{"yovb", 2, 97, "ym20g"},
			{"yovc", 2, 116, "ym21g"},
			{"yovf", 2, 84, "ym24g"},
		},
	};
	static constexpr std::uint8_t kLateCount[3] = {5, 5, 3};
	static constexpr std::uint8_t kMedalMission[29] = {
		0, 0, 0, 0, 0, 0, 1, 0, 0, 0,
		0, 2, 0, 0, 0, 0, 3, 0, 0, 0,
		0, 4, 0, 5, 0, 0, 0, 6, 0,
	};

	const std::uint16_t previous =
		campaign.mission > 1 ? campaign.mission - 1 : 1;
	std::uint8_t variant = 2;
	if (previous < 29 && kMedalMission[previous] != 0)
	{
		variant = 0;
	}
	else if (previous >= 1 && previous <= campaign::kMissionCount
		&& campaign.mission_results[previous - 1]
			!= campaign::MissionGrade::none)
	{
		variant = 1;
	}

	const bool late = campaign.mission >= 19;
	const std::uint8_t count =
		late ? kLateCount[variant] : kEarlyCount[variant];
	const std::uint8_t choice =
		static_cast<std::uint8_t>(std::rand() % count);
	return late ? kLate[variant][choice] : kEarly[variant][choice];
}

bool load_hub_actor_sprite(
	App& app,
	std::uint32_t slot,
	const HubActorRecord& record)
{
	app.hub_actor_start_frame[slot] = record.start_frame;
	app.hub_actor_frame_count[slot] = record.frame_count;
	app.hub_actor_active[slot] = false;

	char path[io::kMaxPath];
	std::snprintf(path, sizeof(path), "%s.spr", record.sprite);
	assets::SpriteList sprites;
	if (!assets::load_sprite_list(app.vfs, path, sprites)
		|| !render::frontend_vr_ambient_assets_init(
			app.frontend_renderer, slot, &sprites, record.frame_count))
	{
		SDL_Log("VR actor sprites could not be opened: %s", path);
		render::frontend_vr_ambient_assets_init(
			app.frontend_renderer, slot, nullptr, 0);
		return false;
	}
	return true;
}

void select_hub_actor_assets(App& app)
{
	const HubActorRecord& primary = choose_hub_actor(app.campaign);
	std::snprintf(
		app.hub_actor_sound_stem,
		sizeof(app.hub_actor_sound_stem),
		"%s",
		primary.sound);
	load_hub_actor_sprite(app, 0, primary);

	static constexpr HubActorRecord kLateSecondary[3] = {
		{"yovs", 117, 85, ""},
		{"yovt", 117, 74, ""},
		{"yovu", 117, 79, ""},
	};
	if (app.campaign.mission >= 19)
	{
		load_hub_actor_sprite(
			app, 1, kLateSecondary[app.hub_secondary_cycle]);
		app.hub_secondary_cycle =
			static_cast<std::uint8_t>(
				(app.hub_secondary_cycle + 1) % 3);
	}
	else
	{
		app.hub_actor_start_frame[1] = 0;
		app.hub_actor_frame_count[1] = 0;
		app.hub_actor_active[1] = false;
		render::frontend_vr_ambient_assets_init(
			app.frontend_renderer, 1, nullptr, 0);
	}
}

template <std::size_t N>
bool is_cursor_texture(
	bgfx::TextureHandle handle,
	const render::FrontendTexture (&textures)[N])
{
	if (!bgfx::isValid(handle))
	{
		return false;
	}
	for (const render::FrontendTexture& texture : textures)
	{
		if (bgfx::isValid(texture.handle)
			&& handle.idx == texture.handle.idx)
		{
			return true;
		}
	}
	return false;
}

void remove_held_cursor(App& app)
{
	std::uint32_t destination = 0;
	for (std::uint32_t source = 0;
		source < app.frontend_commands.count;
		++source)
	{
		const render::FrontendCommand& command =
			app.frontend_commands.items[source];
		const bgfx::TextureHandle texture = command.texture;
		const bool cursor =
			command.type == render::FrontendCommandType::loadout_cursor
			|| is_cursor_texture(texture, app.frontend_renderer.shell.cursor)
			|| is_cursor_texture(
				texture,
				app.frontend_renderer.shell.in_game_options_cursor)
			|| is_cursor_texture(
				texture,
				app.frontend_renderer.shell.options_detail_cursor)
			|| is_cursor_texture(
				texture,
				app.frontend_renderer.shell.control_options_cursor)
			|| is_cursor_texture(
				texture, app.frontend_renderer.campaign.cursor)
			|| is_cursor_texture(texture, app.frontend_renderer.vr.cursor)
			|| is_cursor_texture(
				texture, app.frontend_renderer.debrief.cursor)
			|| is_cursor_texture(
				texture, app.frontend_renderer.restart.cursor);
		if (!cursor)
		{
			app.frontend_commands.items[destination++] = command;
		}
	}
	app.frontend_commands.count = destination;
}
}

int play_fat_sample(
	App& app,
	const audio::FatBank& bank,
	std::uint32_t sample,
	int volume,
	int loop_count)
{
	if (bank.ready)
	{
		return audio::fat_play_auto(
			app.audio, bank, sample, volume, loop_count, 64, 0);
	}
	return -1;
}

void play_frontend_click(App& app)
{
	play_fat_sample(app, app.standard_sounds, 11);
}

void play_walk_sound(App& app, std::uint32_t sample)
{
	play_fat_sample(app, app.walk_sounds, sample);
}

void start_hub_actor(App& app, const char* transition)
{
	audio::mp3_stream_close(app.vfs, app.hub_actor_sound);
	app.hub_actor_active[0] = false;
	app.hub_actor_active[1] = false;
	const bool actor_transition =
		std::strcmp(transition, "rel_bunkroom2briefing_door.bik") == 0
		|| std::strcmp(transition, "bunk2wr.bik") == 0;
	if (!actor_transition || app.hub_actor_sound_stem[0] == '\0')
	{
		return;
	}
	app.hub_actor_active[0] = true;
	app.hub_actor_active[1] =
		std::strcmp(transition, "bunk2wr.bik") == 0
		&& app.campaign.mission >= 19;

	char path[io::kMaxPath];
	std::snprintf(
		path, sizeof(path), "%s.mp3", app.hub_actor_sound_stem);
	const audio::Stream& stream = app.audio.streams[3];
	if (!audio::mp3_stream_open(
			app.vfs,
			path,
			app.audio.ready ? stream.source : 0,
			stream.buffers,
			false,
			app.hub_actor_sound))
	{
		SDL_Log("VR actor sound could not be opened: %s", path);
	}
	audio::apply_stream_gains(app.audio);
}

void start_hub_ambience(App& app)
{
	if (app.hub_ambience_voice >= 0 || !app.vr_sounds.ready)
	{
		return;
	}
	app.hub_ambience_voice = audio::fat_play_auto(
		app.audio,
		app.vr_sounds,
		app.campaign_hub.late_campaign ? 5 : 4,
		80,
		0,
		64,
		0);
}

void stop_hub_ambience(App& app)
{
	if (app.hub_ambience_voice >= 0)
	{
		audio::fat_stop(
			app.audio,
			static_cast<std::uint32_t>(app.hub_ambience_voice));
		app.hub_ambience_voice = -1;
	}
}

void stop_briefing_voice(App& app, int& voice)
{
	if (voice < 0)
	{
		return;
	}
	audio::fat_stop(
		app.audio,
		static_cast<std::uint32_t>(voice));
	voice = -1;
}

void stop_briefing_room_chatter(App& app)
{
	for (int& voice : app.briefing_room_voices)
	{
		stop_briefing_voice(app, voice);
	}
}

void enter_main_menu(App& app, std::uint64_t now)
{
	media::bink_movie_stop(app.vfs, app.transition_movie);
	SDL_StopTextInput(app.window);
	app.frontend_phase = FrontendPhase::main_menu;
	app.transition_holds_frame = false;
	app.main_menu.entered_at = now;
	if (app.audio.ready
		&& !app.menu_music.active
		&& !audio::wav_stream_open(
			app.vfs,
			"music/New_Pensive.wav",
			app.audio.streams[0].source,
			app.audio.streams[0].buffers,
			true,
			app.menu_music))
	{
		SDL_Log("Menu music could not be opened");
	}
	audio::apply_stream_gains(app.audio);
}

void enter_options(App& app, std::uint64_t now)
{
	media::bink_movie_stop(app.vfs, app.transition_movie);
	SDL_StopTextInput(app.window);
	app.frontend_phase = FrontendPhase::options;
	app.transition_holds_frame = false;
	app.options.entered_at = now;
}

void enter_campaign(App& app, std::uint64_t now)
{
	media::bink_movie_stop(app.vfs, app.transition_movie);
	app.frontend_phase = FrontendPhase::campaign;
	app.transition_holds_frame = false;
	app.campaign_frontend.entered_at = now;
	SDL_StartTextInput(app.window);
}

void enter_multiplayer(App& app, std::uint64_t now)
{
	media::bink_movie_stop(app.vfs, app.transition_movie);
	network::multiplayer_transport_initialize(
		app.multiplayer_transport);
	frontend::multiplayer_frontend_enter(
		app.multiplayer_frontend, now);
	app.frontend_phase = FrontendPhase::multiplayer_frontend;
	app.transition_holds_frame = false;
	SDL_StopTextInput(app.window);
}

void prepare_campaign_hub(App& app)
{
	audio::wav_stream_close(app.vfs, app.menu_music);
	audio::mp3_stream_close(app.vfs, app.hub_actor_sound);
	stop_hub_ambience(app);
	frontend::campaign_hub_reset(
		app.campaign_hub, app.campaign.mission >= 19);
	select_hub_actor_assets(app);
}

void enter_campaign_hub(
	App& app,
	std::uint8_t node_index,
	std::uint64_t now)
{
	media::bink_movie_stop(app.vfs, app.transition_movie);
	prepare_campaign_hub(app);
	frontend::campaign_hub_enter(app.campaign_hub, node_index);
	const frontend::HubNode& node =
		frontend::campaign_hub_node(app.campaign_hub);
	start_hub_actor(app, node.primary_movie);
	char path[io::kMaxPath];
	std::snprintf(path, sizeof(path), "vr/%s", node.primary_movie);
	const audio::Stream& stream = app.audio.streams[2];
	if (!media::bink_movie_open(
			app.vfs,
			path,
			app.audio.ready ? stream.source : 0,
			stream.buffers,
			now,
			app.campaign_movie))
	{
		SDL_Log("Campaign hub movie could not be opened: %s", path);
	}
	app.frontend_phase = FrontendPhase::campaign_hub;
	app.transition_holds_frame = false;
	SDL_StopTextInput(app.window);
	start_hub_ambience(app);
}

void advance_startup_logos(App& app, std::uint64_t now)
{
	constexpr const char* kStartupLogos[] = {
		"new_nms.bik",
		"new_dalogo_fs_uncmpr.bik",
		"warty_.bik",
	};
	if (app.startup_logo_index < std::size(kStartupLogos))
	{
		const char* path = kStartupLogos[app.startup_logo_index++];
		begin_transition(
			app,
			path,
			TransitionDestination::startup_logo,
			now,
			true,
			true);
		return;
	}
	begin_transition(
		app,
		"SPLASH TO MM.bik",
		TransitionDestination::main_menu,
		now,
		false,
		true);
}

void enter_transition_destination(App& app, std::uint64_t now)
{
	switch (app.transition_destination)
	{
	case TransitionDestination::startup_logo:
		advance_startup_logos(app, now);
		break;
	case TransitionDestination::main_menu:
		enter_main_menu(app, now);
		break;
	case TransitionDestination::options:
		enter_options(app, now);
		break;
	case TransitionDestination::campaign:
		enter_campaign(app, now);
		break;
	case TransitionDestination::campaign_hub:
		enter_campaign_hub(
			app, app.campaign.mission >= 19 ? 50 : 0, now);
		break;
	case TransitionDestination::multiplayer:
		enter_multiplayer(app, now);
		break;
	}
}

void begin_transition(
	App& app,
	const char* path,
	TransitionDestination destination,
	std::uint64_t now,
	bool always_play,
	bool skip_directly,
	TransitionAudio audio)
{
	app.transition_destination = destination;
	app.transition_skips_directly = skip_directly;
	app.transition_holds_frame =
		app.frontend_renderer.ready && app.frontend_commands.count != 0;
	if (app.transition_holds_frame)
	{
		remove_held_cursor(app);
	}
	if (!always_play && !app.config.transitions)
	{
		enter_transition_destination(app, now);
		return;
	}
	const audio::Stream& movie_stream = app.audio.streams[
		audio == TransitionAudio::in_game_options ? 4 : 2];
	if (!media::bink_movie_open(
			app.vfs,
			path,
			app.audio.ready ? movie_stream.source : 0,
			movie_stream.buffers,
			now,
			app.transition_movie))
	{
		SDL_Log("Frontend transition could not be opened: %s", path);
		enter_transition_destination(app, now);
		return;
	}
	app.frontend_phase = FrontendPhase::transition;
}
}

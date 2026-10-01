#pragma once

#include <SDL3/SDL.h>

#include "assets/game_stats.hpp"
#include "assets/image.hpp"
#include "assets/ship_model.hpp"
#include "assets/vfx.hpp"
#include "audio/audio.hpp"
#include "audio/cb97_stream.hpp"
#include "audio/fat.hpp"
#include "audio/mp3.hpp"
#include "audio/music.hpp"
#include "audio/sound3d.hpp"
#include "audio/wav.hpp"
#include "campaign/campaign.hpp"
#include "campaign/cinematics.hpp"
#include "campaign/multiplayer_progression.hpp"
#include "config/config.hpp"
#include "frontend/campaign_frontend.hpp"
#include "frontend/campaign_hub.hpp"
#include "frontend/campaign_induction.hpp"
#include "frontend/cd_player.hpp"
#include "frontend/credits.hpp"
#include "frontend/itac_shell.hpp"
#include "frontend/loadout.hpp"
#include "frontend/main_menu.hpp"
#include "frontend/medal_display.hpp"
#include "frontend/mission_briefing.hpp"
#include "frontend/multiplayer_debrief.hpp"
#include "frontend/multiplayer_frontend.hpp"
#include "frontend/multiplayer_lobby.hpp"
#include "frontend/news_report.hpp"
#include "frontend/options.hpp"
#include "frontend/post_mission_choice.hpp"
#include "frontend/sim_pod.hpp"
#include "game/mission_session.hpp"
#include "hud/movie.hpp"
#include "io/vfs.hpp"
#include "localization/language.hpp"
#include "media/bink_player.hpp"
#include "network/multiplayer_transport.hpp"
#include "platform/input_queue.hpp"
#include "input/gameplay_input.hpp"
#include "render/frontend_renderer.hpp"
#include "render/mission_renderer.hpp"

#include <cstdint>
#include <vector>

namespace sl_open::platform
{
enum class FrontendPhase : std::uint8_t
{
	transition,
	main_menu,
	options,
	campaign,
	campaign_induction,
	campaign_hub,
	campaign_briefing,
	campaign_loadout,
	campaign_takeoff_movie,
	campaign_award_movie,
	campaign_failure_movie,
	campaign_post_mission_movie,
	campaign_restart_choice,
	campaign_sim_pod,
	campaign_news,
	campaign_cd,
	campaign_medals,
	campaign_itac,
	multiplayer_frontend,
	multiplayer_lobby,
	multiplayer_campaign_load,
	multiplayer_loadout,
	multiplayer_launch_wait,
	multiplayer_debrief,
	mission_loading,
	instant_action,
	credits,
};

enum class TransitionDestination : std::uint8_t
{
	startup_logo,
	main_menu,
	options,
	campaign,
	campaign_hub,
	multiplayer,
};

enum class TransitionAudio : std::uint8_t
{
	primary,
	in_game_options,
};

enum class StartupError : std::uint8_t
{
	none,
	data_not_found,
	archive_error,
};

enum class MissionFailureKind : std::uint8_t
{
	destroyed,
	interrupted,
	executed,
};

enum class CampaignSequencePurpose : std::uint8_t
{
	none,
	post_mission,
	transfer_exit,
	final_chapter,
	multiplayer_post_mission,
};

enum class MultiplayerMissionPresentation : std::uint8_t
{
	none,
	post_mission,
	terminal,
	award,
	final_chapter,
};

struct FrontendUploadAssets
{
	assets::TextureImage splash;
	assets::TextureImage mission_loading_splash;
	assets::TextureImage options_background;
	assets::TextureImage options_detail_background;
	assets::TextureImage in_game_options_background;
	assets::TextureImage in_game_options_detail_background;
	assets::TextureImage gameplay_powerball;
	assets::TextureImage campaign_background;
	assets::TextureImage save_load_background;
	assets::TextureImage multiplayer_background;
	assets::TextureImage multiplayer_lobby_background;
	assets::TextureImage cd_early_background;
	assets::TextureImage cd_late_background;
	assets::TextureImage itac_backgrounds[8];
	assets::TextureImage briefing_doors[2];
	assets::TextureImage loadout_backgrounds[2];
	assets::TextureImage loadout_panels;
	assets::TextureImage loadout_disc[4];
	assets::TextureImage loadout_glow;
	assets::TextureImage loadout_hardpoints;
	assets::TextureImage loadout_ship_textures[12];
	assets::ShipModel loadout_missiles[10];
	assets::TextureImage loadout_missile_texture;
	assets::Font loadout_title_font;
	assets::Font loadout_info_font;
	std::uint8_t loadout_palette[256 * 4];
	assets::TextureImage debrief_background;
	assets::SpriteList frontend_sprites;
	assets::SpriteList options_sprites;
	assets::SpriteList options_detail_sprites;
	assets::SpriteList control_options_sprites;
	assets::SpriteList in_game_options_sprites;
	assets::SpriteList gameplay_hud_sprites;
	assets::SpriteList gameplay_scoreboard_sprites;
	assets::SpriteList about_sprites;
	assets::SpriteList quit_sprites;
	assets::SpriteList campaign_sprites;
	assets::SpriteList multiplayer_sprites;
	assets::SpriteList vr_sprites;
	assets::SpriteList early_briefing_sprites;
	assets::SpriteList late_briefing_sprites;
	assets::SpriteList early_briefing_exit_sprites;
	assets::SpriteList late_briefing_exit_sprites;
	assets::SpriteList loadout_sprites;
	assets::ShipModel loadout_ships[12];
	assets::ShipModel loadout_guns[12];
	assets::SpriteList debrief_sprites;
	assets::SpriteList restart_sprites;
	assets::SpriteList cd_sprites;
	assets::SpriteList early_medal_sprites[3];
	assets::SpriteList early_bar_sprites[3];
	assets::SpriteList late_medal_sprites[6];
	assets::SpriteList late_bar_sprites[5];
	assets::SpriteList itac_news_sprites;
	assets::SpriteList itac_video_sprites;
	assets::SpriteList itac_gfx_sprites;
	assets::SpriteList itac_squad_sprites;
	assets::SpriteList itac_person_sprites;
	assets::SpriteList itac_capital_sprites;
	assets::SpriteList itac_fighter_sprites;
	assets::SpriteList itac_kills_sprites;
	assets::SpriteList credits_sprites;
	assets::TextureImage sim_pod_backgrounds[2];
	assets::SpriteList sim_pod_sprites;
	assets::Font options_font;
	assets::Font pause_small_font;
	assets::Font gameplay_hud_font;
	assets::Font gameplay_scoreboard_font;
	assets::Font gameplay_message_font;
	assets::Font itac_font;
	assets::Font itac_small_font;
	assets::Font credits_font;
};

struct App
{
	SDL_EMFS_Context* filesystem;
	SDL_Window* window;
	SDL_Renderer* startup_renderer;
	SDL_Gamepad* gamepad;
	SDL_Joystick* joystick;
	SDL_JoystickID controller_instance;
	std::uint32_t width;
	std::uint32_t height;
	std::uint32_t frame_limit;
	std::uint32_t frame_count;
	std::uint32_t screenshot_index{};
	std::uint16_t requested_mission;
	const char* requested_data_root;
	char data_root[io::kMaxPath];
	io::Vfs vfs;
	LanguageTable language;
	LanguageTable itac_language;
	assets::GameStats game_stats;
	FrontendUploadAssets upload_assets;
	audio::Runtime audio;
	audio::WavStream menu_music;
	audio::MusicRuntime mission_music;
	audio::WavStream cd_music;
	audio::WavStream credits_music;
	audio::Cb97Stream campaign_speech;
	audio::Cb97Stream mission_speech;
	audio::Cb97Stream mission_command_speech;
	std::vector<audio::Cb97Asset> mission_speech_assets;
	std::vector<audio::WavAsset> mission_music_assets;
	std::vector<hud::MovieAsset> mission_movie_assets;
	audio::Mp3Stream hub_actor_sound;
	audio::FatBank standard_sounds;
	audio::FatBank spatial_sounds;
	audio::Sound3DRuntime sound3d;
	audio::FatBank vr_sounds;
	audio::FatBank walk_sounds;
	audio::FatBank briefing_sounds;
	audio::FatBank briefing_wait_sounds;
	audio::FatBank loadout_sounds;
	audio::FatBank campaign_cinematic_sounds;
	Config config;
	campaign::CampaignStore campaign_store;
	campaign::CampaignState campaign;
	campaign::CampaignState mission_checkpoint;
	campaign::CampaignState instant_action_campaign_snapshot;
	campaign::CampaignState multiplayer_local_profile;
	campaign::CampaignState multiplayer_campaign;
	campaign::CampaignState multiplayer_pending_campaign;
	campaign::MultiplayerCampaignProgression
		multiplayer_progression;
	campaign::MultiplayerMissionResolution
		multiplayer_pending_resolution;
	media::BinkMovie transition_movie;
	media::BinkMovie campaign_movie;
	media::BinkMovie mission_foster_movie;
	std::uint64_t mission_foster_paused_voices{};
	std::uint8_t mission_foster_paused_streams{};
	bool mission_foster_active{};
	render::FrontendRenderer frontend_renderer;
	render::FrontendCommands frontend_commands;
	render::MissionRenderer mission_renderer;
	InputQueue input_queue;
	input::GameplayDeviceState gameplay_devices;
	frontend::MainMenu main_menu;
	frontend::OptionsMenu options;
	frontend::CampaignFrontend campaign_frontend;
	frontend::CampaignInduction campaign_induction;
	frontend::CampaignHub campaign_hub;
	frontend::MissionBriefing mission_briefing;
	frontend::MultiplayerFrontend multiplayer_frontend;
	frontend::MultiplayerLobby multiplayer_lobby;
	frontend::MultiplayerDebrief multiplayer_debrief;
	frontend::Loadout loadout;
	frontend::LoadoutCatalog loadout_catalog;
	frontend::PostMissionChoice post_mission_choice;
	frontend::SimPod sim_pod;
	frontend::NewsReport news_report;
	frontend::CdPlayer cd_player;
	frontend::MedalDisplay medal_display;
	frontend::ItacShell itac_shell;
	frontend::Credits credits;
	game::MissionSession mission_session;
	game::MissionLaunchRequest pending_mission_request;
	network::MultiplayerTransport multiplayer_transport;
	frontend::DisplayModes display_modes;
	bool data_root_looks_valid;
	bool frontend_assets_ready;
	bool loose_first;
	bool bgfx_ready;
	bool joystick_available;
	bool relative_mouse_requested;
	bool transition_holds_frame;
	bool transition_skips_directly;
	bool induction_narration_started;
	bool news_narration_started;
	bool briefing_speech_started;
	bool mission_checkpoint_valid{};
	bool instant_action_snapshot_valid{};
	bool alternate_mission_25{};
	bool itac_post_mission{};
	bool campaign_sequence_waiting_audio{};
	bool campaign_profile_save_pending{};
	bool mission_lock_tone_active{};
	bool mission_scanner_tone_active{};
	bool mission_threat_tone_active{};
	bool mission_clock_warmup{};
	bool mission_loading_presented{};
	bool multiplayer_local_profile_valid{};
	bool multiplayer_campaign_valid{};
	bool multiplayer_campaign_loaded_save{};
	bool multiplayer_campaign_load_active{};
	bool multiplayer_campaign_save_from_debrief{};
	bool multiplayer_ai_turrets{};
	bool multiplayer_internal_relaunch{};
	bool multiplayer_report_valid{};
	bool multiplayer_local_outcome_reported{};
	bool multiplayer_result_applied{};
	bool multiplayer_pending_campaign_valid{};
	bool multiplayer_profile_save_pending{};
	bool multiplayer_completed_mission_25_alternate{};
	bool multiplayer_pending_gameplay_valid{};
	bool multiplayer_network_aborted{};
	std::uint32_t mission_scanner_beep_serial{};
	std::uint8_t startup_logo_index{};
	std::uint8_t takeoff_movie_index{};
	int hub_ambience_voice{-1};
	int briefing_wait_voice{-1};
	int briefing_room_voices[2]{-1, -1};
	int loadout_ambience_voice{-1};
	int campaign_cinematic_voice{-1};
	char hub_actor_sound_stem[20]{};
	char multiplayer_callsign[game::kMultiplayerPlayerNameBytes]{};
	char multiplayer_session_name[
		network::kMultiplayerSessionNameBytes]{};
	char multiplayer_lobby_chat_entry[
		frontend::kMultiplayerLobbyChatBytes]{};
	char multiplayer_lobby_chat_lines[
		frontend::kMultiplayerLobbyChatLineCapacity][
			network::kMultiplayerSessionNameBytes
				+ mission::kNetworkChatBytes + 4]{};
	char multiplayer_debrief_chat_entry[
		frontend::kMultiplayerDebriefChatBytes]{};
	char multiplayer_debrief_chat_lines[
		frontend::kMultiplayerDebriefChatLineCapacity][
			game::kMultiplayerPlayerNameBytes
				+ frontend::kMultiplayerDebriefChatBytes + 4]{};
	frontend::MultiplayerDebriefReport multiplayer_report;
	mission::NetworkOutboundMessage multiplayer_pending_gameplay;
	std::uint16_t hub_actor_start_frame[2]{};
	std::uint16_t hub_actor_frame_count[2]{};
	bool hub_actor_active[2]{};
	std::uint8_t hub_secondary_cycle{};
	std::uint8_t cd_volume{127};
	StartupError startup_error;
	FrontendPhase frontend_phase;
	FrontendPhase options_return_phase{FrontendPhase::campaign_hub};
	TransitionDestination transition_destination;
	campaign::AdvanceResult pending_advance;
	campaign::CampaignMovieSequence campaign_sequence;
	CampaignSequencePurpose campaign_sequence_purpose{
		CampaignSequencePurpose::none};
	MultiplayerMissionPresentation
		multiplayer_mission_presentation{
			MultiplayerMissionPresentation::none};
	campaign::MissionCoordinatorResult pending_mission_result{
		campaign::MissionCoordinatorResult::ordinary};
	campaign::MissionGrade pending_mission_grade{
		campaign::MissionGrade::none};
	std::int32_t pending_score_delta{};
	std::uint16_t pending_score_events{};
	std::uint8_t campaign_sequence_index{};
	std::uint8_t multiplayer_lobby_chat_count{};
	std::uint8_t multiplayer_debrief_chat_count{};
};
}

#pragma once

#include "assets/missile_stats.hpp"
#include "game/camera_runtime.hpp"
#include "game/world.hpp"
#include "hud/movie.hpp"
#include "input/gameplay_input.hpp"
#include "mission/runtime.hpp"
#include "render/frontend_renderer.hpp"

#include <cstdint>
#include <vector>

namespace sl_open::hud
{
constexpr std::uint32_t kPanelCount = 15;
constexpr std::uint32_t kTimedMessageCount = 4;
constexpr std::uint32_t kTimedMessageBytes = 100;
constexpr std::uint32_t kChatMessageBytes = 64;
constexpr float kChatMessageWidth = 300.0f;

enum class PanelAnimation : std::uint8_t
{
	closed,
	opening,
	closing,
	open,
};

struct PanelState
{
	PanelAnimation animation{PanelAnimation::closed};
	std::int32_t countdown{};
	std::int32_t animation_time{};
	bool hold{};
};

struct TimedMessage
{
	char text[kTimedMessageBytes]{};
	std::uint32_t expiry{};
};

struct OrdnanceEntry
{
	std::int16_t count{-1};
	std::int16_t ring_position{};
	std::int16_t shape{};
	std::int16_t language_id{};
	std::int16_t type{};
};

enum class MissileLockPhase : std::uint8_t
{
	idle,
	opening,
	acquiring,
	locked,
	forced_release,
	closing,
};

struct MissileLockState
{
	MissileLockPhase phase{MissileLockPhase::idle};
	game::ObjectHandle target;
	std::int16_t component{-1};
	std::int16_t ordnance_type{-1};
	std::int32_t opening_percent{100};
	std::int32_t acquisition_phase{};
	std::int32_t rotation_degrees{};
	std::int32_t carried_rotation_degrees{};
	glm::vec3 retained_point{};
	bool tone_active{};
};

struct HudBlinkChannel
{
	std::int32_t mode{};
	std::uint32_t phase{};
	std::uint32_t serial{};
};

struct HudSoundEvent
{
	std::uint8_t sample{};
	std::uint8_t volume{};
};

struct PanelRenderSnapshot
{
	std::vector<render::FrontendCommand> commands;
	bool valid{};
};

struct Runtime
{
	Movie movie;
	MissileLockState missile_lock;
	PanelState panels[kPanelCount];
	PanelRenderSnapshot target_panel_snapshots[2];
	TimedMessage messages[kTimedMessageCount];
	OrdnanceEntry ordnance[10];
	HudBlinkChannel hud_icons[20];
	HudSoundEvent sound_events[16];
	char chat_message[kChatMessageBytes]{};
	std::uint8_t sound_read{};
	std::uint8_t sound_count{};
	std::uint8_t message_count{};
	std::uint8_t ordnance_count{};
	std::uint8_t selected_ordnance{};
	std::uint8_t sound_camera_mode{};
	std::uint8_t sensor_mode{2};
	std::int16_t sensor_shape{363};
	std::int16_t sensor_destination{363};
	std::int8_t sensor_direction{-1};
	std::uint32_t sensor_deadline{};
	game::ObjectHandle center_contact;
	std::uint16_t tracked_contact{UINT16_MAX};
	game::ObjectHandle previous_target;
	std::uint16_t previous_objective{UINT16_MAX};
	game::ObjectHandle missile_lock_log_target;
	std::int16_t missile_lock_log_type{-1};
	std::uint32_t lock_animation_deadline{};
	std::uint32_t scanner_serial{};
	std::uint32_t scanner_beep_deadline{};
	std::uint32_t scanner_beep_serial{};
	std::uint32_t ordnance_empty_sound_deadline{};
	std::uint32_t threat_warning_phase{};
	std::uint32_t death_warning_phase{};
	std::uint32_t request_blink_phase{};
	std::uint32_t distortion_random_seed{};
	std::uint32_t last_hit_sound_tick{};
	std::uint32_t comms_request_serial{};
	std::int32_t aggregate_ordnance{};
	std::int32_t comms_static_time{};
	std::uint16_t status_icon_mask{};
	std::uint16_t request_blink_shape{UINT16_MAX};
	std::int16_t chat_destination{-1};
	std::int32_t ecm_charge{2000};
	std::int32_t cloak_charge{10000};
	std::int32_t spectral_charge{6000};
	std::int16_t comms_contact_class{};
	std::uint8_t lock_animation_frame{};
	std::uint8_t hud_icon_grid_slot[9]{
		UINT8_MAX, UINT8_MAX, UINT8_MAX,
		UINT8_MAX, UINT8_MAX, UINT8_MAX,
		UINT8_MAX, UINT8_MAX, UINT8_MAX,
	};
	float pointer_x{};
	float pointer_y{};
	float reticle_x{};
	float reticle_y{};
	glm::vec3 weapon_lead_world{0.0f};
	float weapon_lead_x{-1.0f};
	float weapon_lead_y{-1.0f};
	float reticle_return_amount{};
	float hit_distortion{};
	float camera_disturbance{};
	float whiteout_red{};
	float whiteout_exhaust{};
	bool pointer_inside{};
	bool reticle_initialized{};
	bool reticle_captured{};
	bool weapon_lead_valid{};
	bool powerball_window_held{};
	bool shield_balance_held{};
	bool sensor_transition{};
	bool smart_target{};
	bool ordnance_initialized{};
	bool scanner_tone_continuous{};
	bool hud_icon_draw[6]{};
	bool status_icon_draw[9]{};
	bool player_schematic_hits[4]{};
	bool target_schematic_hits[4]{};
	bool comms_static_active{};
	bool deathmatch_mission{};
	bool threat_warning_tone{};
	bool hit_sound_pending{};
	bool scoreboard_held{};
	bool chat_active{};
};

void runtime_reset(
	Runtime& runtime,
	std::uint32_t drawable_width,
	std::uint32_t drawable_height,
	std::uint32_t random_seed);
void runtime_update_power(
	game::WorldObject& player,
	float x,
	float y);
void runtime_set_pointer(
	Runtime& runtime,
	float x,
	float y,
	bool inside);
void runtime_consume_player_hit_triggers(
	Runtime& runtime,
	game::World& world,
	std::uint32_t simulation_tick);
void runtime_update(
	Runtime& runtime,
	game::World& world,
	mission::Runtime& mission,
	const assets::MissileStatsTable& missile_stats,
	bool local_missile_has_target,
	std::uint32_t simulation_steps,
	std::uint32_t simulation_tick,
	std::uint32_t drawable_width,
	std::uint32_t drawable_height);
bool runtime_open_panel(Runtime& runtime, std::uint8_t panel);
void runtime_close_panel(Runtime& runtime, std::uint8_t panel);
void runtime_prepare_targeting_input(
	Runtime& runtime,
	game::World& world);
void runtime_apply_targeting_input(
	Runtime& runtime,
	game::World& world,
	const game::CameraRuntime& camera,
	std::uint32_t drawable_width,
	std::uint32_t drawable_height,
	const input::GameplayInput& input);
void runtime_apply_equipment_action(
	Runtime& runtime,
	game::World& world,
	mission::Runtime& mission,
	game::WorldObject& player,
	std::uint8_t action,
	std::uint32_t simulation_tick);
void runtime_apply_player_action(
	Runtime& runtime,
	game::World& world,
	mission::Runtime& mission,
	std::uint8_t action,
	bool active,
	std::uint32_t simulation_tick);
void runtime_set_scoreboard_held(Runtime& runtime, bool held);
void runtime_enqueue_message(
	Runtime& runtime,
	const char* text,
	std::uint32_t mission_time);
void runtime_begin_chat(
	Runtime& runtime,
	std::int16_t destination = -1);
bool runtime_append_chat(
	Runtime& runtime,
	const render::FrontendRenderer& renderer,
	const char* text);
void runtime_backspace_chat(Runtime& runtime);
bool runtime_submit_chat(
	Runtime& runtime,
	std::int16_t& destination,
	char (&text)[kChatMessageBytes]);
void runtime_enqueue_ui_sound(Runtime& runtime, std::uint8_t event);
void runtime_enqueue_sample(Runtime& runtime, std::uint8_t sample);
bool runtime_pop_sound(
	Runtime& runtime,
	std::uint8_t& sample,
	std::uint8_t& volume);
bool runtime_pop_hit_sound(Runtime& runtime);
}

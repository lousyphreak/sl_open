#pragma once

#include "game/session.hpp"
#include "mission/network_runtime.hpp"

#include <cstddef>
#include <cstdint>

namespace sl_open::network
{
// DirectPlay supplied StarLancer's original framing. This adapter deliberately
// uses a small, versioned protocol of its own while preserving the decoded
// gameplay opcodes and the retail guaranteed/conditional delivery classes.
// It is a native IPv4 transport; browser builds report unsupported instead of
// presenting multiplayer sessions which cannot be joined.
constexpr std::uint16_t kMultiplayerProtocolVersion = 5;
constexpr std::uint16_t kMultiplayerDiscoveryPort = 28900;
constexpr std::uint16_t kMultiplayerDefaultSessionPort = 28901;
constexpr std::uint8_t kMultiplayerTransportPlayerCapacity = 8;
constexpr std::size_t kMultiplayerSessionNameBytes = 64;
constexpr std::size_t kMultiplayerRetailSessionNameCharacters = 19;
constexpr std::size_t kMultiplayerIpv4TextBytes = 16;
constexpr std::size_t kMultiplayerDiscoveryCapacity = 32;
constexpr std::size_t kMultiplayerEventCapacity = 96;
constexpr std::size_t kMultiplayerGameplayQueueCapacity = 128;
constexpr std::size_t kMultiplayerLobbyChatCapacity = 32;
constexpr std::size_t kMultiplayerPostMissionChatCapacity = 32;

struct MultiplayerSessionId
{
	std::uint8_t bytes[16]{};
};

constexpr bool operator==(
	const MultiplayerSessionId& left,
	const MultiplayerSessionId& right)
{
	for (std::size_t index = 0; index < sizeof(left.bytes); ++index)
	{
		if (left.bytes[index] != right.bytes[index])
		{
			return false;
		}
	}
	return true;
}

constexpr bool operator!=(
	const MultiplayerSessionId& left,
	const MultiplayerSessionId& right)
{
	return !(left == right);
}

enum class MultiplayerTransportState : std::uint8_t
{
	uninitialized,
	unsupported,
	idle,
	discovering,
	hosting_lobby,
	connecting,
	client_lobby,
	host_prelaunch,
	client_prelaunch,
	gameplay,
	post_mission,
	disconnected,
	error,
};

enum class MultiplayerTransportError : std::uint8_t
{
	none,
	unsupported_platform,
	socket_runtime,
	socket_create,
	socket_option,
	address_invalid,
	bind_failed,
	listen_failed,
	connect_failed,
	connection_lost,
	connection_timeout,
	session_full,
	session_mismatch,
	protocol_mismatch,
	protocol_violation,
	malformed_packet,
	oversize_packet,
	queue_full,
	invalid_state,
	invalid_lobby,
	not_authority,
	udp_unavailable,
};

enum class MultiplayerSessionMode : std::uint8_t
{
	cooperative,
	deathmatch,
};

enum class MultiplayerDepartureReason : std::uint8_t
{
	leave,
	connection_lost,
	timeout,
	protocol_error,
	host_shutdown,
	kicked,
};

enum class MultiplayerPostMissionAction : std::uint8_t
{
	none,
	replay,
	continue_campaign,
};

enum class MultiplayerTransportEventKind : std::uint8_t
{
	none,
	discovery_updated,
	connected,
	lobby_updated,
	lobby_chat_available,
	peer_joined,
	peer_departed,
	prelaunch,
	launch,
	gameplay_available,
	player_mission_outcome,
	mission_result,
	post_mission_updated,
	post_mission_chat_available,
	post_mission_action,
	// DirectPlay's migrate-host session flag keeps the application in its
	// current screen while the transport authority changes. player is the
	// new authority's gameplay ordinal when a launch exists, otherwise its
	// stable lobby slot.
	authority_changed,
	disconnected,
	error,
};

struct MultiplayerLobbyPlayer
{
	std::int16_t loadout[game::kMissionLoadoutSlots]{};
	std::int16_t selected_ship{-1};
	std::int8_t team{-1};
	// FUN_004b6840 returns the retail five-tick-smoothed, one-way
	// connection delay in 100 Hz gameplay ticks. This is the value shown
	// by the pause UI and compared with the 100/150 bad-link thresholds.
	std::uint32_t latency{};
	// FUN_004b6790's unsmoothed one-way delay, saturated to the eight-bit
	// payload consumed by opcodes 0x38 and 0x48.
	std::uint8_t one_way_latency{};
	std::int32_t deathmatch_kills{};
	std::int32_t deathmatch_deaths{};
	char name[game::kMultiplayerPlayerNameBytes]{};
	bool connected{};
	bool ready{};
	bool post_mission_ready{};
	// Retail's debrief RESTART label is an outcome flag set when that
	// player's terminal result requires another attempt. It is not a
	// player-selected replay vote; only the READY bit is user-controlled.
	bool outcome_requires_restart{};
};

struct MultiplayerLobbyRules
{
	std::uint32_t authoritative_seed{};
	std::uint16_t mission{};
	std::int8_t configured_team_count{-1};
	MultiplayerSessionMode mode{MultiplayerSessionMode::cooperative};
	bool team_mode{};
	bool respawn_targetable{true};
	bool ai_turrets{};
};

constexpr std::size_t kMultiplayerCoopMissionNameBytes = 256;
constexpr std::size_t kMultiplayerCoopMissionTitleBytes = 200;

// Retail opcode 8 is a fixed-width cooperative selection record. The first
// field is the MISS/save identifier shown to the mission owner, followed by
// its presentation title and the one-byte mission number.
struct MultiplayerCoopMissionSelection
{
	char mission_name[kMultiplayerCoopMissionNameBytes]{};
	char presentation_title[kMultiplayerCoopMissionTitleBytes]{};
	std::uint8_t mission{};
	bool present{};
};

struct MultiplayerLobbySnapshot
{
	MultiplayerLobbyPlayer
		players[kMultiplayerTransportPlayerCapacity];
	MultiplayerSessionId session_id;
	MultiplayerLobbyRules rules;
	MultiplayerCoopMissionSelection coop_selection;
	char session_name[kMultiplayerSessionNameBytes]{};
	std::uint8_t player_count{};
	std::uint8_t leader_slot{};
};

struct MultiplayerDiscoverySession
{
	MultiplayerSessionId session_id;
	char session_name[kMultiplayerSessionNameBytes]{};
	char address[kMultiplayerIpv4TextBytes]{};
	std::uint32_t last_seen_at{};
	std::uint16_t port{};
	std::uint16_t mission{};
	std::uint8_t player_count{};
	std::uint8_t player_capacity{kMultiplayerTransportPlayerCapacity};
	MultiplayerSessionMode mode{MultiplayerSessionMode::cooperative};
	bool team_mode{};
	bool ai_turrets{};
};

enum class MultiplayerPlayerMissionOutcome : std::uint8_t
{
	none,
	survived,
	destroyed,
	captured,
	ejected,
	executed,
	departed,
	kicked,
	network_abort,
};

struct MultiplayerPlayerMissionReport
{
	game::SessionResult result;
	game::MultiplayerCampaignLaunchState campaign;
	campaign::AdvanceResult advance;
	MultiplayerPlayerMissionOutcome outcome{
		MultiplayerPlayerMissionOutcome::none};
	std::uint8_t lobby_slot{UINT8_MAX};
	bool valid{};
};

struct MultiplayerMissionResultSnapshot
{
	// Every DirectPlay participant owns its own campaign/profile result.
	// reports are replicated in stable gameplay order; no host report is
	// substituted for another participant's grade, score, retry state,
	// persistent variables, presentation, or profile progression.
	MultiplayerPlayerMissionReport
		reports[kMultiplayerTransportPlayerCapacity];
	// Compatibility projection for callers being migrated from the former
	// aggregate API. These fields always mirror this process's LOCAL report
	// and are never serialized as session authority.
	game::SessionResult authoritative;
	game::MultiplayerCampaignLaunchState campaign;
	campaign::AdvanceResult advance;
	MultiplayerPlayerMissionOutcome
		players[kMultiplayerTransportPlayerCapacity]{};
	std::int32_t deathmatch_kills[
		kMultiplayerTransportPlayerCapacity]{};
	std::int32_t deathmatch_deaths[
		kMultiplayerTransportPlayerCapacity]{};
	std::uint8_t debrief_leader_player{UINT8_MAX};
	bool authoritative_valid{};
	bool network_aborted{};
	bool debrief_leader_valid{};
	bool deathmatch_scores_valid{};
};

struct MultiplayerDiscoveryView
{
	const MultiplayerDiscoverySession* sessions{};
	std::uint32_t count{};
	std::uint32_t generation{};
};

struct MultiplayerDebriefParticipant
{
	MultiplayerLobbyPlayer lobby_player;
	MultiplayerPlayerMissionOutcome outcome{
		MultiplayerPlayerMissionOutcome::none};
	std::uint8_t gameplay_player{UINT8_MAX};
	std::uint8_t lobby_slot{UINT8_MAX};
};

// A compact, connected-only view in gameplay-player order. leader_participant
// indexes this compact array (not the sparse lobby), while leader_valid keeps
// retail's distinction between a Continue-capable leader and the fallback
// all-RESTART leader who can only choose Replay.
struct MultiplayerDebriefParticipantView
{
	MultiplayerDebriefParticipant
		participants[kMultiplayerTransportPlayerCapacity];
	std::uint8_t count{};
	std::uint8_t leader_participant{UINT8_MAX};
	bool leader_valid{};
};

struct MultiplayerGameplayPlayerLatency
{
	std::uint32_t smoothed_one_way_ticks{};
	std::uint8_t raw_one_way_ticks{};
};

struct MultiplayerTransportEvent
{
	MultiplayerTransportEventKind kind{
		MultiplayerTransportEventKind::none};
	MultiplayerTransportError error{MultiplayerTransportError::none};
	MultiplayerDepartureReason departure_reason{
		MultiplayerDepartureReason::leave};
	MultiplayerPostMissionAction post_mission_action{
		MultiplayerPostMissionAction::none};
	MultiplayerPlayerMissionOutcome mission_outcome{
		MultiplayerPlayerMissionOutcome::none};
	std::uint32_t authority_epoch{};
	std::uint8_t player{UINT8_MAX};
};

struct MultiplayerPostMissionChat
{
	std::uint8_t source_player{UINT8_MAX};
	char text[mission::kNetworkChatBytes]{};
};

struct MultiplayerLobbyChat
{
	// Lobby slots remain stable until that participant departs.
	std::uint8_t source_player{UINT8_MAX};
	char text[mission::kNetworkChatBytes]{};
};

namespace detail
{
constexpr std::uintptr_t kInvalidSocketHandle = UINTPTR_MAX;
constexpr std::size_t kTcpReceiveBytes = 8192;
constexpr std::size_t kTcpSendBytes = 16384;
constexpr std::size_t kWirePacketBytes = 2048;
constexpr std::size_t kDatagramQueueCapacity = 128;

struct MultiplayerTransportPeer
{
	std::uint8_t receive[kTcpReceiveBytes]{};
	std::uint8_t send[kTcpSendBytes]{};
	std::uintptr_t socket{kInvalidSocketHandle};
	std::uint64_t authentication_token{};
	std::uint64_t identity_token{};
	std::uint64_t last_receive_at{};
	std::uint64_t last_send_at{};
	std::uint64_t ping_sent_at{};
	std::uint64_t last_ping_at{};
	std::uint32_t ping_nonce{};
	std::uint32_t raw_one_way_latency_ticks{};
	std::uint32_t udp_send_sequence{};
	std::uint32_t udp_receive_sequence{};
	std::uint32_t address_be{};
	std::uint16_t listener_port_be{};
	std::uint16_t udp_port_be{};
	std::uint16_t receive_count{};
	std::uint16_t send_read{};
	std::uint16_t send_count{};
	std::uint8_t lobby_slot{UINT8_MAX};
	std::uint8_t gameplay_slot{UINT8_MAX};
	bool occupied{};
	bool connecting{};
	bool authenticated{};
	bool udp_endpoint_known{};
	bool udp_sequence_seen{};
	bool departure_published{};
	bool post_mission{};
	bool migration_connection{};
	MultiplayerDepartureReason requested_departure_reason{
		MultiplayerDepartureReason::connection_lost};
};

// Replicated DirectPlay-player identity and reachable endpoint. The
// connection authentication token is deliberately not stored here: it is
// replaced by the new authority during every migration, while identity_token
// remains stable for the lifetime of the session.
struct MultiplayerTransportMember
{
	std::uint64_t identity_token{};
	std::uint32_t address_be{};
	std::uint16_t listener_port_be{};
	std::uint16_t udp_port_be{};
	std::uint8_t gameplay_slot{UINT8_MAX};
	bool connected{};
	bool post_mission{};
};

struct MultiplayerQueuedDatagram
{
	std::uint8_t bytes[kWirePacketBytes]{};
	std::uint32_t address_be{};
	std::uint16_t port_be{};
	std::uint16_t length{};
};
}

// This object intentionally owns all storage used by the transport. Socket
// handles are represented as uintptr_t so the public header remains portable
// between WinSock and POSIX without leaking platform headers into App.
struct MultiplayerTransport
{
	detail::MultiplayerTransportPeer
		peers[kMultiplayerTransportPlayerCapacity];
	detail::MultiplayerTransportMember
		members[kMultiplayerTransportPlayerCapacity];
	detail::MultiplayerQueuedDatagram
		datagrams[detail::kDatagramQueueCapacity];
	MultiplayerTransportEvent events[kMultiplayerEventCapacity];
	mission::NetworkOutboundMessage
		gameplay[kMultiplayerGameplayQueueCapacity];
	MultiplayerLobbyChat
		lobby_chat[kMultiplayerLobbyChatCapacity];
	MultiplayerPostMissionChat
		post_mission_chat[kMultiplayerPostMissionChatCapacity];
	MultiplayerDiscoverySession
		discovered[kMultiplayerDiscoveryCapacity];
	MultiplayerLobbySnapshot lobby;
	game::MultiplayerLaunchSnapshot launch_snapshot;
	game::MultiplayerCampaignLaunchState prelaunch_campaign;
	// shared_bootstrap is the mutable host-migration/Continue image.
	// replay_bootstrap remains the immutable opcode-9 launch checkpoint.
	game::MultiplayerMissionBootstrap shared_bootstrap;
	game::MultiplayerMissionBootstrap replay_bootstrap;
	MultiplayerMissionResultSnapshot mission_result;
	MultiplayerLobbyPlayer pending_local_player;
	MultiplayerSessionId expected_session_id;
	std::uintptr_t listen_socket{detail::kInvalidSocketHandle};
	std::uintptr_t discovery_socket{detail::kInvalidSocketHandle};
	std::uintptr_t udp_socket{detail::kInvalidSocketHandle};
	std::uint64_t last_discovery_query_at{};
	std::uint64_t started_at{};
	std::uint64_t last_authority_snapshot_at{};
	std::uint64_t migration_started_at{};
	std::uint64_t next_latency_smoothing_at{};
	std::uint64_t authority_token{};
	std::uint64_t previous_authority_token{};
	std::uint64_t local_identity_token{};
	std::uint32_t discovery_nonce{};
	std::uint32_t discovery_generation{};
	std::uint32_t discovery_target_address_be{};
	std::uint32_t authority_epoch{};
	std::uint32_t previous_authority_epoch{};
	std::uint32_t launch_generation{};
	std::uint16_t session_port{};
	std::uint16_t local_udp_port{};
	std::uint16_t local_listener_port{};
	std::uint16_t discovery_target_port_be{};
	std::uint8_t event_read{};
	std::uint8_t event_count{};
	std::uint8_t gameplay_read{};
	std::uint8_t gameplay_count{};
	std::uint8_t lobby_chat_read{};
	std::uint8_t lobby_chat_count{};
	std::uint8_t post_mission_chat_read{};
	std::uint8_t post_mission_chat_count{};
	std::uint8_t datagram_read{};
	std::uint8_t datagram_count{};
	std::uint8_t local_lobby_slot{UINT8_MAX};
	std::uint8_t local_gameplay_player{UINT8_MAX};
	std::uint8_t authority_lobby_slot{UINT8_MAX};
	std::uint8_t migration_target_slot{UINT8_MAX};
	std::uint8_t migration_failed_mask{};
	std::uint8_t migration_pending_mask{};
	std::uint8_t gameplay_lobby_slot[
		kMultiplayerTransportPlayerCapacity]{};
	MultiplayerTransportState state{
		MultiplayerTransportState::uninitialized};
	MultiplayerTransportError last_error{
		MultiplayerTransportError::none};
	bool socket_runtime_initialized{};
	bool host{};
	bool discovery_active{};
	bool discovery_targeted{};
	bool launch_snapshot_valid{};
	bool migration_in_progress{};
	bool authority_snapshot_dirty{};
	bool authority_prelaunch_pending{};
	bool shared_bootstrap_pending{};
	bool initialized{};
};

bool multiplayer_transport_supported();
void multiplayer_transport_initialize(MultiplayerTransport& transport);
void multiplayer_transport_shutdown(
	MultiplayerTransport& transport,
	MultiplayerDepartureReason reason =
		MultiplayerDepartureReason::host_shutdown);

bool multiplayer_transport_begin_discovery(
	MultiplayerTransport& transport,
	std::uint64_t now);
// Direct-IP is still enumeration: this sends repeated discovery probes only
// to the supplied host and populates the normal discovery view. It never
// opens a session connection; the caller explicitly joins a selected offer.
bool multiplayer_transport_begin_ipv4_discovery(
	MultiplayerTransport& transport,
	const char* address,
	std::uint16_t discovery_port,
	std::uint64_t now);
void multiplayer_transport_end_discovery(
	MultiplayerTransport& transport);
MultiplayerDiscoveryView multiplayer_transport_discovery_view(
	const MultiplayerTransport& transport);

bool multiplayer_transport_host(
	MultiplayerTransport& transport,
	const char* session_name,
	const MultiplayerLobbyPlayer& local_player,
	const MultiplayerLobbyRules& rules,
	std::uint16_t port,
	std::uint64_t now);
bool multiplayer_transport_join(
	MultiplayerTransport& transport,
	const MultiplayerDiscoverySession& session,
	const MultiplayerLobbyPlayer& local_player,
	std::uint64_t now);
bool multiplayer_transport_join_ipv4(
	MultiplayerTransport& transport,
	const char* address,
	std::uint16_t port,
	const MultiplayerSessionId* expected_session,
	const MultiplayerLobbyPlayer& local_player,
	std::uint64_t now);

// Poll never blocks. Call it from the App pump during lobbies, gameplay, and
// debriefs; mission teardown deliberately does not own this lifetime.
void multiplayer_transport_poll(
	MultiplayerTransport& transport,
	std::uint64_t now);

bool multiplayer_transport_set_local_player(
	MultiplayerTransport& transport,
	const MultiplayerLobbyPlayer& player);
bool multiplayer_transport_set_rules(
	MultiplayerTransport& transport,
	const MultiplayerLobbyRules& rules);
bool multiplayer_transport_set_coop_mission_selection(
	MultiplayerTransport& transport,
	const char* mission_name,
	const char* presentation_title,
	std::uint8_t mission);
// Installs only this process's personal campaign/profile. Seed it before
// host()/join() (or before the first poll after join); those operations retain
// it so a welcome and prelaunch received in one poll cannot outrun App.
// Shared prelaunch, launch, migration, and replay packets carry narrow mission
// control plus the opcode-9 image and never serialize another player's profile
// into this slot.
bool multiplayer_transport_set_personal_campaign(
	MultiplayerTransport& transport,
	const game::MultiplayerCampaignLaunchState& campaign);
bool multiplayer_transport_set_session_name(
	MultiplayerTransport& transport,
	const char* session_name);
bool multiplayer_transport_submit_lobby_chat(
	MultiplayerTransport& transport,
	const char* text);
bool multiplayer_transport_pop_lobby_chat(
	MultiplayerTransport& transport,
	MultiplayerLobbyChat& message);
bool multiplayer_transport_host_kick(
	MultiplayerTransport& transport,
	std::uint8_t lobby_slot);
// START locks the roster, publishes the shared mission-control state, and
// moves every participant to the retail loadout phase while preserving each
// participant's personal campaign image. Each participant must subsequently
// submit exactly one finalized loadout before the authority can launch.
bool multiplayer_transport_begin_prelaunch(
	MultiplayerTransport& transport,
	const game::MultiplayerCampaignLaunchState& campaign);
bool multiplayer_transport_submit_prelaunch_loadout(
	MultiplayerTransport& transport,
	const MultiplayerLobbyPlayer& player);
bool multiplayer_transport_start_game(
	MultiplayerTransport& transport);
bool multiplayer_transport_launch_snapshot(
	const MultiplayerTransport& transport,
	game::MultiplayerLaunchSnapshot& snapshot);
const game::MultiplayerCampaignLaunchState&
multiplayer_transport_prelaunch_campaign(
	const MultiplayerTransport& transport);
// Replaces the mutable in-flight opcode-9 state after MissionSession capture.
// The immutable replay checkpoint is deliberately unchanged.
bool multiplayer_transport_update_mission_bootstrap(
	MultiplayerTransport& transport,
	const game::MultiplayerMissionBootstrap& bootstrap);
const game::MultiplayerMissionBootstrap&
multiplayer_transport_mission_bootstrap(
	const MultiplayerTransport& transport);
const MultiplayerLobbySnapshot& multiplayer_transport_lobby(
	const MultiplayerTransport& transport);
std::uint8_t multiplayer_transport_lobby_slot_for_gameplay_player(
	const MultiplayerTransport& transport,
	std::uint8_t gameplay_player);
std::uint8_t multiplayer_transport_local_gameplay_player(
	const MultiplayerTransport& transport);
// Resolves through the retained gameplay-to-lobby mapping and rejects stale
// or disconnected slots. Both returned values use retail 100 Hz ticks, not
// milliseconds or round-trip time.
bool multiplayer_transport_gameplay_player_latency(
	const MultiplayerTransport& transport,
	std::uint8_t gameplay_player,
	MultiplayerGameplayPlayerLatency& latency);

bool multiplayer_transport_submit_gameplay(
	MultiplayerTransport& transport,
	const mission::NetworkOutboundMessage& message);
bool multiplayer_transport_pop_gameplay(
	MultiplayerTransport& transport,
	mission::NetworkOutboundMessage& message);
// A false submit result leaves guaranteed gameplay wholly unqueued. Callers
// retain that one mission-runtime message and retry it after poll drains the
// transport; conditional messages are accepted as droppable retail traffic.
bool multiplayer_transport_gameplay_writable(
	const MultiplayerTransport& transport,
	const mission::NetworkOutboundMessage& message);

bool multiplayer_transport_report_player_mission_outcome(
	MultiplayerTransport& transport,
	MultiplayerPlayerMissionOutcome outcome);
// Every cooperative participant publishes exactly its own full report. The
// authority only validates/relays it under that player's stable identity.
// outcome may be omitted after report_player_mission_outcome(), in which case
// the retained matching outcome is used atomically.
bool multiplayer_transport_publish_mission_result(
	MultiplayerTransport& transport,
	const game::SessionResult& result,
	const game::MultiplayerCampaignLaunchState& campaign,
	const campaign::AdvanceResult& advance,
	MultiplayerPlayerMissionOutcome outcome =
		MultiplayerPlayerMissionOutcome::none);
// Deathmatch has no cooperative debrief/READY phase. Publication preserves
// the gameplay-order scoreboard (including retained departed-player totals)
// and moves all surviving peers directly back to the lobby.
bool multiplayer_transport_publish_deathmatch_result(
	MultiplayerTransport& transport,
	const std::int32_t (&kills)[
		kMultiplayerTransportPlayerCapacity],
	const std::int32_t (&deaths)[
		kMultiplayerTransportPlayerCapacity],
	bool network_aborted = false);
const MultiplayerMissionResultSnapshot&
multiplayer_transport_mission_result(
	const MultiplayerTransport& transport);
const MultiplayerPlayerMissionReport*
multiplayer_transport_player_mission_report(
	const MultiplayerTransport& transport,
	std::uint8_t gameplay_player);
const MultiplayerPlayerMissionReport*
multiplayer_transport_local_mission_report(
	const MultiplayerTransport& transport);
MultiplayerDebriefParticipantView
multiplayer_transport_debrief_participants(
	const MultiplayerTransport& transport);

bool multiplayer_transport_enter_post_mission(
	MultiplayerTransport& transport);
bool multiplayer_transport_set_post_mission_ready(
	MultiplayerTransport& transport,
	bool ready);
bool multiplayer_transport_submit_post_mission_chat(
	MultiplayerTransport& transport,
	const char* text);
bool multiplayer_transport_pop_post_mission_chat(
	MultiplayerTransport& transport,
	MultiplayerPostMissionChat& message);
// The elected gameplay player submits Continue/Replay. The host validates the
// READY gate and broadcasts one canonical action; every peer returns to its
// lobby state and updates the selected mission before the current authority
// calls begin_prelaunch() with continued or checkpoint-restored control state.
bool multiplayer_transport_post_mission_action(
	MultiplayerTransport& transport,
	MultiplayerPostMissionAction action);
bool multiplayer_transport_leave(MultiplayerTransport& transport);

bool multiplayer_transport_pop_event(
	MultiplayerTransport& transport,
	MultiplayerTransportEvent& event);
bool multiplayer_transport_udp_ready(
	const MultiplayerTransport& transport);
bool multiplayer_transport_is_host(
	const MultiplayerTransport& transport);
MultiplayerTransportState multiplayer_transport_state(
	const MultiplayerTransport& transport);
MultiplayerTransportError multiplayer_transport_last_error(
	const MultiplayerTransport& transport);
}

#include "network/multiplayer_transport.hpp"

#include "game/runtime_limits.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <iterator>
#include <limits>
#include <random>
#include <utility>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#elif !defined(__EMSCRIPTEN__)
#include <arpa/inet.h>
#include <cerrno>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
#endif
namespace sl_open::network
{
namespace
{
[[maybe_unused]] constexpr std::uint32_t kProtocolMagic =
	0x534c4d50u; // "SLMP"
[[maybe_unused]] constexpr std::uint64_t
	kConnectionTimeoutMilliseconds = 15000;
[[maybe_unused]] constexpr std::uint64_t
	kPingIntervalMilliseconds = 1010;
[[maybe_unused]] constexpr std::uint64_t
	kLatencySmoothingIntervalMilliseconds = 50;
[[maybe_unused]] constexpr std::uint64_t
	kDiscoveryIntervalMilliseconds = 1000;
[[maybe_unused]] constexpr std::uint64_t
	kDiscoveryExpiryMilliseconds = 5000;
[[maybe_unused]] constexpr std::uint64_t
	kAuthoritySnapshotIntervalMilliseconds = 1000;
[[maybe_unused]] constexpr std::uint8_t kBroadcastPlayer =
	UINT8_MAX;

enum class PacketType : std::uint8_t
{
	discovery_query = 1,
	discovery_offer = 2,
	client_hello = 3,
	server_welcome = 4,
	lobby_snapshot = 5,
	lobby_mutation = 6,
	launch = 7,
	gameplay = 8,
	peer_departure = 9,
	post_mission_status = 10,
	post_mission_action = 11,
	ping = 12,
	pong = 13,
	disconnect = 14,
	udp_probe = 15,
	udp_probe_ack = 16,
	post_mission_chat = 17,
	lobby_chat = 18,
	prelaunch = 19,
	prelaunch_loadout = 20,
	player_mission_outcome = 21,
	mission_result = 22,
	authority_snapshot = 23,
	authority_runtime = 24,
	migration_hello = 25,
	migration_welcome = 26,
	authority_handoff = 27,
};

enum class MissionResultPayload : std::uint8_t
{
	player_report,
	deathmatch_scores,
};

struct Writer
{
	std::uint8_t* bytes{};
	std::size_t capacity{};
	std::size_t count{};
	bool valid{true};

	void raw(const void* source, std::size_t length)
	{
		if (!valid || source == nullptr || length > capacity - count)
		{
			valid = false;
			return;
		}
		std::memcpy(bytes + count, source, length);
		count += length;
	}

	void u8(std::uint8_t value)
	{
		raw(&value, sizeof(value));
	}

	void i8(std::int8_t value)
	{
		u8(static_cast<std::uint8_t>(value));
	}

	void u16(std::uint16_t value)
	{
		const std::uint8_t encoded[2]{
			static_cast<std::uint8_t>(value >> 8u),
			static_cast<std::uint8_t>(value),
		};
		raw(encoded, sizeof(encoded));
	}

	void i16(std::int16_t value)
	{
		u16(static_cast<std::uint16_t>(value));
	}

	void u32(std::uint32_t value)
	{
		const std::uint8_t encoded[4]{
			static_cast<std::uint8_t>(value >> 24u),
			static_cast<std::uint8_t>(value >> 16u),
			static_cast<std::uint8_t>(value >> 8u),
			static_cast<std::uint8_t>(value),
		};
		raw(encoded, sizeof(encoded));
	}

	void i32(std::int32_t value)
	{
		u32(std::bit_cast<std::uint32_t>(value));
	}

	void u64(std::uint64_t value)
	{
		u32(static_cast<std::uint32_t>(value >> 32u));
		u32(static_cast<std::uint32_t>(value));
	}

	void boolean(bool value)
	{
		u8(value ? 1u : 0u);
	}

	void floating(float value)
	{
		u32(std::bit_cast<std::uint32_t>(value));
	}

	template<std::size_t Capacity>
	void text(const char (&value)[Capacity])
	{
		std::size_t length = 0;
		while (length < Capacity && value[length] != '\0')
		{
			++length;
		}
		if (length == Capacity
			|| length > std::numeric_limits<std::uint8_t>::max())
		{
			valid = false;
			return;
		}
		u8(static_cast<std::uint8_t>(length));
		raw(value, length);
	}
};

struct Reader
{
	const std::uint8_t* bytes{};
	std::size_t count{};
	std::size_t position{};
	bool valid{true};

	void raw(void* destination, std::size_t length)
	{
		if (!valid
			|| destination == nullptr
			|| length > count - position)
		{
			valid = false;
			return;
		}
		std::memcpy(destination, bytes + position, length);
		position += length;
	}

	std::uint8_t u8()
	{
		std::uint8_t value{};
		raw(&value, sizeof(value));
		return value;
	}

	std::int8_t i8()
	{
		return static_cast<std::int8_t>(u8());
	}

	std::uint16_t u16()
	{
		const std::uint16_t high = u8();
		const std::uint16_t low = u8();
		return static_cast<std::uint16_t>((high << 8u) | low);
	}

	std::int16_t i16()
	{
		return static_cast<std::int16_t>(u16());
	}

	std::uint32_t u32()
	{
		const std::uint32_t first = u8();
		const std::uint32_t second = u8();
		const std::uint32_t third = u8();
		const std::uint32_t fourth = u8();
		return (first << 24u)
			| (second << 16u)
			| (third << 8u)
			| fourth;
	}

	std::int32_t i32()
	{
		return std::bit_cast<std::int32_t>(u32());
	}

	std::uint64_t u64()
	{
		const std::uint64_t high = u32();
		const std::uint64_t low = u32();
		return (high << 32u) | low;
	}

	bool boolean()
	{
		const std::uint8_t value = u8();
		if (value > 1)
		{
			valid = false;
		}
		return value != 0;
	}

	float floating()
	{
		return std::bit_cast<float>(u32());
	}

	template<std::size_t Capacity>
	void text(char (&destination)[Capacity])
	{
		const std::uint8_t length = u8();
		if (!valid || length >= Capacity || length > count - position)
		{
			valid = false;
			return;
		}
		if (length != 0)
		{
			raw(destination, length);
		}
		if (valid)
		{
			destination[length] = '\0';
		}
	}

	bool done() const
	{
		return valid && position == count;
	}
};

bool terminated(const char* text, std::size_t capacity)
{
	if (text == nullptr)
	{
		return false;
	}
	for (std::size_t index = 0; index < capacity; ++index)
	{
		if (text[index] == '\0')
		{
			return true;
		}
	}
	return false;
}

std::size_t bounded_text_length(
	const char* text,
	std::size_t capacity)
{
	if (!terminated(text, capacity))
	{
		return capacity;
	}
	std::size_t length = 0;
	while (text[length] != '\0')
	{
		++length;
	}
	return length;
}

template<std::size_t Capacity>
bool copy_text(char (&destination)[Capacity], const char* source)
{
	if (!terminated(source, Capacity))
	{
		return false;
	}
	std::size_t length = 0;
	while (source[length] != '\0')
	{
		++length;
	}
	std::memcpy(destination, source, length + 1);
	return true;
}

bool id_is_zero(const MultiplayerSessionId& id)
{
	for (const std::uint8_t byte : id.bytes)
	{
		if (byte != 0)
		{
			return false;
		}
	}
	return true;
}

bool valid_rules(const MultiplayerLobbyRules& rules)
{
	return rules.mission != 0
		&& (rules.mode == MultiplayerSessionMode::cooperative
			|| rules.mode == MultiplayerSessionMode::deathmatch)
		&& rules.configured_team_count >= -1
		&& rules.configured_team_count <= 4
		&& (!rules.team_mode
			|| rules.mode == MultiplayerSessionMode::deathmatch)
		&& (!rules.ai_turrets
		|| rules.mode == MultiplayerSessionMode::deathmatch);
}

bool valid_coop_selection(
	const MultiplayerCoopMissionSelection& selection,
	const MultiplayerLobbyRules& rules)
{
	if (!selection.present)
	{
		return selection.mission == 0
			&& selection.mission_name[0] == '\0'
			&& selection.presentation_title[0] == '\0';
	}
	return rules.mode == MultiplayerSessionMode::cooperative
		&& selection.mission == rules.mission
		&& terminated(
			selection.mission_name,
			sizeof(selection.mission_name))
		&& selection.mission_name[0] != '\0'
		&& terminated(
			selection.presentation_title,
			sizeof(selection.presentation_title))
		&& selection.presentation_title[0] != '\0';
}

bool valid_player(
	const MultiplayerLobbyPlayer& player,
	bool require_launch_values,
	bool team_mode)
{
	if (!terminated(player.name, sizeof(player.name))
		|| player.name[0] == '\0'
		|| player.selected_ship < -1
		|| player.selected_ship > 11
		|| player.team < -1
		|| player.team > 3
		|| (require_launch_values
			&& (player.selected_ship < 0
				|| (team_mode && player.team < 0))))
	{
		return false;
	}
	for (const std::int16_t definition : player.loadout)
	{
		if (definition < -1 || definition > 10)
		{
			return false;
		}
	}
	return true;
}

[[maybe_unused]] bool valid_lobby(
	const MultiplayerLobbySnapshot& lobby)
{
	if (!valid_rules(lobby.rules)
		|| !valid_coop_selection(
			lobby.coop_selection, lobby.rules)
		|| !terminated(lobby.session_name, sizeof(lobby.session_name))
		|| lobby.session_name[0] == '\0'
		|| lobby.player_count == 0
		|| lobby.player_count > kMultiplayerTransportPlayerCapacity
		|| lobby.leader_slot >= kMultiplayerTransportPlayerCapacity
		|| !lobby.players[lobby.leader_slot].connected)
	{
		return false;
	}
	std::uint8_t connected = 0;
	for (const MultiplayerLobbyPlayer& player : lobby.players)
	{
		if (player.connected)
		{
			++connected;
			if (!valid_player(player, false, lobby.rules.team_mode))
			{
				return false;
			}
		}
	}
	return connected == lobby.player_count;
}

void write_id(Writer& writer, const MultiplayerSessionId& id)
{
	writer.raw(id.bytes, sizeof(id.bytes));
}

void read_id(Reader& reader, MultiplayerSessionId& id)
{
	reader.raw(id.bytes, sizeof(id.bytes));
}

void write_player(Writer& writer, const MultiplayerLobbyPlayer& player)
{
	writer.boolean(player.connected);
	writer.boolean(player.ready);
	writer.boolean(player.post_mission_ready);
	writer.boolean(player.outcome_requires_restart);
	writer.i16(player.selected_ship);
	writer.i8(player.team);
	writer.u32(player.latency);
	writer.u8(player.one_way_latency);
	writer.i32(player.deathmatch_kills);
	writer.i32(player.deathmatch_deaths);
	writer.text(player.name);
	for (const std::int16_t definition : player.loadout)
	{
		writer.i16(definition);
	}
}

void read_player(Reader& reader, MultiplayerLobbyPlayer& player)
{
	player = {};
	player.connected = reader.boolean();
	player.ready = reader.boolean();
	player.post_mission_ready = reader.boolean();
	player.outcome_requires_restart = reader.boolean();
	player.selected_ship = reader.i16();
	player.team = reader.i8();
	player.latency = reader.u32();
	player.one_way_latency = reader.u8();
	player.deathmatch_kills = reader.i32();
	player.deathmatch_deaths = reader.i32();
	reader.text(player.name);
	for (std::int16_t& definition : player.loadout)
	{
		definition = reader.i16();
	}
}

void write_rules(Writer& writer, const MultiplayerLobbyRules& rules)
{
	writer.u32(rules.authoritative_seed);
	writer.u16(rules.mission);
	writer.i8(rules.configured_team_count);
	writer.u8(static_cast<std::uint8_t>(rules.mode));
	writer.boolean(rules.team_mode);
	writer.boolean(rules.respawn_targetable);
	writer.boolean(rules.ai_turrets);
}

void read_rules(Reader& reader, MultiplayerLobbyRules& rules)
{
	rules.authoritative_seed = reader.u32();
	rules.mission = reader.u16();
	rules.configured_team_count = reader.i8();
	rules.mode = static_cast<MultiplayerSessionMode>(reader.u8());
	rules.team_mode = reader.boolean();
	rules.respawn_targetable = reader.boolean();
	rules.ai_turrets = reader.boolean();
	if (rules.mode != MultiplayerSessionMode::cooperative
		&& rules.mode != MultiplayerSessionMode::deathmatch)
	{
		reader.valid = false;
	}
}

void write_lobby(Writer& writer, const MultiplayerLobbySnapshot& lobby)
{
	write_id(writer, lobby.session_id);
	writer.text(lobby.session_name);
	write_rules(writer, lobby.rules);
	writer.boolean(lobby.coop_selection.present);
	if (lobby.coop_selection.present)
	{
		writer.raw(
			lobby.coop_selection.mission_name,
			sizeof(lobby.coop_selection.mission_name));
		writer.raw(
			lobby.coop_selection.presentation_title,
			sizeof(lobby.coop_selection.presentation_title));
		writer.u8(lobby.coop_selection.mission);
	}
	writer.u8(lobby.player_count);
	writer.u8(lobby.leader_slot);
	for (const MultiplayerLobbyPlayer& player : lobby.players)
	{
		write_player(writer, player);
	}
}

bool read_lobby(Reader& reader, MultiplayerLobbySnapshot& lobby)
{
	lobby = {};
	read_id(reader, lobby.session_id);
	reader.text(lobby.session_name);
	read_rules(reader, lobby.rules);
	lobby.coop_selection.present = reader.boolean();
	if (lobby.coop_selection.present)
	{
		reader.raw(
			lobby.coop_selection.mission_name,
			sizeof(lobby.coop_selection.mission_name));
		reader.raw(
			lobby.coop_selection.presentation_title,
			sizeof(lobby.coop_selection.presentation_title));
		lobby.coop_selection.mission = reader.u8();
	}
	lobby.player_count = reader.u8();
	lobby.leader_slot = reader.u8();
	for (MultiplayerLobbyPlayer& player : lobby.players)
	{
		read_player(reader, player);
	}
	return reader.valid && valid_lobby(lobby);
}

bool valid_campaign_state(
	const campaign::CampaignState& campaign,
	bool allow_campaign_complete = false)
{
	if (!terminated(campaign.id, sizeof(campaign.id))
		|| campaign.id[0] == '\0'
		|| !terminated(campaign.callsign, sizeof(campaign.callsign))
		|| campaign.callsign[0] == '\0'
		|| campaign.difficulty > campaign::Difficulty::hard
		|| campaign.pilot > campaign::Pilot::male
		|| campaign.mission == 0
		|| campaign.mission
			> campaign::kMissionCount
				+ (allow_campaign_complete ? 1u : 0u)
		|| campaign.rank > 8
		|| campaign.progression > 8
		|| campaign.selected_ship < -1
		|| campaign.selected_ship > 11)
	{
		return false;
	}
	for (const campaign::MissionGrade grade :
		campaign.mission_results)
	{
		if (grade < campaign::MissionGrade::none
			|| grade > campaign::MissionGrade::perfect)
		{
			return false;
		}
	}
	for (const std::int16_t definition : campaign.loadout)
	{
		if (definition < -1 || definition > 10)
		{
			return false;
		}
	}
	for (const std::uint8_t rank : campaign.mission_best_ranks)
	{
		if (rank > 8)
		{
			return false;
		}
	}
	return true;
}

bool valid_campaign_launch(
	const game::MultiplayerCampaignLaunchState& launch,
	const MultiplayerLobbyRules& rules)
{
	if (rules.mode == MultiplayerSessionMode::deathmatch)
	{
		return !launch.present
			&& !launch.mission_25_alternate;
	}
	return launch.present
		&& valid_campaign_state(launch.state)
		&& launch.state.mission == rules.mission
		&& (!launch.mission_25_alternate
			|| rules.mission == 25);
}

void write_campaign_launch(
	Writer& writer,
	const game::MultiplayerCampaignLaunchState& launch)
{
	writer.boolean(launch.present);
	writer.boolean(launch.mission_25_alternate);
	if (!launch.present)
	{
		return;
	}
	const campaign::CampaignState& campaign = launch.state;
	writer.text(campaign.id);
	writer.text(campaign.callsign);
	writer.u8(static_cast<std::uint8_t>(campaign.difficulty));
	writer.u8(static_cast<std::uint8_t>(campaign.pilot));
	writer.u16(campaign.mission);
	writer.i32(campaign.score);
	writer.u8(campaign.rank);
	writer.u8(campaign.progression);
	for (const bool value : campaign.medals)
	{
		writer.boolean(value);
	}
	for (const bool value : campaign.bars)
	{
		writer.boolean(value);
	}
	for (const campaign::MissionGrade value :
		campaign.mission_results)
	{
		writer.i16(static_cast<std::int16_t>(value));
	}
	for (const std::uint16_t value :
		campaign.mission_score_events)
	{
		writer.u16(value);
	}
	for (const std::uint16_t value : campaign.retry_history)
	{
		writer.u16(value);
	}
	writer.u16(campaign.retry_count);
	writer.i16(campaign.selected_ship);
	for (const std::int16_t value : campaign.loadout)
	{
		writer.i16(value);
	}
	for (const std::uint8_t value :
		campaign.mission_best_ranks)
	{
		writer.u8(value);
	}
	for (const std::int32_t value :
		campaign.branch_variables)
	{
		writer.i32(value);
	}
	writer.u32(campaign.leaderboard_seed);
}

void read_campaign_launch(
	Reader& reader,
	game::MultiplayerCampaignLaunchState& launch)
{
	launch = {};
	launch.present = reader.boolean();
	launch.mission_25_alternate = reader.boolean();
	if (!launch.present)
	{
		return;
	}
	campaign::CampaignState& campaign = launch.state;
	reader.text(campaign.id);
	reader.text(campaign.callsign);
	campaign.difficulty =
		static_cast<campaign::Difficulty>(reader.u8());
	campaign.pilot =
		static_cast<campaign::Pilot>(reader.u8());
	campaign.mission = reader.u16();
	campaign.score = reader.i32();
	campaign.rank = reader.u8();
	campaign.progression = reader.u8();
	for (bool& value : campaign.medals)
	{
		value = reader.boolean();
	}
	for (bool& value : campaign.bars)
	{
		value = reader.boolean();
	}
	for (campaign::MissionGrade& value :
		campaign.mission_results)
	{
		value = static_cast<campaign::MissionGrade>(reader.i16());
	}
	for (std::uint16_t& value :
		campaign.mission_score_events)
	{
		value = reader.u16();
	}
	for (std::uint16_t& value : campaign.retry_history)
	{
		value = reader.u16();
	}
	campaign.retry_count = reader.u16();
	campaign.selected_ship = reader.i16();
	for (std::int16_t& value : campaign.loadout)
	{
		value = reader.i16();
	}
	for (std::uint8_t& value :
		campaign.mission_best_ranks)
	{
		value = reader.u8();
	}
	for (std::int32_t& value : campaign.branch_variables)
	{
		value = reader.i32();
	}
	campaign.leaderboard_seed = reader.u32();
}

bool campaign_launch_equal(
	const game::MultiplayerCampaignLaunchState& left,
	const game::MultiplayerCampaignLaunchState& right)
{
	// Only the mission-control portion is shared. Retail keeps every
	// participant's profile identity, score/rank/awards, retry history,
	// mission grades/events, loadout and persistent campaign variables
	// local even though all peers enter the same mission and difficulty.
	return left.present == right.present
		&& left.mission_25_alternate
			== right.mission_25_alternate
		&& (!left.present
			|| (left.state.mission == right.state.mission
				&& left.state.difficulty
					== right.state.difficulty));
}

bool valid_campaign_control(
	const game::MultiplayerCampaignLaunchState& control,
	const MultiplayerLobbyRules& rules)
{
	if (rules.mode == MultiplayerSessionMode::deathmatch)
	{
		return !control.present
			&& !control.mission_25_alternate;
	}
	return control.present
		&& control.state.mission == rules.mission
		&& control.state.difficulty <= campaign::Difficulty::hard
		&& (!control.mission_25_alternate
			|| rules.mission == 25);
}

void write_campaign_control(
	Writer& writer,
	const game::MultiplayerCampaignLaunchState& launch)
{
	writer.boolean(launch.present);
	writer.boolean(launch.mission_25_alternate);
	if (launch.present)
	{
		writer.u8(static_cast<std::uint8_t>(
			launch.state.difficulty));
		writer.u16(launch.state.mission);
	}
}

void read_campaign_control(
	Reader& reader,
	game::MultiplayerCampaignLaunchState& control)
{
	control = {};
	control.present = reader.boolean();
	control.mission_25_alternate = reader.boolean();
	if (control.present)
	{
		control.state.difficulty =
			static_cast<campaign::Difficulty>(reader.u8());
		control.state.mission = reader.u16();
	}
}

bool install_campaign_control(
	const game::MultiplayerCampaignLaunchState& control,
	const MultiplayerLobbyRules& rules,
	const game::MultiplayerCampaignLaunchState& personal,
	game::MultiplayerCampaignLaunchState& installed)
{
	if (!valid_campaign_control(control, rules))
	{
		return false;
	}
	if (!control.present)
	{
		installed = {};
		return true;
	}
	if (!personal.present
		|| !valid_campaign_state(personal.state, true))
	{
		return false;
	}
	installed = personal;
	installed.present = true;
	installed.mission_25_alternate =
		control.mission_25_alternate;
	installed.state.mission = control.state.mission;
	installed.state.difficulty = control.state.difficulty;
	return true;
}

void write_mission_bootstrap(
	Writer& writer,
	const game::MultiplayerMissionBootstrap& bootstrap)
{
	writer.boolean(bootstrap.present);
	if (!bootstrap.present)
	{
		return;
	}
	writer.u32(bootstrap.launch_generation);
	writer.u16(bootstrap.mission);
	for (const std::uint32_t value :
		bootstrap.image.session_state)
	{
		writer.u32(value);
	}
	writer.i16(bootstrap.image.pilo.pilot_id);
	writer.u8(bootstrap.image.pilo.availability);
	writer.u8(bootstrap.image.pilo.reserved);
	for (const std::int16_t pilot :
		bootstrap.image.pilot_assignments)
	{
		writer.i16(pilot);
	}
}

void read_mission_bootstrap(
	Reader& reader,
	game::MultiplayerMissionBootstrap& bootstrap)
{
	bootstrap = {};
	bootstrap.present = reader.boolean();
	if (!bootstrap.present)
	{
		return;
	}
	bootstrap.launch_generation = reader.u32();
	bootstrap.mission = reader.u16();
	for (std::uint32_t& value :
		bootstrap.image.session_state)
	{
		value = reader.u32();
	}
	bootstrap.image.pilo.pilot_id = reader.i16();
	bootstrap.image.pilo.availability = reader.u8();
	bootstrap.image.pilo.reserved = reader.u8();
	for (std::int16_t& pilot :
		bootstrap.image.pilot_assignments)
	{
		pilot = reader.i16();
	}
}

bool mission_bootstrap_equal(
	const game::MultiplayerMissionBootstrap& left,
	const game::MultiplayerMissionBootstrap& right)
{
	return left.present == right.present
		&& (!left.present
			|| (left.launch_generation
					== right.launch_generation
				&& left.mission == right.mission
				&& left.image.pilo.pilot_id
					== right.image.pilo.pilot_id
				&& left.image.pilo.availability
					== right.image.pilo.availability
				&& left.image.pilo.reserved
					== right.image.pilo.reserved
				&& std::equal(
					std::begin(left.image.session_state),
					std::end(left.image.session_state),
					std::begin(right.image.session_state))
				&& std::equal(
					std::begin(left.image.pilot_assignments),
					std::end(left.image.pilot_assignments),
					std::begin(
						right.image.pilot_assignments))));
}

void write_launch(
	Writer& writer,
	const game::MultiplayerLaunchSnapshot& snapshot)
{
	write_campaign_control(writer, snapshot.campaign);
	write_mission_bootstrap(writer, snapshot.bootstrap);
	writer.u32(snapshot.authoritative_seed);
	writer.u16(snapshot.authoritative_mission);
	writer.u8(snapshot.player_count);
	writer.u8(snapshot.local_player);
	writer.u8(snapshot.gameplay_player_prefix);
	writer.i8(snapshot.configured_team_count);
	writer.u8(static_cast<std::uint8_t>(snapshot.role));
	writer.boolean(snapshot.deathmatch_mode);
	writer.boolean(snapshot.team_mode);
	writer.boolean(snapshot.respawn_targetable);
	writer.boolean(snapshot.ai_turrets);
	for (const game::MultiplayerPlayerLaunch& player : snapshot.players)
	{
		writer.boolean(player.connected);
		writer.i16(player.selected_ship);
		writer.i32(player.team);
		writer.u32(player.latency);
		writer.u8(player.one_way_latency);
		writer.text(player.name);
		for (const std::int16_t definition : player.loadout)
		{
			writer.i16(definition);
		}
	}
}

bool read_launch(
	Reader& reader,
	const game::MultiplayerCampaignLaunchState& personal_campaign,
	game::MultiplayerLaunchSnapshot& snapshot)
{
	snapshot = {};
	game::MultiplayerCampaignLaunchState campaign_control;
	read_campaign_control(reader, campaign_control);
	read_mission_bootstrap(reader, snapshot.bootstrap);
	snapshot.authoritative_seed = reader.u32();
	snapshot.authoritative_mission = reader.u16();
	snapshot.player_count = reader.u8();
	snapshot.local_player = reader.u8();
	snapshot.gameplay_player_prefix = reader.u8();
	snapshot.configured_team_count = reader.i8();
	snapshot.role =
		static_cast<game::MultiplayerRole>(reader.u8());
	snapshot.deathmatch_mode = reader.boolean();
	snapshot.team_mode = reader.boolean();
	snapshot.respawn_targetable = reader.boolean();
	snapshot.ai_turrets = reader.boolean();
	for (game::MultiplayerPlayerLaunch& player : snapshot.players)
	{
		player.connected = reader.boolean();
		player.selected_ship = reader.i16();
		player.team = reader.i32();
		player.latency = reader.u32();
		player.one_way_latency = reader.u8();
		reader.text(player.name);
		for (std::int16_t& definition : player.loadout)
		{
			definition = reader.i16();
		}
	}
	MultiplayerLobbyRules rules;
	rules.mission = snapshot.authoritative_mission;
	rules.mode = snapshot.deathmatch_mode
		? MultiplayerSessionMode::deathmatch
		: MultiplayerSessionMode::cooperative;
	rules.team_mode = snapshot.team_mode;
	rules.ai_turrets = snapshot.ai_turrets;
	if (!install_campaign_control(
			campaign_control,
			rules,
			personal_campaign,
			snapshot.campaign))
	{
		reader.valid = false;
	}
	return reader.valid
		&& game::valid_multiplayer_launch_snapshot(snapshot);
}

bool session_result_campaign_is_zero(
	const game::SessionResult& result)
{
	if (result.grade != -1
		|| result.score_delta != 0
		|| result.score_events != 0
		|| result.campaign_state_captured
		|| result.objectives_completed_before_ejection)
	{
		return false;
	}
	for (const std::int32_t value :
		result.persistent_variables)
	{
		if (value != 0)
		{
			return false;
		}
	}
	return true;
}

bool session_result_is_empty(
	const game::SessionResult& result)
{
	return result.kind == game::SessionResultKind::none
		&& result.mission == 0
		&& result.coordinator_result == 0
		&& session_result_campaign_is_zero(result);
}

bool valid_mission_result(
	const game::SessionResult& result,
	const game::MultiplayerLaunchSnapshot& launch)
{
	if (result.kind <= game::SessionResultKind::restart
		|| result.kind > game::SessionResultKind::application_exit
		|| result.mission != launch.authoritative_mission
		|| result.grade < -1
		|| result.grade > 4
		|| result.coordinator_result > 9)
	{
		return false;
	}
	return !launch.deathmatch_mode
		|| session_result_campaign_is_zero(result);
}

void write_mission_result(
	Writer& writer,
	const game::SessionResult& result)
{
	writer.u8(static_cast<std::uint8_t>(result.kind));
	writer.u16(result.mission);
	writer.i16(result.grade);
	writer.i32(result.score_delta);
	writer.u16(result.score_events);
	writer.u8(result.coordinator_result);
	writer.boolean(result.campaign_state_captured);
	writer.boolean(result.objectives_completed_before_ejection);
	for (const std::int32_t value :
		result.persistent_variables)
	{
		writer.i32(value);
	}
}

void read_mission_result(
	Reader& reader,
	game::SessionResult& result)
{
	result = {};
	result.kind =
		static_cast<game::SessionResultKind>(reader.u8());
	result.mission = reader.u16();
	result.grade = reader.i16();
	result.score_delta = reader.i32();
	result.score_events = reader.u16();
	result.coordinator_result = reader.u8();
	result.campaign_state_captured = reader.boolean();
	result.objectives_completed_before_ejection =
		reader.boolean();
	for (std::int32_t& value : result.persistent_variables)
	{
		value = reader.i32();
	}
}

bool valid_advance_result(
	const campaign::AdvanceResult& advance,
	std::uint16_t completed_mission)
{
	const bool no_advance = advance.completed_mission == 0;
	return (no_advance
			|| advance.completed_mission == completed_mission)
		&& advance.next_mission <= campaign::kMissionCount + 1
		&& advance.new_medal >= -1
		&& advance.new_medal
			< static_cast<std::int8_t>(campaign::kAwardCount)
		&& advance.new_bar >= -1
		&& advance.new_bar
			< static_cast<std::int8_t>(campaign::kAwardCount)
		&& advance.previous_rank >= -1
		&& advance.previous_rank <= 8
		&& advance.new_rank >= -1
		&& advance.new_rank <= 8
		&& (!no_advance
			|| (advance.next_mission == 0
				&& advance.new_medal == -1
				&& advance.new_bar == -1
				&& advance.previous_rank == -1
				&& advance.new_rank == -1
				&& !advance.campaign_complete))
		&& (no_advance
			|| advance.campaign_complete
				== (advance.next_mission
					== campaign::kMissionCount + 1));
}

void write_advance_result(
	Writer& writer,
	const campaign::AdvanceResult& advance)
{
	writer.u16(advance.completed_mission);
	writer.u16(advance.next_mission);
	writer.i8(advance.new_medal);
	writer.i8(advance.new_bar);
	writer.i8(advance.previous_rank);
	writer.i8(advance.new_rank);
	writer.boolean(advance.campaign_complete);
}

void read_advance_result(
	Reader& reader,
	campaign::AdvanceResult& advance)
{
	advance = {};
	advance.completed_mission = reader.u16();
	advance.next_mission = reader.u16();
	advance.new_medal = reader.i8();
	advance.new_bar = reader.i8();
	advance.previous_rank = reader.i8();
	advance.new_rank = reader.i8();
	advance.campaign_complete = reader.boolean();
}

bool valid_player_mission_outcome(
	MultiplayerPlayerMissionOutcome outcome)
{
	return outcome >= MultiplayerPlayerMissionOutcome::none
		&& outcome
			<= MultiplayerPlayerMissionOutcome::network_abort;
}

MultiplayerPlayerMissionOutcome outcome_for_coordinator(
	std::uint8_t coordinator)
{
	using Coordinator = campaign::MissionCoordinatorResult;
	switch (static_cast<Coordinator>(coordinator))
	{
	case Coordinator::ordinary:
	case Coordinator::mission_side:
		return MultiplayerPlayerMissionOutcome::survived;
	case Coordinator::destroyed:
		return MultiplayerPlayerMissionOutcome::destroyed;
	case Coordinator::retry:
		return MultiplayerPlayerMissionOutcome::ejected;
	case Coordinator::interrupted:
		return MultiplayerPlayerMissionOutcome::captured;
	case Coordinator::executed:
		return MultiplayerPlayerMissionOutcome::executed;
	case Coordinator::transfer:
	case Coordinator::cooperative_transfer:
		return MultiplayerPlayerMissionOutcome::kicked;
	case Coordinator::exit_session:
		return MultiplayerPlayerMissionOutcome::departed;
	case Coordinator::network_abort:
		return MultiplayerPlayerMissionOutcome::network_abort;
	}
	return MultiplayerPlayerMissionOutcome::none;
}

MultiplayerPlayerMissionOutcome outcome_for_mission_report(
	const game::SessionResult& result,
	const game::MultiplayerCampaignLaunchState& campaign_state,
	const campaign::AdvanceResult& advance)
{
	using Coordinator = campaign::MissionCoordinatorResult;
	const auto coordinator =
		static_cast<Coordinator>(result.coordinator_result);
	if (campaign_state.present
		&& campaign_state.mission_25_alternate
		&& result.mission == 25
		&& result.grade >= 0)
	{
		// FUN_00425240's valid first-leg report routes directly to 25A,
		// including the raw coordinator-5 path.
		return MultiplayerPlayerMissionOutcome::survived;
	}
	if (coordinator == Coordinator::retry)
	{
		// The first two retail ejections continue through ordinary
		// progression/debrief. Only the exhausted terminal retry remains
		// a RESTART/ejected outcome.
		return advance.completed_mission != 0
			? MultiplayerPlayerMissionOutcome::survived
			: MultiplayerPlayerMissionOutcome::ejected;
	}
	return outcome_for_coordinator(result.coordinator_result);
}

bool reportable_player_mission_outcome(
	MultiplayerPlayerMissionOutcome outcome)
{
	switch (outcome)
	{
	case MultiplayerPlayerMissionOutcome::survived:
	case MultiplayerPlayerMissionOutcome::destroyed:
	case MultiplayerPlayerMissionOutcome::captured:
	case MultiplayerPlayerMissionOutcome::ejected:
	case MultiplayerPlayerMissionOutcome::executed:
	case MultiplayerPlayerMissionOutcome::departed:
	case MultiplayerPlayerMissionOutcome::kicked:
	case MultiplayerPlayerMissionOutcome::network_abort:
		return true;
	case MultiplayerPlayerMissionOutcome::none:
		return false;
	}
	return false;
}

bool valid_player_mission_report(
	const MultiplayerPlayerMissionReport& report,
	const game::MultiplayerLaunchSnapshot& launch)
{
	if (!report.valid
		|| launch.deathmatch_mode
		|| !reportable_player_mission_outcome(report.outcome)
		|| report.outcome
			!= outcome_for_mission_report(
				report.result,
				report.campaign,
				report.advance)
		|| !valid_mission_result(report.result, launch)
		|| !report.campaign.present
		|| !valid_campaign_state(
			report.campaign.state, true)
		|| report.campaign.state.difficulty
			!= launch.campaign.state.difficulty
		|| !valid_advance_result(
			report.advance, launch.authoritative_mission))
	{
		return false;
	}
	const bool direct_alternate =
		report.campaign.mission_25_alternate
		&& launch.authoritative_mission == 25
		&& report.outcome
			== MultiplayerPlayerMissionOutcome::survived
		&& report.result.grade >= 0
		&& report.advance.completed_mission == 0
		&& report.campaign.state.mission == 25;
	if (report.campaign.mission_25_alternate)
	{
		return direct_alternate;
	}
	if (report.advance.completed_mission == 0)
	{
		return report.campaign.state.mission
			== launch.authoritative_mission;
	}
	return report.advance.completed_mission
			== launch.authoritative_mission
		&& report.campaign.state.mission
			== report.advance.next_mission;
}

void write_player_mission_report(
	Writer& writer,
	const MultiplayerPlayerMissionReport& report)
{
	writer.u8(static_cast<std::uint8_t>(report.outcome));
	write_mission_result(writer, report.result);
	write_campaign_launch(writer, report.campaign);
	write_advance_result(writer, report.advance);
}

bool read_player_mission_report(
	Reader& reader,
	const game::MultiplayerLaunchSnapshot& launch,
	MultiplayerPlayerMissionReport& report)
{
	report = {};
	report.outcome =
		static_cast<MultiplayerPlayerMissionOutcome>(
			reader.u8());
	read_mission_result(reader, report.result);
	read_campaign_launch(reader, report.campaign);
	read_advance_result(reader, report.advance);
	report.valid = reader.valid;
	return reader.valid
		&& valid_player_mission_report(report, launch);
}

bool has_direct_mission_25_report(
	const MultiplayerMissionResultSnapshot& result)
{
	for (const MultiplayerPlayerMissionReport& report :
		result.reports)
	{
		if (report.valid
			&& report.outcome
				== MultiplayerPlayerMissionOutcome::survived
			&& report.campaign.present
			&& report.campaign.mission_25_alternate)
		{
			return true;
		}
	}
	return false;
}

const MultiplayerPlayerMissionReport* mission_report_for_lobby_slot(
	const MultiplayerMissionResultSnapshot& result,
	std::uint8_t lobby_slot)
{
	for (const MultiplayerPlayerMissionReport& report :
		result.reports)
	{
		if (report.valid && report.lobby_slot == lobby_slot)
		{
			return &report;
		}
	}
	return nullptr;
}

bool outcome_requires_restart(
	MultiplayerPlayerMissionOutcome outcome)
{
	switch (outcome)
	{
	case MultiplayerPlayerMissionOutcome::destroyed:
	case MultiplayerPlayerMissionOutcome::captured:
	case MultiplayerPlayerMissionOutcome::ejected:
	case MultiplayerPlayerMissionOutcome::executed:
	case MultiplayerPlayerMissionOutcome::departed:
	case MultiplayerPlayerMissionOutcome::kicked:
	case MultiplayerPlayerMissionOutcome::network_abort:
		return true;
	case MultiplayerPlayerMissionOutcome::none:
	case MultiplayerPlayerMissionOutcome::survived:
		return false;
	}
	return false;
}

struct DebriefLeader
{
	std::uint8_t player{UINT8_MAX};
	bool valid{};
};

DebriefLeader compute_debrief_leader(
	const MultiplayerMissionResultSnapshot& result,
	const game::MultiplayerLaunchSnapshot& launch)
{
	DebriefLeader fallback;
	for (std::uint8_t player = 0;
		player < launch.player_count;
		++player)
	{
		if (!launch.players[player].connected)
		{
			continue;
		}
		if (result.players[player]
			== MultiplayerPlayerMissionOutcome::none)
		{
			continue;
		}
		if (fallback.player == UINT8_MAX)
		{
			fallback.player = player;
		}
		if (!outcome_requires_restart(result.players[player]))
		{
			return {player, true};
		}
	}
	return fallback;
}

bool valid_mission_result_snapshot(
	const MultiplayerMissionResultSnapshot& snapshot,
	const game::MultiplayerLaunchSnapshot& launch)
{
	if (!snapshot.authoritative_valid)
	{
		return false;
	}
	if (launch.deathmatch_mode)
	{
		if (!snapshot.deathmatch_scores_valid
			|| !session_result_is_empty(snapshot.authoritative)
			|| snapshot.campaign.present
			|| snapshot.campaign.mission_25_alternate
			|| snapshot.advance.completed_mission != 0
			|| !valid_advance_result(
				snapshot.advance,
				launch.authoritative_mission)
			|| snapshot.debrief_leader_player != UINT8_MAX
			|| snapshot.debrief_leader_valid)
		{
			return false;
		}
		for (std::size_t player = 0;
			player < kMultiplayerTransportPlayerCapacity;
			++player)
		{
			if (snapshot.players[player]
					!= MultiplayerPlayerMissionOutcome::none
				|| (player >= launch.player_count
					&& (snapshot.deathmatch_kills[player] != 0
						|| snapshot.deathmatch_deaths[player]
							!= 0)))
			{
				return false;
			}
		}
		return true;
	}
	if (snapshot.deathmatch_scores_valid
		|| !valid_mission_result(
			snapshot.authoritative, launch)
		|| !valid_advance_result(
			snapshot.advance, launch.authoritative_mission)
		|| (snapshot.authoritative.coordinator_result
				== static_cast<std::uint8_t>(
					campaign::MissionCoordinatorResult::
						network_abort)
			&& !snapshot.network_aborted))
	{
		return false;
	}
	for (std::size_t player = 0;
		player < kMultiplayerTransportPlayerCapacity;
		++player)
	{
		if (snapshot.deathmatch_kills[player] != 0
			|| snapshot.deathmatch_deaths[player] != 0)
		{
			return false;
		}
	}
	const bool direct_alternate =
		!launch.deathmatch_mode
		&& snapshot.campaign.present
		&& snapshot.campaign.mission_25_alternate
		&& snapshot.advance.completed_mission == 0;
	for (std::size_t player = 0;
		player < kMultiplayerTransportPlayerCapacity;
		++player)
	{
		const MultiplayerPlayerMissionOutcome outcome =
			snapshot.players[player];
		const bool valid_alternate_outcome =
			outcome == MultiplayerPlayerMissionOutcome::none
			|| outcome
				== MultiplayerPlayerMissionOutcome::survived
			|| (!launch.players[player].connected
				&& (outcome
						== MultiplayerPlayerMissionOutcome::
							departed
					|| outcome
						== MultiplayerPlayerMissionOutcome::
							kicked
					|| outcome
						== MultiplayerPlayerMissionOutcome::
							network_abort));
		if (!valid_player_mission_outcome(outcome)
			|| (player < launch.player_count
				&& ((direct_alternate
						&& !valid_alternate_outcome)
					|| (!direct_alternate
						&& outcome
							== MultiplayerPlayerMissionOutcome::
								none)))
			|| (player >= launch.player_count
				&& outcome
					!= MultiplayerPlayerMissionOutcome::none))
		{
			return false;
		}
	}
	if (direct_alternate)
	{
		if (snapshot.debrief_leader_player != UINT8_MAX
			|| snapshot.debrief_leader_valid)
		{
			return false;
		}
	}
	else
	{
		const DebriefLeader leader =
			compute_debrief_leader(snapshot, launch);
		if (snapshot.debrief_leader_player != leader.player
			|| snapshot.debrief_leader_valid != leader.valid)
		{
			return false;
		}
	}
	if (!snapshot.campaign.present
		|| !valid_campaign_state(
			snapshot.campaign.state, true)
		|| (snapshot.campaign.mission_25_alternate
			&& snapshot.campaign.state.mission != 25))
	{
		return false;
	}
	if (snapshot.advance.completed_mission == 0)
	{
		return snapshot.campaign.state.mission
			== launch.authoritative_mission;
	}
	return !snapshot.campaign.mission_25_alternate
		&& snapshot.advance.completed_mission
			== launch.authoritative_mission
		&& snapshot.campaign.state.mission
			== snapshot.advance.next_mission;
}

void write_mission_result_snapshot(
	Writer& writer,
	const MultiplayerMissionResultSnapshot& snapshot)
{
	write_mission_result(writer, snapshot.authoritative);
	write_campaign_launch(writer, snapshot.campaign);
	write_advance_result(writer, snapshot.advance);
	for (const MultiplayerPlayerMissionOutcome outcome :
		snapshot.players)
	{
		writer.u8(static_cast<std::uint8_t>(outcome));
	}
	for (const std::int32_t value :
		snapshot.deathmatch_kills)
	{
		writer.i32(value);
	}
	for (const std::int32_t value :
		snapshot.deathmatch_deaths)
	{
		writer.i32(value);
	}
	writer.u8(snapshot.debrief_leader_player);
	writer.boolean(snapshot.network_aborted);
	writer.boolean(snapshot.debrief_leader_valid);
	writer.boolean(snapshot.deathmatch_scores_valid);
}

bool read_mission_result_snapshot(
	Reader& reader,
	const game::MultiplayerLaunchSnapshot& launch,
	MultiplayerMissionResultSnapshot& snapshot)
{
	snapshot = {};
	read_mission_result(reader, snapshot.authoritative);
	read_campaign_launch(reader, snapshot.campaign);
	read_advance_result(reader, snapshot.advance);
	for (MultiplayerPlayerMissionOutcome& outcome :
		snapshot.players)
	{
		outcome = static_cast<MultiplayerPlayerMissionOutcome>(
			reader.u8());
	}
	for (std::int32_t& value : snapshot.deathmatch_kills)
	{
		value = reader.i32();
	}
	for (std::int32_t& value : snapshot.deathmatch_deaths)
	{
		value = reader.i32();
	}
	snapshot.debrief_leader_player = reader.u8();
	snapshot.network_aborted = reader.boolean();
	snapshot.debrief_leader_valid = reader.boolean();
	snapshot.deathmatch_scores_valid = reader.boolean();
	snapshot.authoritative_valid = true;
	return reader.valid
		&& valid_mission_result_snapshot(snapshot, launch);
}

bool finite_transform(
	const glm::vec3& position,
	const glm::mat3& orientation)
{
	for (std::size_t index = 0; index < 3; ++index)
	{
		if (!std::isfinite(position[index]))
		{
			return false;
		}
		for (std::size_t row = 0; row < 3; ++row)
		{
			if (!std::isfinite(orientation[index][row]))
			{
				return false;
			}
		}
	}
	return true;
}

bool valid_gameplay_opcode(mission::NetworkGameplayOpcode opcode)
{
	using Opcode = mission::NetworkGameplayOpcode;
	switch (opcode)
	{
	case Opcode::landing:
	case Opcode::object_state:
	case Opcode::pause_state:
	case Opcode::component_damage:
	case Opcode::component_state:
	case Opcode::bank_damage:
	case Opcode::bank_state:
	case Opcode::deathmatch_restart:
	case Opcode::deathmatch_state_request:
	case Opcode::deathmatch_state:
	case Opcode::deathmatch_respawn:
	case Opcode::tag_bomb_assignment:
	case Opcode::tag_bomb_detonate:
	case Opcode::dark_reign_tower_state:
	case Opcode::vampire_assignment:
	case Opcode::shadow_assignment:
	case Opcode::shadow_kill:
	case Opcode::nuclear_reset_player:
	case Opcode::dark_reign_drop:
	case Opcode::nuclear_success:
	case Opcode::session_script_ready:
	case Opcode::session_script_start:
	case Opcode::player_stats:
	case Opcode::deathmatch_pickup:
	case Opcode::deathmatch_powerup_activate:
	case Opcode::deathmatch_unhide_object:
	case Opcode::player_attack_request:
	case Opcode::player_backoff_request:
	case Opcode::player_help_request:
	case Opcode::cloak_state:
	case Opcode::player_ejected:
	case Opcode::script_sync_ready:
	case Opcode::script_sync_restart:
	case Opcode::friendly_fire:
	case Opcode::deathmatch_proximity_mine:
	case Opcode::player_departure:
	case Opcode::player_target_reference:
	case Opcode::deathmatch_reposition_object:
		return true;
	}
	return false;
}

bool valid_delivery(
	const mission::NetworkOutboundMessage& message)
{
	using Delivery = mission::NetworkDelivery;
	switch (message.delivery)
	{
	case Delivery::broadcast_conditional:
	case Delivery::broadcast_guaranteed:
		return message.destination_player == kBroadcastPlayer;
	case Delivery::directed_guaranteed:
	case Delivery::directed_conditional:
		return message.destination_player
			< kMultiplayerTransportPlayerCapacity;
	}
	return false;
}

bool conditional_delivery(mission::NetworkDelivery delivery)
{
	return delivery
			== mission::NetworkDelivery::broadcast_conditional
		|| delivery
			== mission::NetworkDelivery::directed_conditional;
}

bool valid_semantic_message(
	const mission::NetworkOutboundMessage& message)
{
	using Kind = mission::NetworkOutboundKind;
	using Opcode = mission::NetworkGameplayOpcode;
	if (message.source_player >= kMultiplayerTransportPlayerCapacity
		|| !valid_delivery(message))
	{
		return false;
	}
	switch (message.kind)
	{
	case Kind::chat:
		return !conditional_delivery(message.delivery)
			&& terminated(message.chat_text, sizeof(message.chat_text));
	case Kind::object_state:
	{
		if (message.opcode != Opcode::object_state
			|| message.object_index >= game::kMaxGameObjects
			|| message.object_state_sequence > 0x7fu
			|| message.object_state_position_mode > 3)
		{
			return false;
		}
		for (const std::int8_t value :
			message.object_state_demand)
		{
			if (value < -31 || value > 31)
			{
				return false;
			}
		}
		for (const std::int8_t value :
			message.object_state_angular)
		{
			if (value < -32 || value > 31)
			{
				return false;
			}
		}
		for (const std::int16_t value :
			message.object_state_velocity)
		{
			if (value < -255 || value > 255)
			{
				return false;
			}
		}
		const std::uint8_t position_width =
			message.object_state_position_mode == 1
				? 21u
				: message.object_state_position_mode == 2
					? 29u
					: message.object_state_position_mode == 3
						? 16u
						: 0u;
		for (const std::int32_t value :
			message.object_state_position)
		{
			if (position_width == 0)
			{
				if (value != 0)
				{
					return false;
				}
				continue;
			}
			const std::int32_t minimum =
				-(std::int32_t{1} << (position_width - 1u));
			const std::int32_t maximum =
				(std::int32_t{1} << (position_width - 1u)) - 1;
			if (value < minimum || value > maximum)
			{
				return false;
			}
		}
		for (const std::uint16_t value :
			message.object_state_orientation)
		{
			if (value > 0x03ffu)
			{
				return false;
			}
		}
		if ((!message.object_state_motion_present
				&& (message.object_state_demand[0] != 0
					|| message.object_state_demand[1] != 0
					|| message.object_state_demand[2] != 0
					|| message.object_state_demand[3] != 0
					|| message.object_state_angular[0] != 0
					|| message.object_state_angular[1] != 0
					|| message.object_state_angular[2] != 0
					|| message.object_state_velocity[0] != 0
					|| message.object_state_velocity[1] != 0
					|| message.object_state_velocity[2] != 0))
			|| (!message.object_state_orientation_present
				&& (message.object_state_orientation[0] != 0
					|| message.object_state_orientation[1] != 0
					|| message.object_state_orientation[2] != 0))
			|| (message.object_state_transform_sync
				&& (message.object_state_position_mode != 2
					|| !message.object_state_motion_present
					|| !message.object_state_orientation_present
					|| (message.delivery
							!= mission::NetworkDelivery::
								broadcast_guaranteed
						&& message.delivery
							!= mission::NetworkDelivery::
								directed_guaranteed))))
		{
			return false;
		}
		std::uint16_t bit_count = 29;
		if (message.object_state_motion_present)
		{
			bit_count =
				static_cast<std::uint16_t>(bit_count + 69u);
		}
		bit_count = static_cast<std::uint16_t>(
			bit_count
			+ (message.object_state_position_mode == 1
					? 63u
					: message.object_state_position_mode == 2
						? 87u
						: message.object_state_position_mode == 3
							? 48u
							: 0u));
		if (message.object_state_orientation_present)
		{
			bit_count =
				static_cast<std::uint16_t>(bit_count + 30u);
		}
		return message.retail_bit_count == bit_count;
	}
	case Kind::ai_sequence_sync:
		return message.delivery
				== mission::NetworkDelivery::broadcast_guaranteed
			&& message.object_index < game::kMaxGameObjects
			&& message.sync_index < 16;
	case Kind::ai_deferred_command:
		return message.delivery
				== mission::NetworkDelivery::broadcast_guaranteed
			&& message.object_index < game::kMaxGameObjects
			&& message.command_target_kind <= 4;
	case Kind::gameplay:
		break;
	default:
		return false;
	}
	if (!valid_gameplay_opcode(message.opcode)
		|| message.scenario_state_bits
			> sizeof(message.scenario_state) * 8u)
	{
		return false;
	}
	switch (message.opcode)
	{
	case Opcode::landing:
		return message.delivery
			== mission::NetworkDelivery::broadcast_guaranteed;
	case Opcode::component_damage:
	case Opcode::component_state:
		return message.object_index < game::kMaxGameObjects
			&& message.damage_source_index <= 0x01ffu;
	case Opcode::object_state:
		// Object-state packets are validated by the dedicated outbound-kind
		// branch above. They are never valid through the generic gameplay
		// representation.
		return false;
	case Opcode::bank_damage:
	case Opcode::bank_state:
		if (message.object_index >= game::kMaxGameObjects
			|| message.damage_source_index > 0x01ffu)
		{
			return false;
		}
		for (const std::uint8_t value : message.damage_bank_value)
		{
			if (value > 0x7fu)
			{
				return false;
			}
		}
		return true;
	case Opcode::pause_state:
		return message.delivery
				== mission::NetworkDelivery::broadcast_guaranteed
			&& static_cast<std::uint8_t>(
				message.pause_reason) <= 0x0fu;
	case Opcode::deathmatch_state:
		return message.scenario_state_bits != 0;
	case Opcode::deathmatch_state_request:
	case Opcode::shadow_kill:
	case Opcode::nuclear_reset_player:
	case Opcode::dark_reign_drop:
	case Opcode::nuclear_success:
		return message.scenario_player >= 0
			&& message.scenario_player < 8;
	case Opcode::deathmatch_respawn:
		return message.object_index < 8
			&& message.spawn_object_index < game::kMaxGameObjects;
	case Opcode::tag_bomb_assignment:
		return message.scenario_player >= -1
			&& message.scenario_player < 8
			&& message.scenario_secondary_player >= -1
			&& message.scenario_secondary_player < 8;
	case Opcode::dark_reign_tower_state:
		return message.tower_state < 8;
	case Opcode::vampire_assignment:
		return message.scenario_player >= 0
			&& message.scenario_player < 8;
	case Opcode::shadow_assignment:
		return message.scenario_player >= -1
			&& message.scenario_player < 8;
	case Opcode::session_script_ready:
		return message.sync_index < 8;
	case Opcode::script_sync_ready:
	case Opcode::script_sync_restart:
		return message.sync_index < 128;
	case Opcode::deathmatch_pickup:
		return message.scenario_player >= 0
			&& message.scenario_player < 8
			&& message.scenario_object < game::kMaxGameObjects;
	case Opcode::deathmatch_powerup_activate:
		return message.scenario_player >= 0
			&& message.scenario_player < 8
			&& message.scenario_powerup >= 0
			&& message.scenario_powerup < 10;
	case Opcode::deathmatch_unhide_object:
		return message.scenario_object < game::kMaxGameObjects;
	case Opcode::player_attack_request:
	case Opcode::player_backoff_request:
		return conditional_delivery(message.delivery)
			&& message.object_index <= 0x01ffu;
	case Opcode::player_help_request:
		return conditional_delivery(message.delivery)
			&& message.object_index == UINT16_MAX;
	case Opcode::cloak_state:
		return message.delivery
			== mission::NetworkDelivery::broadcast_guaranteed;
	case Opcode::player_ejected:
		return message.object_index < game::kMaxGameObjects;
	case Opcode::deathmatch_proximity_mine:
		return message.scenario_player >= 0
			&& message.scenario_player < 8
			&& message.scenario_object < game::kMaxGameObjects
			&& finite_transform(message.position, message.orientation);
	case Opcode::player_departure:
		return message.delivery
				== mission::NetworkDelivery::broadcast_guaranteed
			&& message.departure_player < 8;
	case Opcode::player_target_reference:
		// FUN_004bb980 writes the target object in nine bits and the
		// selected live-model component in seven. All-ones are the
		// canonical no-target (-1) sentinels retained by NetworkRuntime.
		return message.delivery
				== mission::NetworkDelivery::broadcast_guaranteed
			&& message.object_index <= 0x01ffu
			&& message.command_target_component >= -1
			&& message.command_target_component <= 0x7e;
	case Opcode::deathmatch_reposition_object:
		return message.scenario_object < game::kMaxGameObjects
			&& finite_transform(message.position, message.orientation);
	case Opcode::deathmatch_restart:
	case Opcode::tag_bomb_detonate:
	case Opcode::session_script_start:
	case Opcode::player_stats:
	case Opcode::friendly_fire:
		return true;
	}
	return false;
}

void write_semantic_message(
	Writer& writer,
	const mission::NetworkOutboundMessage& message)
{
	writer.u8(static_cast<std::uint8_t>(message.kind));
	writer.u8(static_cast<std::uint8_t>(message.opcode));
	writer.u8(static_cast<std::uint8_t>(message.delivery));
	writer.u8(message.source_player);
	writer.u8(message.destination_player);
	writer.u16(message.object_index);
	writer.u16(message.spawn_object_index);
	writer.i16(message.command_id);
	writer.i16(message.command_selector);
	writer.u16(message.command_target);
	writer.i16(message.command_target_component);
	writer.i16(message.command_sequence);
	for (const std::uint32_t state : message.command_state)
	{
		writer.u32(state);
	}
	writer.u8(message.sync_index);
	writer.u8(message.one_way_latency);
	writer.u8(message.command_target_kind);
	writer.u8(message.publication_seed);
	writer.u8(message.tower_state);
	writer.i8(message.scenario_player);
	writer.i8(message.scenario_secondary_player);
	writer.i8(message.scenario_powerup);
	writer.u16(message.scenario_object);
	writer.u8(message.departure_player);
	writer.u16(message.scenario_state_bits);
	const std::size_t scenario_bytes =
		(message.scenario_state_bits + 7u) / 8u;
	writer.raw(message.scenario_state, scenario_bytes);
	writer.boolean(message.scenario_flag);
	writer.u32(message.delay_ticks);
	writer.boolean(message.all_players);
	writer.u16(message.damage_source_index);
	writer.u16(message.damage_component_value);
	writer.u8(message.damage_component_id);
	writer.u8(message.damage_bank_mask);
	writer.raw(
		message.damage_bank_value,
		sizeof(message.damage_bank_value));
	writer.u8(message.deferred_priority);
	writer.i32(message.player_kills);
	writer.i32(message.player_deaths);
	writer.boolean(message.pause_active);
	writer.u8(static_cast<std::uint8_t>(
		message.pause_reason));
	writer.u8(message.object_state_sequence);
	writer.u8(message.object_state_position_mode);
	writer.boolean(message.object_state_motion_present);
	writer.boolean(message.object_state_orientation_present);
	writer.boolean(message.object_state_transform_sync);
	for (const std::int8_t value : message.object_state_demand)
	{
		writer.i8(value);
	}
	for (const std::int8_t value : message.object_state_angular)
	{
		writer.i8(value);
	}
	for (const std::int16_t value : message.object_state_velocity)
	{
		writer.i16(value);
	}
	for (const std::int32_t value : message.object_state_position)
	{
		writer.i32(value);
	}
	for (const std::uint16_t value :
		message.object_state_orientation)
	{
		writer.u16(value);
	}
	writer.u16(message.retail_bit_count);
	writer.text(message.chat_text);
	for (std::size_t index = 0; index < 3; ++index)
	{
		writer.floating(message.position[index]);
	}
	for (std::size_t column = 0; column < 3; ++column)
	{
		for (std::size_t row = 0; row < 3; ++row)
		{
			writer.floating(message.orientation[column][row]);
		}
	}
}

bool read_semantic_message(
	Reader& reader,
	mission::NetworkOutboundMessage& message)
{
	message = {};
	message.kind =
		static_cast<mission::NetworkOutboundKind>(reader.u8());
	message.opcode =
		static_cast<mission::NetworkGameplayOpcode>(reader.u8());
	message.delivery =
		static_cast<mission::NetworkDelivery>(reader.u8());
	message.source_player = reader.u8();
	message.destination_player = reader.u8();
	message.object_index = reader.u16();
	message.spawn_object_index = reader.u16();
	message.command_id = reader.i16();
	message.command_selector = reader.i16();
	message.command_target = reader.u16();
	message.command_target_component = reader.i16();
	message.command_sequence = reader.i16();
	for (std::uint32_t& state : message.command_state)
	{
		state = reader.u32();
	}
	message.sync_index = reader.u8();
	message.one_way_latency = reader.u8();
	message.command_target_kind = reader.u8();
	message.publication_seed = reader.u8();
	message.tower_state = reader.u8();
	message.scenario_player = reader.i8();
	message.scenario_secondary_player = reader.i8();
	message.scenario_powerup = reader.i8();
	message.scenario_object = reader.u16();
	message.departure_player = reader.u8();
	message.scenario_state_bits = reader.u16();
	if (message.scenario_state_bits
		> sizeof(message.scenario_state) * 8u)
	{
		reader.valid = false;
		return false;
	}
	const std::size_t scenario_bytes =
		(message.scenario_state_bits + 7u) / 8u;
	reader.raw(message.scenario_state, scenario_bytes);
	message.scenario_flag = reader.boolean();
	message.delay_ticks = reader.u32();
	message.all_players = reader.boolean();
	message.damage_source_index = reader.u16();
	message.damage_component_value = reader.u16();
	message.damage_component_id = reader.u8();
	message.damage_bank_mask = reader.u8();
	reader.raw(
		message.damage_bank_value,
		sizeof(message.damage_bank_value));
	message.deferred_priority = reader.u8();
	message.player_kills = reader.i32();
	message.player_deaths = reader.i32();
	message.pause_active = reader.boolean();
	message.pause_reason =
		static_cast<mission::NetworkPauseReason>(reader.u8());
	message.object_state_sequence = reader.u8();
	message.object_state_position_mode = reader.u8();
	message.object_state_motion_present = reader.boolean();
	message.object_state_orientation_present = reader.boolean();
	message.object_state_transform_sync = reader.boolean();
	for (std::int8_t& value : message.object_state_demand)
	{
		value = reader.i8();
	}
	for (std::int8_t& value : message.object_state_angular)
	{
		value = reader.i8();
	}
	for (std::int16_t& value : message.object_state_velocity)
	{
		value = reader.i16();
	}
	for (std::int32_t& value : message.object_state_position)
	{
		value = reader.i32();
	}
	for (std::uint16_t& value :
		message.object_state_orientation)
	{
		value = reader.u16();
	}
	message.retail_bit_count = reader.u16();
	reader.text(message.chat_text);
	for (std::size_t index = 0; index < 3; ++index)
	{
		message.position[index] = reader.floating();
	}
	for (std::size_t column = 0; column < 3; ++column)
	{
		for (std::size_t row = 0; row < 3; ++row)
		{
			message.orientation[column][row] =
				reader.floating();
		}
	}
	return reader.valid && valid_semantic_message(message);
}

void write_header(
	Writer& writer,
	PacketType type,
	const MultiplayerSessionId& session_id)
{
	writer.u32(kProtocolMagic);
	writer.u16(kMultiplayerProtocolVersion);
	writer.u8(static_cast<std::uint8_t>(type));
	writer.u8(0);
	write_id(writer, session_id);
}

bool read_header(
	Reader& reader,
	PacketType& type,
	MultiplayerSessionId& session_id)
{
	if (reader.u32() != kProtocolMagic
		|| reader.u16() != kMultiplayerProtocolVersion)
	{
		reader.valid = false;
		return false;
	}
	type = static_cast<PacketType>(reader.u8());
	const std::uint8_t flags = reader.u8();
	read_id(reader, session_id);
	if (flags != 0
		|| type < PacketType::discovery_query
		|| type > PacketType::authority_handoff)
	{
		reader.valid = false;
		return false;
	}
	return reader.valid;
}

std::uint8_t connected_player_count(
	const MultiplayerLobbySnapshot& lobby)
{
	std::uint8_t count = 0;
	for (const MultiplayerLobbyPlayer& player : lobby.players)
	{
		if (player.connected)
		{
			++count;
		}
	}
	return count;
}

bool materialize_mission_bootstrap(
	MultiplayerTransport& transport,
	const game::MultiplayerCampaignLaunchState& campaign_state,
	std::uint16_t mission,
	const game::MultiplayerMissionBootstrap* continuity,
	game::MultiplayerMissionBootstrap& bootstrap)
{
	if (transport.lobby.rules.mode
			!= MultiplayerSessionMode::cooperative
		|| !campaign_state.present
		|| !valid_campaign_state(campaign_state.state, true)
		|| mission == 0
		|| mission > campaign::kMissionCount
		|| transport.lobby.player_count == 0)
	{
		return false;
	}
	std::uint32_t generation = transport.launch_generation;
	if (transport.shared_bootstrap.present)
	{
		generation = std::max(
			generation,
			transport.shared_bootstrap.launch_generation);
	}
	if (transport.replay_bootstrap.present)
	{
		generation = std::max(
			generation,
			transport.replay_bootstrap.launch_generation);
	}
	if (transport.launch_snapshot_valid
		&& transport.launch_snapshot.bootstrap.present)
	{
		generation = std::max(
			generation,
			transport.launch_snapshot.bootstrap
				.launch_generation);
	}
	if (generation == UINT32_MAX)
	{
		return false;
	}

	bootstrap = {};
	if (continuity != nullptr && continuity->present)
	{
		bootstrap.image = continuity->image;
	}
	bootstrap.present = true;
	bootstrap.launch_generation = generation + 1;
	bootstrap.mission = mission;
	for (std::size_t index = 0;
		index < game::kMissionPersistentVariableCount;
		++index)
	{
		bootstrap.image.session_state[
			game::kMultiplayerCampaignVariableSlots[index]] =
				std::bit_cast<std::uint32_t>(
					campaign_state.state
						.branch_variables[index]);
	}
	bootstrap.image.session_state[15] =
		transport.lobby.player_count;
	if (!game::valid_multiplayer_mission_bootstrap(
			bootstrap,
			mission,
			transport.lobby.player_count,
			false))
	{
		bootstrap = {};
		return false;
	}
	return true;
}

bool reconcile_pending_bootstrap_roster(
	const MultiplayerTransport& transport,
	game::MultiplayerMissionBootstrap& bootstrap)
{
	if (game::valid_multiplayer_mission_bootstrap(
			bootstrap,
			transport.lobby.rules.mission,
			transport.lobby.player_count,
			false))
	{
		return true;
	}
	// Replay is byte-exact, including the slot-15 member count that was
	// broadcast at its checkpoint. A changed roster cannot silently turn it
	// into a different launch. Fresh/Continue images remain mutable until
	// opcode 9 is published and may adopt the finalized prelaunch roster.
	if (!bootstrap.present
		|| bootstrap.mission != transport.lobby.rules.mission
		|| mission_bootstrap_equal(
			bootstrap, transport.replay_bootstrap))
	{
		return false;
	}
	bootstrap.image.session_state[15] =
		transport.lobby.player_count;
	return game::valid_multiplayer_mission_bootstrap(
		bootstrap,
		transport.lobby.rules.mission,
		transport.lobby.player_count,
		false);
}

bool make_launch_snapshot(
	const MultiplayerTransport& transport,
	std::uint8_t recipient_lobby_slot,
	game::MultiplayerRole role,
	game::MultiplayerLaunchSnapshot& snapshot)
{
	snapshot = {};
	const MultiplayerLobbySnapshot& lobby = transport.lobby;
	if (!valid_lobby(lobby)
		|| !valid_campaign_launch(
			transport.prelaunch_campaign, lobby.rules)
		|| (lobby.rules.mode
				== MultiplayerSessionMode::cooperative
			&& (!transport.shared_bootstrap_pending
				|| !game::
					valid_multiplayer_mission_bootstrap(
						transport.shared_bootstrap,
						lobby.rules.mission,
						lobby.player_count,
						false)))
		|| recipient_lobby_slot
			>= kMultiplayerTransportPlayerCapacity
		|| !lobby.players[recipient_lobby_slot].connected)
	{
		return false;
	}
	snapshot.campaign = transport.prelaunch_campaign;
	snapshot.bootstrap =
		lobby.rules.mode == MultiplayerSessionMode::cooperative
			? transport.shared_bootstrap
			: game::MultiplayerMissionBootstrap{};
	snapshot.authoritative_seed =
		lobby.rules.authoritative_seed;
	snapshot.authoritative_mission = lobby.rules.mission;
	snapshot.player_count = lobby.player_count;
	snapshot.gameplay_player_prefix =
		lobby.rules.mode == MultiplayerSessionMode::deathmatch
			? kMultiplayerTransportPlayerCapacity
			: lobby.player_count;
	snapshot.configured_team_count =
		lobby.rules.configured_team_count;
	snapshot.role = role;
	snapshot.deathmatch_mode =
		lobby.rules.mode == MultiplayerSessionMode::deathmatch;
	snapshot.team_mode = lobby.rules.team_mode;
	snapshot.respawn_targetable =
		lobby.rules.respawn_targetable;
	snapshot.ai_turrets = lobby.rules.ai_turrets;

	std::uint8_t gameplay_slot = 0;
	for (std::uint8_t lobby_slot = 0;
		lobby_slot < kMultiplayerTransportPlayerCapacity;
		++lobby_slot)
	{
		const MultiplayerLobbyPlayer& source =
			lobby.players[lobby_slot];
		if (!source.connected)
		{
			continue;
		}
		if (!source.ready
			|| !valid_player(
				source, true, lobby.rules.team_mode))
		{
			return false;
		}
		game::MultiplayerPlayerLaunch& destination =
			snapshot.players[gameplay_slot];
		std::copy(
			std::begin(source.loadout),
			std::end(source.loadout),
			std::begin(destination.loadout));
		destination.selected_ship = source.selected_ship;
		destination.team = source.team;
		destination.latency = source.latency;
		destination.one_way_latency = source.one_way_latency;
		std::memcpy(
			destination.name,
			source.name,
			sizeof(destination.name));
		destination.connected = true;
		if (lobby_slot == recipient_lobby_slot)
		{
			snapshot.local_player = gameplay_slot;
		}
		++gameplay_slot;
	}
	return gameplay_slot == lobby.player_count
		&& game::valid_multiplayer_launch_snapshot(snapshot);
}

bool launch_matches_prelaunch(
	const MultiplayerTransport& transport,
	const game::MultiplayerLaunchSnapshot& launch)
{
	const MultiplayerLobbySnapshot& lobby = transport.lobby;
	const bool valid_launch_state =
		launch.deathmatch_mode
			? transport.state
				== MultiplayerTransportState::client_lobby
			: transport.state
				== MultiplayerTransportState::client_prelaunch;
	if (!valid_launch_state
		|| !valid_lobby(lobby)
		|| !campaign_launch_equal(
			launch.campaign, transport.prelaunch_campaign)
		|| launch.authoritative_seed
			!= lobby.rules.authoritative_seed
		|| launch.authoritative_mission != lobby.rules.mission
		|| launch.player_count != lobby.player_count
		|| launch.configured_team_count
			!= lobby.rules.configured_team_count
		|| launch.deathmatch_mode
			!= (lobby.rules.mode
				== MultiplayerSessionMode::deathmatch)
		|| launch.team_mode != lobby.rules.team_mode
		|| launch.respawn_targetable
			!= lobby.rules.respawn_targetable
		|| launch.ai_turrets != lobby.rules.ai_turrets)
	{
		return false;
	}
	std::uint8_t gameplay_player = 0;
	for (const MultiplayerLobbyPlayer& player : lobby.players)
	{
		if (!player.connected)
		{
			continue;
		}
		if (!player.ready
			|| gameplay_player >= launch.player_count)
		{
			return false;
		}
		const game::MultiplayerPlayerLaunch& published =
			launch.players[gameplay_player++];
		if (!published.connected
			|| published.selected_ship != player.selected_ship
			|| published.team != player.team
			|| published.latency != player.latency
			|| published.one_way_latency
				!= player.one_way_latency
			|| std::strncmp(
				published.name,
				player.name,
				sizeof(published.name)) != 0
			|| !std::equal(
				std::begin(published.loadout),
				std::end(published.loadout),
				std::begin(player.loadout)))
		{
			return false;
		}
	}
	return gameplay_player == launch.player_count;
}
bool event_is_coalescible(MultiplayerTransportEventKind kind)
{
	return kind == MultiplayerTransportEventKind::discovery_updated
		|| kind == MultiplayerTransportEventKind::lobby_updated
		|| kind
			== MultiplayerTransportEventKind::lobby_chat_available
		|| kind == MultiplayerTransportEventKind::gameplay_available
		|| kind
			== MultiplayerTransportEventKind::post_mission_updated
		|| kind
			== MultiplayerTransportEventKind::
				post_mission_chat_available;
}

bool queue_event(
	MultiplayerTransport& transport,
	const MultiplayerTransportEvent& event)
{
	if (event_is_coalescible(event.kind))
	{
		for (std::uint8_t ordinal = 0;
			ordinal < transport.event_count;
			++ordinal)
		{
			const std::uint8_t index =
				static_cast<std::uint8_t>(
					(transport.event_read + ordinal)
					% kMultiplayerEventCapacity);
			if (transport.events[index].kind == event.kind)
			{
				return true;
			}
		}
	}
	if (transport.event_count >= kMultiplayerEventCapacity)
	{
		transport.last_error = MultiplayerTransportError::queue_full;
		return false;
	}
	const std::uint8_t write = static_cast<std::uint8_t>(
		(transport.event_read + transport.event_count)
		% kMultiplayerEventCapacity);
	transport.events[write] = event;
	++transport.event_count;
	return true;
}

void queue_error(
	MultiplayerTransport& transport,
	MultiplayerTransportError error,
	bool terminal)
{
	transport.last_error = error;
	if (terminal)
	{
		transport.state = MultiplayerTransportState::error;
	}
	MultiplayerTransportEvent event;
	event.kind = MultiplayerTransportEventKind::error;
	event.error = error;
	(void)queue_event(transport, event);
}

[[maybe_unused]] bool queue_gameplay(
	MultiplayerTransport& transport,
	const mission::NetworkOutboundMessage& message)
{
	if (transport.gameplay_count
		>= kMultiplayerGameplayQueueCapacity)
	{
		if (conditional_delivery(message.delivery))
		{
			return true;
		}
		queue_error(
			transport, MultiplayerTransportError::queue_full, false);
		return false;
	}
	const std::uint8_t write = static_cast<std::uint8_t>(
		(transport.gameplay_read + transport.gameplay_count)
		% kMultiplayerGameplayQueueCapacity);
	transport.gameplay[write] = message;
	++transport.gameplay_count;
	MultiplayerTransportEvent event;
	event.kind =
		MultiplayerTransportEventKind::gameplay_available;
	(void)queue_event(transport, event);
	return true;
}

bool can_queue_gameplay(
	const MultiplayerTransport& transport,
	const mission::NetworkOutboundMessage& message)
{
	return transport.gameplay_count
			< kMultiplayerGameplayQueueCapacity
		|| conditional_delivery(message.delivery);
}

bool valid_utf8(const char* text, std::size_t capacity)
{
	if (!terminated(text, capacity))
	{
		return false;
	}
	const auto* bytes =
		reinterpret_cast<const unsigned char*>(text);
	std::size_t index = 0;
	while (bytes[index] != 0)
	{
		const unsigned char lead = bytes[index];
		if (lead < 0x80)
		{
			++index;
			continue;
		}
		std::size_t continuation = 0;
		std::uint32_t codepoint = 0;
		if (lead >= 0xc2 && lead <= 0xdf)
		{
			continuation = 1;
			codepoint = lead & 0x1fu;
		}
		else if (lead >= 0xe0 && lead <= 0xef)
		{
			continuation = 2;
			codepoint = lead & 0x0fu;
		}
		else if (lead >= 0xf0 && lead <= 0xf4)
		{
			continuation = 3;
			codepoint = lead & 0x07u;
		}
		else
		{
			return false;
		}
		for (std::size_t offset = 1;
			offset <= continuation;
			++offset)
		{
			if (index + offset >= capacity
				|| (bytes[index + offset] & 0xc0u)
					!= 0x80u)
			{
				return false;
			}
			codepoint = (codepoint << 6u)
				| (bytes[index + offset] & 0x3fu);
		}
		if ((continuation == 2
				&& codepoint >= 0xd800u
				&& codepoint <= 0xdfffu)
			|| (continuation == 2 && codepoint < 0x800u)
			|| (continuation == 3 && codepoint < 0x10000u)
			|| codepoint > 0x10ffffu)
		{
			return false;
		}
		index += continuation + 1;
	}
	return index != 0;
}

bool queue_post_mission_chat(
	MultiplayerTransport& transport,
	std::uint8_t source,
	const char* text)
{
	if (source >= kMultiplayerTransportPlayerCapacity
		|| !valid_utf8(text, mission::kNetworkChatBytes)
		|| transport.post_mission_chat_count
			>= kMultiplayerPostMissionChatCapacity)
	{
		if (transport.post_mission_chat_count
			>= kMultiplayerPostMissionChatCapacity)
		{
			queue_error(
				transport,
				MultiplayerTransportError::queue_full,
				false);
		}
		return false;
	}
	const std::uint8_t write = static_cast<std::uint8_t>(
		(transport.post_mission_chat_read
			+ transport.post_mission_chat_count)
		% kMultiplayerPostMissionChatCapacity);
	MultiplayerPostMissionChat& message =
		transport.post_mission_chat[write];
	message = {};
	message.source_player = source;
	(void)copy_text(message.text, text);
	++transport.post_mission_chat_count;
	MultiplayerTransportEvent event;
	event.kind = MultiplayerTransportEventKind::
		post_mission_chat_available;
	(void)queue_event(transport, event);
	return true;
}

bool queue_lobby_chat(
	MultiplayerTransport& transport,
	std::uint8_t source,
	const char* text)
{
	if (source >= kMultiplayerTransportPlayerCapacity
		|| !valid_utf8(text, mission::kNetworkChatBytes)
		|| transport.lobby_chat_count
			>= kMultiplayerLobbyChatCapacity)
	{
		if (transport.lobby_chat_count
			>= kMultiplayerLobbyChatCapacity)
		{
			queue_error(
				transport,
				MultiplayerTransportError::queue_full,
				false);
		}
		return false;
	}
	const std::uint8_t write = static_cast<std::uint8_t>(
		(transport.lobby_chat_read
			+ transport.lobby_chat_count)
		% kMultiplayerLobbyChatCapacity);
	MultiplayerLobbyChat& message =
		transport.lobby_chat[write];
	message = {};
	message.source_player = source;
	(void)copy_text(message.text, text);
	++transport.lobby_chat_count;
	MultiplayerTransportEvent event;
	event.kind =
		MultiplayerTransportEventKind::lobby_chat_available;
	(void)queue_event(transport, event);
	return true;
}

#if !defined(__EMSCRIPTEN__)
std::uint64_t random_u64()
{
	std::random_device random;
	std::uint64_t result =
		static_cast<std::uint64_t>(random()) << 32u;
	result ^= static_cast<std::uint64_t>(random());
	if (result == 0)
	{
		result = 1;
	}
	return result;
}

void generate_session_id(MultiplayerSessionId& id)
{
	const std::uint64_t first = random_u64();
	const std::uint64_t second = random_u64();
	for (std::size_t index = 0; index < 8; ++index)
	{
		id.bytes[index] = static_cast<std::uint8_t>(
			first >> ((7u - index) * 8u));
		id.bytes[index + 8] = static_cast<std::uint8_t>(
			second >> ((7u - index) * 8u));
	}
	// RFC-4122 variant/version bits make these IDs easy to recognize in
	// diagnostics without claiming interoperability with a UUID service.
	id.bytes[6] =
		static_cast<std::uint8_t>((id.bytes[6] & 0x0fu) | 0x40u);
	id.bytes[8] =
		static_cast<std::uint8_t>((id.bytes[8] & 0x3fu) | 0x80u);
}
#endif
}
#if !defined(__EMSCRIPTEN__)
namespace
{
#if defined(_WIN32)
using NativeSocket = SOCKET;
constexpr NativeSocket kInvalidNativeSocket = INVALID_SOCKET;
using SocketLength = int;
#else
using NativeSocket = int;
constexpr NativeSocket kInvalidNativeSocket = -1;
using SocketLength = socklen_t;
#endif

NativeSocket native_socket(std::uintptr_t handle)
{
	return handle == detail::kInvalidSocketHandle
		? kInvalidNativeSocket
		: static_cast<NativeSocket>(handle);
}

std::uintptr_t public_socket(NativeSocket socket)
{
	return socket == kInvalidNativeSocket
		? detail::kInvalidSocketHandle
		: static_cast<std::uintptr_t>(socket);
}

void close_native_socket(NativeSocket socket)
{
	if (socket == kInvalidNativeSocket)
	{
		return;
	}
#if defined(_WIN32)
	closesocket(socket);
#else
	close(socket);
#endif
}

void close_socket(std::uintptr_t& handle)
{
	close_native_socket(native_socket(handle));
	handle = detail::kInvalidSocketHandle;
}

int socket_error()
{
#if defined(_WIN32)
	return WSAGetLastError();
#else
	return errno;
#endif
}

bool socket_would_block(int error)
{
#if defined(_WIN32)
	return error == WSAEWOULDBLOCK
		|| error == WSAEINPROGRESS
		|| error == WSAEALREADY;
#else
	return error == EWOULDBLOCK
		|| error == EAGAIN
		|| error == EINPROGRESS
		|| error == EALREADY;
#endif
}

bool socket_interrupted(int error)
{
#if defined(_WIN32)
	return error == WSAEINTR;
#else
	return error == EINTR;
#endif
}

bool set_nonblocking(NativeSocket socket)
{
#if defined(_WIN32)
	u_long enabled = 1;
	return ioctlsocket(socket, FIONBIO, &enabled) == 0;
#else
	const int flags = fcntl(socket, F_GETFL, 0);
	return flags >= 0
		&& fcntl(socket, F_SETFL, flags | O_NONBLOCK) == 0;
#endif
}

bool set_boolean_option(
	NativeSocket socket,
	int level,
	int option,
	bool value)
{
	const int enabled = value ? 1 : 0;
#if defined(_WIN32)
	return setsockopt(
		socket,
		level,
		option,
		reinterpret_cast<const char*>(&enabled),
		static_cast<SocketLength>(sizeof(enabled))) == 0;
#else
	return setsockopt(
		socket,
		level,
		option,
		&enabled,
		static_cast<SocketLength>(sizeof(enabled))) == 0;
#endif
}

bool prepare_stream_socket(NativeSocket socket)
{
	if (!set_nonblocking(socket)
		|| !set_boolean_option(
			socket, IPPROTO_TCP, TCP_NODELAY, true))
	{
		return false;
	}
#if defined(SO_NOSIGPIPE)
	if (!set_boolean_option(
		socket, SOL_SOCKET, SO_NOSIGPIPE, true))
	{
		return false;
	}
#endif
	return true;
}

NativeSocket create_udp_socket(
	bool broadcast,
	bool reuse_address)
{
	const NativeSocket socket =
		::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	if (socket == kInvalidNativeSocket)
	{
		return kInvalidNativeSocket;
	}
	if (!set_nonblocking(socket)
		|| (broadcast
			&& !set_boolean_option(
				socket, SOL_SOCKET, SO_BROADCAST, true))
		|| (reuse_address
			&& !set_boolean_option(
				socket, SOL_SOCKET, SO_REUSEADDR, true)))
	{
		close_native_socket(socket);
		return kInvalidNativeSocket;
	}
	return socket;
}

bool bind_socket(
	NativeSocket socket,
	std::uint16_t port,
	std::uint16_t* bound_port = nullptr)
{
	sockaddr_in address{};
	address.sin_family = AF_INET;
	address.sin_addr.s_addr = htonl(INADDR_ANY);
	address.sin_port = htons(port);
	if (::bind(
			socket,
			reinterpret_cast<const sockaddr*>(&address),
			static_cast<SocketLength>(sizeof(address))) != 0)
	{
		return false;
	}
	if (bound_port != nullptr)
	{
		SocketLength length = sizeof(address);
		if (getsockname(
				socket,
				reinterpret_cast<sockaddr*>(&address),
				&length) != 0)
		{
			return false;
		}
		*bound_port = ntohs(address.sin_port);
	}
	return true;
}

NativeSocket create_bound_udp(
	std::uint16_t port,
	bool broadcast,
	bool reuse_address,
	std::uint16_t* bound_port = nullptr)
{
	const NativeSocket socket =
		create_udp_socket(broadcast, reuse_address);
	if (socket == kInvalidNativeSocket
		|| !bind_socket(socket, port, bound_port))
	{
		close_native_socket(socket);
		return kInvalidNativeSocket;
	}
	return socket;
}

NativeSocket create_listener(
	std::uint16_t port,
	std::uint16_t& bound_port)
{
	const NativeSocket socket =
		::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (socket == kInvalidNativeSocket)
	{
		return kInvalidNativeSocket;
	}
	if (!prepare_stream_socket(socket)
		|| !set_boolean_option(
			socket, SOL_SOCKET, SO_REUSEADDR, true)
		|| !bind_socket(socket, port, &bound_port)
		|| listen(socket, kMultiplayerTransportPlayerCapacity) != 0)
	{
		close_native_socket(socket);
		return kInvalidNativeSocket;
	}
	return socket;
}

bool socket_runtime_start(MultiplayerTransport& transport)
{
	if (transport.socket_runtime_initialized)
	{
		return true;
	}
#if defined(_WIN32)
	WSADATA data{};
	if (WSAStartup(MAKEWORD(2, 2), &data) != 0)
	{
		return false;
	}
#endif
	transport.socket_runtime_initialized = true;
	return true;
}

void socket_runtime_stop(MultiplayerTransport& transport)
{
	if (!transport.socket_runtime_initialized)
	{
		return;
	}
#if defined(_WIN32)
	WSACleanup();
#endif
	transport.socket_runtime_initialized = false;
}

void close_peer(detail::MultiplayerTransportPeer& peer)
{
	close_socket(peer.socket);
	peer = {};
}

void close_operation_sockets(MultiplayerTransport& transport)
{
	for (detail::MultiplayerTransportPeer& peer : transport.peers)
	{
		close_peer(peer);
	}
	close_socket(transport.listen_socket);
	close_socket(transport.discovery_socket);
	close_socket(transport.udp_socket);
	transport.discovery_active = false;
	transport.discovery_targeted = false;
	transport.discovery_target_address_be = 0;
	transport.discovery_target_port_be = 0;
	transport.datagram_read = 0;
	transport.datagram_count = 0;
}

bool ring_write(
	detail::MultiplayerTransportPeer& peer,
	const std::uint8_t* bytes,
	std::size_t count)
{
	if (bytes == nullptr
		|| count > detail::kTcpSendBytes - peer.send_count)
	{
		return false;
	}
	for (std::size_t index = 0; index < count; ++index)
	{
		const std::size_t destination =
			(peer.send_read + peer.send_count + index)
			% detail::kTcpSendBytes;
		peer.send[destination] = bytes[index];
	}
	peer.send_count = static_cast<std::uint16_t>(
		peer.send_count + count);
	return true;
}

bool queue_tcp_payload(
	detail::MultiplayerTransportPeer& peer,
	const std::uint8_t* payload,
	std::size_t payload_size)
{
	if (!peer.occupied
		|| payload == nullptr
		|| payload_size == 0
		|| payload_size > detail::kWirePacketBytes
		|| payload_size > UINT16_MAX
		|| payload_size + 2
			> detail::kTcpSendBytes - peer.send_count)
	{
		return false;
	}
	const std::uint8_t length[2]{
		static_cast<std::uint8_t>(payload_size >> 8u),
		static_cast<std::uint8_t>(payload_size),
	};
	return ring_write(peer, length, sizeof(length))
		&& ring_write(peer, payload, payload_size);
}

template<typename Body>
bool queue_tcp_packet(
	detail::MultiplayerTransportPeer& peer,
	PacketType type,
	const MultiplayerSessionId& session_id,
	Body&& body)
{
	std::uint8_t bytes[detail::kWirePacketBytes];
	Writer writer{bytes, sizeof(bytes)};
	write_header(writer, type, session_id);
	body(writer);
	return writer.valid
		&& queue_tcp_payload(peer, bytes, writer.count);
}

template<typename Body>
bool encode_packet(
	std::uint8_t (&bytes)[detail::kWirePacketBytes],
	std::size_t& count,
	PacketType type,
	const MultiplayerSessionId& session_id,
	Body&& body)
{
	Writer writer{bytes, sizeof(bytes)};
	write_header(writer, type, session_id);
	body(writer);
	count = writer.count;
	return writer.valid && writer.count != 0;
}

bool can_queue_tcp_payload(
	const detail::MultiplayerTransportPeer& peer,
	std::size_t payload_size)
{
	return peer.occupied
		&& payload_size != 0
		&& payload_size <= detail::kWirePacketBytes
		&& payload_size <= UINT16_MAX
		&& payload_size + 2
			<= detail::kTcpSendBytes - peer.send_count;
}

bool queue_datagram(
	MultiplayerTransport& transport,
	const std::uint8_t* bytes,
	std::size_t count,
	std::uint32_t address_be,
	std::uint16_t port_be)
{
	if (bytes == nullptr
		|| count == 0
		|| count > detail::kWirePacketBytes)
	{
		return false;
	}
	if (transport.datagram_count
		>= detail::kDatagramQueueCapacity)
	{
		// All queued datagrams are either discovery/probes or retail
		// conditional gameplay. Dropping one under pressure is the
		// intended non-guaranteed delivery behavior.
		return true;
	}
	const std::uint8_t write = static_cast<std::uint8_t>(
		(transport.datagram_read + transport.datagram_count)
		% detail::kDatagramQueueCapacity);
	detail::MultiplayerQueuedDatagram& datagram =
		transport.datagrams[write];
	std::memcpy(datagram.bytes, bytes, count);
	datagram.length = static_cast<std::uint16_t>(count);
	datagram.address_be = address_be;
	datagram.port_be = port_be;
	++transport.datagram_count;
	return true;
}

template<typename Body>
bool queue_udp_packet(
	MultiplayerTransport& transport,
	PacketType type,
	const MultiplayerSessionId& session_id,
	std::uint32_t address_be,
	std::uint16_t port_be,
	Body&& body)
{
	std::uint8_t bytes[detail::kWirePacketBytes];
	Writer writer{bytes, sizeof(bytes)};
	write_header(writer, type, session_id);
	body(writer);
	return writer.valid
		&& queue_datagram(
			transport,
			bytes,
			writer.count,
			address_be,
			port_be);
}

bool flush_peer(
	detail::MultiplayerTransportPeer& peer,
	std::uint64_t now)
{
	if (!peer.occupied || peer.connecting)
	{
		return true;
	}
	const NativeSocket socket = native_socket(peer.socket);
	while (peer.send_count != 0)
	{
		const std::size_t contiguous = std::min<std::size_t>(
			peer.send_count,
			detail::kTcpSendBytes - peer.send_read);
#if defined(MSG_NOSIGNAL)
		const int flags = MSG_NOSIGNAL;
#else
		const int flags = 0;
#endif
		const int sent = ::send(
			socket,
			reinterpret_cast<const char*>(
				peer.send + peer.send_read),
			static_cast<int>(contiguous),
			flags);
		if (sent > 0)
		{
			peer.send_read = static_cast<std::uint16_t>(
				(peer.send_read + sent)
				% detail::kTcpSendBytes);
			peer.send_count = static_cast<std::uint16_t>(
				peer.send_count - sent);
			peer.last_send_at = now;
			continue;
		}
		if (sent == 0)
		{
			return false;
		}
		const int error = socket_error();
		if (socket_interrupted(error))
		{
			continue;
		}
		return socket_would_block(error);
	}
	peer.send_read = 0;
	return true;
}

bool flush_datagrams(MultiplayerTransport& transport)
{
	const NativeSocket socket = native_socket(
		transport.udp_socket != detail::kInvalidSocketHandle
			? transport.udp_socket
			: transport.discovery_socket);
	if (socket == kInvalidNativeSocket)
	{
		return transport.datagram_count == 0;
	}
	while (transport.datagram_count != 0)
	{
		detail::MultiplayerQueuedDatagram& datagram =
			transport.datagrams[transport.datagram_read];
		sockaddr_in destination{};
		destination.sin_family = AF_INET;
		destination.sin_addr.s_addr = datagram.address_be;
		destination.sin_port = datagram.port_be;
		const int sent = sendto(
			socket,
			reinterpret_cast<const char*>(datagram.bytes),
			datagram.length,
			0,
			reinterpret_cast<const sockaddr*>(&destination),
			static_cast<SocketLength>(sizeof(destination)));
		if (sent == datagram.length)
		{
			datagram = {};
			transport.datagram_read =
				static_cast<std::uint8_t>(
					(transport.datagram_read + 1)
					% detail::kDatagramQueueCapacity);
			--transport.datagram_count;
			continue;
		}
		if (sent < 0)
		{
			const int error = socket_error();
			if (socket_interrupted(error))
			{
				continue;
			}
			if (socket_would_block(error))
			{
				return true;
			}
		}
		// Conditional datagrams are not retried after a terminal send
		// error; leaving one at the head would stall all later peers.
		datagram = {};
		transport.datagram_read = static_cast<std::uint8_t>(
			(transport.datagram_read + 1)
			% detail::kDatagramQueueCapacity);
		--transport.datagram_count;
	}
	transport.datagram_read = 0;
	return true;
}
}
#endif
#if !defined(__EMSCRIPTEN__)
namespace
{
void discovery_changed(MultiplayerTransport& transport)
{
	++transport.discovery_generation;
	MultiplayerTransportEvent event;
	event.kind =
		MultiplayerTransportEventKind::discovery_updated;
	(void)queue_event(transport, event);
}

void send_discovery_query(MultiplayerTransport& transport)
{
	const MultiplayerSessionId empty_id{};
	const auto body = [&](Writer& writer)
	{
		writer.u32(transport.discovery_nonce);
	};
	if (transport.discovery_targeted)
	{
		(void)queue_udp_packet(
			transport,
			PacketType::discovery_query,
			empty_id,
			transport.discovery_target_address_be,
			transport.discovery_target_port_be,
			body);
		return;
	}
	(void)queue_udp_packet(
		transport,
		PacketType::discovery_query,
		empty_id,
		htonl(INADDR_BROADCAST),
		htons(kMultiplayerDiscoveryPort),
		body);
	(void)queue_udp_packet(
		transport,
		PacketType::discovery_query,
		empty_id,
		htonl(INADDR_LOOPBACK),
		htons(kMultiplayerDiscoveryPort),
		body);
}

bool start_discovery(
	MultiplayerTransport& transport,
	std::uint64_t now,
	bool targeted,
	std::uint32_t target_address_be,
	std::uint16_t target_port_be)
{
	close_operation_sockets(transport);
	std::fill(
		std::begin(transport.discovered),
		std::end(transport.discovered),
		MultiplayerDiscoverySession{});
	transport.discovery_generation = 0;
	transport.discovery_nonce =
		static_cast<std::uint32_t>(random_u64());
	const NativeSocket socket =
		create_bound_udp(0, true, false);
	if (socket == kInvalidNativeSocket)
	{
		queue_error(
			transport,
			MultiplayerTransportError::socket_create,
			true);
		return false;
	}
	transport.discovery_socket = public_socket(socket);
	transport.discovery_active = true;
	transport.discovery_targeted = targeted;
	transport.discovery_target_address_be =
		targeted ? target_address_be : 0;
	transport.discovery_target_port_be =
		targeted ? target_port_be : 0;
	transport.host = false;
	transport.state = MultiplayerTransportState::discovering;
	send_discovery_query(transport);
	transport.last_discovery_query_at = now;
	transport.started_at = now;
	transport.last_error = MultiplayerTransportError::none;
	return true;
}

void send_discovery_offer(
	MultiplayerTransport& transport,
	std::uint32_t nonce,
	const sockaddr_in& destination)
{
	(void)queue_udp_packet(
		transport,
		PacketType::discovery_offer,
		transport.lobby.session_id,
		destination.sin_addr.s_addr,
		destination.sin_port,
		[&](Writer& writer)
		{
			writer.u32(nonce);
			writer.text(transport.lobby.session_name);
			writer.u16(transport.session_port);
			writer.u16(transport.lobby.rules.mission);
			writer.u8(transport.lobby.player_count);
			writer.u8(kMultiplayerTransportPlayerCapacity);
				writer.u8(static_cast<std::uint8_t>(
					transport.lobby.rules.mode));
				writer.boolean(transport.lobby.rules.team_mode);
				writer.boolean(transport.lobby.rules.ai_turrets);
			});
}

void retain_discovery_offer(
	MultiplayerTransport& transport,
	const MultiplayerSessionId& session_id,
	Reader& reader,
	const sockaddr_in& source,
	std::uint64_t now)
{
	const std::uint32_t nonce = reader.u32();
	MultiplayerDiscoverySession candidate;
	candidate.session_id = session_id;
	reader.text(candidate.session_name);
	candidate.port = reader.u16();
	candidate.mission = reader.u16();
	candidate.player_count = reader.u8();
	candidate.player_capacity = reader.u8();
	candidate.mode =
		static_cast<MultiplayerSessionMode>(reader.u8());
	candidate.team_mode = reader.boolean();
	candidate.ai_turrets = reader.boolean();
	candidate.last_seen_at = static_cast<std::uint32_t>(now);
	if (!reader.done()
		|| nonce != transport.discovery_nonce
		|| id_is_zero(candidate.session_id)
		|| candidate.session_name[0] == '\0'
		|| candidate.port == 0
		|| candidate.mission == 0
		|| candidate.player_count == 0
		|| candidate.player_count
			> kMultiplayerTransportPlayerCapacity
		|| candidate.player_capacity
			!= kMultiplayerTransportPlayerCapacity
		|| (candidate.mode
				!= MultiplayerSessionMode::cooperative
			&& candidate.mode
				!= MultiplayerSessionMode::deathmatch)
			|| (candidate.team_mode
				&& candidate.mode
					!= MultiplayerSessionMode::deathmatch)
			|| (candidate.ai_turrets
				&& candidate.mode
					!= MultiplayerSessionMode::deathmatch)
		|| inet_ntop(
			AF_INET,
			&source.sin_addr,
			candidate.address,
			sizeof(candidate.address)) == nullptr)
	{
		return;
	}
	for (std::size_t index = 0;
		index < kMultiplayerDiscoveryCapacity;
		++index)
	{
		MultiplayerDiscoverySession& current =
			transport.discovered[index];
		if (current.session_id == candidate.session_id)
		{
			const bool changed =
				std::memcmp(
					&current,
					&candidate,
					offsetof(
						MultiplayerDiscoverySession,
						last_seen_at)) != 0
				|| current.port != candidate.port
				|| current.mission != candidate.mission
				|| current.player_count
					!= candidate.player_count
				|| current.player_capacity
					!= candidate.player_capacity
					|| current.mode != candidate.mode
					|| current.team_mode != candidate.team_mode
					|| current.ai_turrets != candidate.ai_turrets;
			current = candidate;
			if (changed)
			{
				discovery_changed(transport);
			}
			return;
		}
	}
	for (MultiplayerDiscoverySession& current :
		transport.discovered)
	{
		if (id_is_zero(current.session_id))
		{
			current = candidate;
			discovery_changed(transport);
			return;
		}
	}
	// Replace the stalest entry rather than allocating an unbounded list.
	std::size_t oldest = 0;
	for (std::size_t index = 1;
		index < kMultiplayerDiscoveryCapacity;
		++index)
	{
		if (transport.discovered[index].last_seen_at
			< transport.discovered[oldest].last_seen_at)
		{
			oldest = index;
		}
	}
	transport.discovered[oldest] = candidate;
	discovery_changed(transport);
}

void service_discovery_receive(
	MultiplayerTransport& transport,
	std::uint64_t now)
{
	const NativeSocket socket =
		native_socket(transport.discovery_socket);
	if (socket == kInvalidNativeSocket)
	{
		return;
	}
	for (;;)
	{
		std::uint8_t bytes[detail::kWirePacketBytes];
		sockaddr_in source{};
		SocketLength source_length = sizeof(source);
		const int received = recvfrom(
			socket,
			reinterpret_cast<char*>(bytes),
			static_cast<int>(sizeof(bytes)),
			0,
			reinterpret_cast<sockaddr*>(&source),
			&source_length);
		if (received < 0)
		{
			const int error = socket_error();
			if (socket_interrupted(error))
			{
				continue;
			}
			break;
		}
		if (received == 0
			|| source.sin_family != AF_INET)
		{
			continue;
		}
		Reader reader{
			bytes, static_cast<std::size_t>(received)};
		PacketType type{};
		MultiplayerSessionId session_id;
		if (!read_header(reader, type, session_id))
		{
			continue;
		}
		if (transport.host
			&& type == PacketType::discovery_query
			&& id_is_zero(session_id)
			&& transport.state
				== MultiplayerTransportState::hosting_lobby
			&& transport.lobby.player_count
				< kMultiplayerTransportPlayerCapacity)
		{
			const std::uint32_t nonce = reader.u32();
			if (reader.done())
			{
				send_discovery_offer(
					transport, nonce, source);
			}
		}
		else if (transport.discovery_active
			&& !transport.host
			&& type == PacketType::discovery_offer)
		{
			retain_discovery_offer(
				transport,
				session_id,
				reader,
				source,
				now);
		}
	}
}

void expire_discovery(
	MultiplayerTransport& transport,
	std::uint64_t now)
{
	bool changed = false;
	const std::uint32_t current = static_cast<std::uint32_t>(now);
	std::size_t write = 0;
	for (std::size_t read = 0;
		read < kMultiplayerDiscoveryCapacity;
		++read)
	{
		MultiplayerDiscoverySession& session =
			transport.discovered[read];
		if (!id_is_zero(session.session_id)
			&& static_cast<std::uint32_t>(
				current - session.last_seen_at)
				> kDiscoveryExpiryMilliseconds)
		{
			changed = true;
			continue;
		}
		if (id_is_zero(session.session_id))
		{
			continue;
		}
		if (write != read)
		{
			transport.discovered[write] = session;
			changed = true;
		}
		++write;
	}
	for (std::size_t index = write;
		index < kMultiplayerDiscoveryCapacity;
		++index)
	{
		transport.discovered[index] = {};
	}
	if (changed)
	{
		discovery_changed(transport);
	}
}

bool session_id_matches(
	const MultiplayerSessionId& expected,
	const MultiplayerSessionId& received,
	bool allow_expected_zero)
{
	return (allow_expected_zero && id_is_zero(expected))
		|| expected == received;
}

enum class AuthorityPhase : std::uint8_t
{
	lobby,
	prelaunch,
	gameplay,
	post_mission,
};

bool authority_phase(
	const MultiplayerTransport& transport,
	AuthorityPhase& phase)
{
	switch (transport.state)
	{
	case MultiplayerTransportState::hosting_lobby:
	case MultiplayerTransportState::client_lobby:
		phase = AuthorityPhase::lobby;
		return true;
	case MultiplayerTransportState::host_prelaunch:
	case MultiplayerTransportState::client_prelaunch:
		phase = AuthorityPhase::prelaunch;
		return true;
	case MultiplayerTransportState::gameplay:
		phase = AuthorityPhase::gameplay;
		return true;
	case MultiplayerTransportState::post_mission:
		phase = AuthorityPhase::post_mission;
		return true;
	default:
		return false;
	}
}

MultiplayerTransportState transport_state_for_authority_phase(
	AuthorityPhase phase,
	bool host)
{
	switch (phase)
	{
	case AuthorityPhase::lobby:
		return host
			? MultiplayerTransportState::hosting_lobby
			: MultiplayerTransportState::client_lobby;
	case AuthorityPhase::prelaunch:
		return host
			? MultiplayerTransportState::host_prelaunch
			: MultiplayerTransportState::client_prelaunch;
	case AuthorityPhase::gameplay:
		return MultiplayerTransportState::gameplay;
	case AuthorityPhase::post_mission:
		return MultiplayerTransportState::post_mission;
	}
	return MultiplayerTransportState::disconnected;
}

detail::MultiplayerTransportPeer* peer_for_lobby_slot(
	MultiplayerTransport& transport,
	std::uint8_t lobby_slot)
{
	for (detail::MultiplayerTransportPeer& peer : transport.peers)
	{
		if (peer.occupied
			&& peer.authenticated
			&& peer.lobby_slot == lobby_slot)
		{
			return &peer;
		}
	}
	return nullptr;
}

const detail::MultiplayerTransportPeer* peer_for_lobby_slot(
	const MultiplayerTransport& transport,
	std::uint8_t lobby_slot)
{
	for (const detail::MultiplayerTransportPeer& peer :
		transport.peers)
	{
		if (peer.occupied
			&& peer.authenticated
			&& peer.lobby_slot == lobby_slot)
		{
			return &peer;
		}
	}
	return nullptr;
}

void write_member(
	Writer& writer,
	const detail::MultiplayerTransportMember& member)
{
	writer.u64(member.identity_token);
	writer.u32(member.address_be);
	writer.u16(member.listener_port_be);
	writer.u16(member.udp_port_be);
	writer.u8(member.gameplay_slot);
	writer.boolean(member.connected);
	writer.boolean(member.post_mission);
}

void read_member(
	Reader& reader,
	detail::MultiplayerTransportMember& member)
{
	member = {};
	member.identity_token = reader.u64();
	member.address_be = reader.u32();
	member.listener_port_be = reader.u16();
	member.udp_port_be = reader.u16();
	member.gameplay_slot = reader.u8();
	member.connected = reader.boolean();
	member.post_mission = reader.boolean();
}

bool valid_member_table(
	const detail::MultiplayerTransportMember (&members)[
		kMultiplayerTransportPlayerCapacity],
	const MultiplayerLobbySnapshot& lobby,
	std::uint8_t authority_lobby_slot,
	bool launch_valid,
	const std::uint8_t (&gameplay_lobby_slot)[
		kMultiplayerTransportPlayerCapacity],
	std::uint8_t gameplay_player_count)
{
	if (!valid_lobby(lobby)
		|| authority_lobby_slot
			>= kMultiplayerTransportPlayerCapacity
		|| lobby.leader_slot != authority_lobby_slot
		|| !members[authority_lobby_slot].connected)
	{
		return false;
	}
	for (std::uint8_t slot = 0;
		slot < kMultiplayerTransportPlayerCapacity;
		++slot)
	{
		const detail::MultiplayerTransportMember& member =
			members[slot];
		if (member.connected
			!= lobby.players[slot].connected)
		{
			return false;
		}
		if (!member.connected)
		{
			if (member.post_mission)
			{
				return false;
			}
			continue;
		}
		if (member.identity_token == 0
			|| member.listener_port_be == 0
			|| member.udp_port_be == 0)
		{
			return false;
		}
		for (std::uint8_t earlier = 0;
			earlier < slot;
			++earlier)
		{
			if (members[earlier].connected
				&& members[earlier].identity_token
					== member.identity_token)
			{
				return false;
			}
		}
		if (!launch_valid)
		{
			if (member.gameplay_slot != UINT8_MAX)
			{
				return false;
			}
			continue;
		}
		if (member.gameplay_slot >= gameplay_player_count
			|| gameplay_lobby_slot[member.gameplay_slot]
				!= slot)
		{
			return false;
		}
	}
	if (launch_valid)
	{
		for (std::uint8_t gameplay_player = 0;
			gameplay_player < gameplay_player_count;
			++gameplay_player)
		{
			const std::uint8_t lobby_slot =
				gameplay_lobby_slot[gameplay_player];
			if (lobby_slot
					>= kMultiplayerTransportPlayerCapacity
				|| (members[lobby_slot].connected
					&& members[lobby_slot].gameplay_slot
						!= gameplay_player))
			{
				return false;
			}
		}
	}
	return true;
}

void synchronize_member_gameplay_slots(
	MultiplayerTransport& transport)
{
	for (detail::MultiplayerTransportMember& member :
		transport.members)
	{
		member.gameplay_slot = UINT8_MAX;
	}
	if (!transport.launch_snapshot_valid)
	{
		return;
	}
	for (std::uint8_t gameplay_player = 0;
		gameplay_player < transport.launch_snapshot.player_count;
		++gameplay_player)
	{
		const std::uint8_t lobby_slot =
			transport.gameplay_lobby_slot[gameplay_player];
		if (lobby_slot < kMultiplayerTransportPlayerCapacity)
		{
			transport.members[lobby_slot].gameplay_slot =
				gameplay_player;
		}
	}
}

std::uint8_t authority_gameplay_player(
	const MultiplayerTransport& transport)
{
	return transport.authority_lobby_slot
				< kMultiplayerTransportPlayerCapacity
		? transport.members[
			transport.authority_lobby_slot].gameplay_slot
		: UINT8_MAX;
}

void mark_authority_snapshot_dirty(
	MultiplayerTransport& transport)
{
	if (transport.host)
	{
		transport.authority_snapshot_dirty = true;
	}
}

bool gameplay_player_connected(
	const MultiplayerTransport& transport,
	std::uint8_t gameplay_player)
{
	return transport.launch_snapshot_valid
		&& gameplay_player
			< transport.launch_snapshot.player_count
		&& transport.launch_snapshot.players[
			gameplay_player].connected;
}

void refresh_debrief_leader(MultiplayerTransport& transport)
{
	if (!transport.launch_snapshot_valid
		|| transport.launch_snapshot.deathmatch_mode)
	{
		return;
	}
	if (has_direct_mission_25_report(transport.mission_result))
	{
		transport.mission_result.debrief_leader_player = UINT8_MAX;
		transport.mission_result.debrief_leader_valid = false;
		return;
	}
	const DebriefLeader leader =
		compute_debrief_leader(
			transport.mission_result,
			transport.launch_snapshot);
	transport.mission_result.debrief_leader_player =
		leader.player;
	transport.mission_result.debrief_leader_valid =
		leader.valid;
}

bool valid_destination(
	const MultiplayerTransport& transport,
	const mission::NetworkOutboundMessage& message)
{
	return message.destination_player == kBroadcastPlayer
		|| gameplay_player_connected(
			transport, message.destination_player);
}

bool source_owns_object(
	const MultiplayerTransport& transport,
	std::uint8_t source,
	std::uint16_t object)
{
	if (!transport.launch_snapshot_valid
		|| source >= transport.launch_snapshot.player_count
		|| object >= game::kMaxGameObjects)
	{
		return false;
	}
	const game::MultiplayerLaunchSnapshot& launch =
		transport.launch_snapshot;
	if (object < launch.gameplay_player_prefix)
	{
		return object == source;
	}
	if (launch.deathmatch_mode)
	{
		return source == authority_gameplay_player(transport);
	}
	if (object == 399)
	{
		return true;
	}
	return (object - source) % launch.player_count == 0;
}

bool validate_authenticated_gameplay(
	const MultiplayerTransport& transport,
	mission::NetworkOutboundMessage& message,
	std::uint8_t authenticated_source)
{
	message.source_player = authenticated_source;
	if (message.kind == mission::NetworkOutboundKind::gameplay)
	{
		using Opcode = mission::NetworkGameplayOpcode;
		switch (message.opcode)
		{
		case Opcode::player_departure:
		{
			// Opcode 0x4c is REJECT, not a source declaration:
			// FUN_004bb950 takes an arbitrary four-bit topology slot.
			if (message.departure_player
					>= transport.launch_snapshot.player_count
				|| !gameplay_player_connected(
					transport, message.departure_player))
			{
				return false;
			}
			break;
		}
		case Opcode::player_target_reference:
			// source_player was replaced with the authenticated gameplay
			// identity above. The remaining 9+7-bit target tuple is
			// canonicalized and range-checked by valid_semantic_message.
			break;
		case Opcode::deathmatch_state_request:
			message.scenario_player =
				static_cast<std::int8_t>(
					authenticated_source);
			break;
		case Opcode::deathmatch_respawn:
			if (message.object_index != authenticated_source)
			{
				return false;
			}
			break;
		case Opcode::session_script_ready:
		{
			// Replies are directed to the current simulation authority.
			// DirectPlay's migrate-host flag moves that role without
			// renumbering the in-flight gameplay roster.
			const std::uint8_t authority =
				authority_gameplay_player(transport);
			if (authority
				>= kMultiplayerTransportPlayerCapacity)
			{
				return false;
			}
			if (authenticated_source != authority)
			{
				message.destination_player = authority;
			}
			break;
		}
		case Opcode::component_state:
		case Opcode::bank_state:
			if (!source_owns_object(
				transport,
				authenticated_source,
				message.object_index))
			{
				return false;
			}
			break;
		default:
			break;
		}
	}
	else if (message.kind
			== mission::NetworkOutboundKind::forced_object_state
		|| message.kind
			== mission::NetworkOutboundKind::ai_deferred_command)
	{
		if (!source_owns_object(
			transport,
			authenticated_source,
			message.object_index))
		{
			return false;
		}
	}
	return valid_semantic_message(message)
		&& valid_destination(transport, message);
}

void retain_deathmatch_player_stats(
	MultiplayerTransport& transport,
	const mission::NetworkOutboundMessage& message)
{
	if (!transport.launch_snapshot_valid
		|| !transport.launch_snapshot.deathmatch_mode
		|| message.kind != mission::NetworkOutboundKind::gameplay
		|| message.opcode
			!= mission::NetworkGameplayOpcode::player_stats
		|| message.source_player
			>= transport.launch_snapshot.player_count)
	{
		return;
	}
	const std::uint8_t gameplay_player = message.source_player;
	transport.mission_result.deathmatch_kills[gameplay_player] =
		message.player_kills;
	transport.mission_result.deathmatch_deaths[gameplay_player] =
		message.player_deaths;
	const std::uint8_t lobby_slot =
		transport.gameplay_lobby_slot[gameplay_player];
	if (lobby_slot < kMultiplayerTransportPlayerCapacity
		&& transport.lobby.players[lobby_slot].connected)
	{
		transport.lobby.players[lobby_slot].deathmatch_kills =
			message.player_kills;
		transport.lobby.players[lobby_slot].deathmatch_deaths =
			message.player_deaths;
	}
	mark_authority_snapshot_dirty(transport);
}

bool sequence_newer(std::uint32_t candidate, std::uint32_t previous)
{
	return static_cast<std::int32_t>(candidate - previous) > 0;
}

bool broadcast_lobby_snapshot(
	MultiplayerTransport& transport,
	const MultiplayerLobbySnapshot& lobby)
{
	std::uint8_t bytes[detail::kWirePacketBytes];
	std::size_t count = 0;
	if (!encode_packet(
			bytes,
			count,
			PacketType::lobby_snapshot,
			lobby.session_id,
			[&](Writer& writer)
			{
				write_lobby(writer, lobby);
			}))
	{
		return false;
	}
	for (const detail::MultiplayerTransportPeer& peer :
		transport.peers)
	{
		if (peer.occupied
			&& peer.authenticated
			&& !can_queue_tcp_payload(peer, count))
		{
			return false;
		}
	}
	for (detail::MultiplayerTransportPeer& peer :
		transport.peers)
	{
		if (peer.occupied
			&& peer.authenticated
			&& !queue_tcp_payload(peer, bytes, count))
		{
			return false;
		}
	}
	MultiplayerTransportEvent event;
	event.kind = MultiplayerTransportEventKind::lobby_updated;
	(void)queue_event(transport, event);
	return true;
}

bool broadcast_lobby(MultiplayerTransport& transport)
{
	transport.lobby.player_count =
		connected_player_count(transport.lobby);
	return broadcast_lobby_snapshot(
		transport, transport.lobby);
}

template<typename Body, typename Include>
bool broadcast_tcp_packet(
	MultiplayerTransport& transport,
	PacketType type,
	Body&& body,
	Include&& include)
{
	std::uint8_t bytes[detail::kWirePacketBytes];
	std::size_t count = 0;
	if (!encode_packet(
			bytes,
			count,
			type,
			transport.lobby.session_id,
			std::forward<Body>(body)))
	{
		return false;
	}
	for (const detail::MultiplayerTransportPeer& peer :
		transport.peers)
	{
		if (peer.occupied
			&& peer.authenticated
			&& include(peer)
			&& !can_queue_tcp_payload(peer, count))
		{
			return false;
		}
	}
	for (detail::MultiplayerTransportPeer& peer :
		transport.peers)
	{
		if (peer.occupied
			&& peer.authenticated
			&& include(peer)
			&& !queue_tcp_payload(peer, bytes, count))
		{
			return false;
		}
	}
	return true;
}

bool encode_authority_snapshot(
	const MultiplayerTransport& transport,
	std::uint8_t (&bytes)[detail::kWirePacketBytes],
	std::size_t& count)
{
	AuthorityPhase phase;
	if (!transport.host
		|| transport.authority_epoch == 0
		|| transport.authority_token == 0
		|| transport.authority_lobby_slot
			>= kMultiplayerTransportPlayerCapacity
		|| !authority_phase(transport, phase))
	{
		return false;
	}
	return encode_packet(
		bytes,
		count,
		PacketType::authority_snapshot,
		transport.lobby.session_id,
		[&](Writer& writer)
		{
			writer.u32(transport.authority_epoch);
			writer.u64(transport.authority_token);
			writer.u8(transport.authority_lobby_slot);
			writer.u8(static_cast<std::uint8_t>(phase));
			for (const detail::MultiplayerTransportMember& member :
				transport.members)
			{
				write_member(writer, member);
			}
			write_lobby(writer, transport.lobby);
		});
}

void write_partial_mission_state(
	Writer& writer,
	const MultiplayerMissionResultSnapshot& result)
{
	for (const MultiplayerPlayerMissionOutcome outcome :
		result.players)
	{
		writer.u8(static_cast<std::uint8_t>(outcome));
	}
	for (const std::int32_t kills : result.deathmatch_kills)
	{
		writer.i32(kills);
	}
	for (const std::int32_t deaths : result.deathmatch_deaths)
	{
		writer.i32(deaths);
	}
	writer.boolean(result.network_aborted);
}

bool encode_authority_runtime(
	const MultiplayerTransport& transport,
	const detail::MultiplayerTransportPeer& peer,
	std::uint8_t (&bytes)[detail::kWirePacketBytes],
	std::size_t& count)
{
	if (!transport.host
		|| !peer.authenticated
		|| peer.lobby_slot
			>= kMultiplayerTransportPlayerCapacity)
	{
		return false;
	}
	game::MultiplayerLaunchSnapshot launch =
		transport.launch_snapshot;
	if (transport.launch_snapshot_valid)
	{
		const std::uint8_t recipient_gameplay =
			transport.members[peer.lobby_slot].gameplay_slot;
		if (recipient_gameplay >= launch.player_count)
		{
			return false;
		}
		launch.role = game::MultiplayerRole::client;
		launch.local_player = recipient_gameplay;
		// write_launch/read_launch retain their strict initial-roster
		// invariant. Current connectivity is carried by the replicated
		// member table and restored after decoding.
		for (std::uint8_t player = 0;
			player < launch.player_count;
			++player)
		{
			launch.players[player].connected = true;
		}
	}
	return encode_packet(
		bytes,
		count,
		PacketType::authority_runtime,
		transport.lobby.session_id,
		[&](Writer& writer)
		{
			writer.u32(transport.authority_epoch);
			writer.u64(transport.authority_token);
			writer.u8(transport.authority_lobby_slot);
			const bool publish_campaign_control =
				transport.state
						== MultiplayerTransportState::
							host_prelaunch
				|| transport.launch_snapshot_valid
				|| transport.shared_bootstrap_pending;
			write_campaign_control(
				writer,
				publish_campaign_control
					? transport.prelaunch_campaign
					: game::
						MultiplayerCampaignLaunchState{});
			writer.u32(transport.launch_generation);
			writer.boolean(
				transport.shared_bootstrap_pending);
			write_mission_bootstrap(
				writer, transport.shared_bootstrap);
			write_mission_bootstrap(
				writer, transport.replay_bootstrap);
			for (const std::uint8_t lobby_slot :
				transport.gameplay_lobby_slot)
			{
				writer.u8(lobby_slot);
			}
			writer.boolean(transport.launch_snapshot_valid);
			if (transport.launch_snapshot_valid)
			{
				write_launch(writer, launch);
			}
			write_partial_mission_state(
				writer,
				transport.launch_snapshot_valid
					? transport.mission_result
					: MultiplayerMissionResultSnapshot{});
		});
}

bool queue_authority_state_for_peer(
	MultiplayerTransport& transport,
	detail::MultiplayerTransportPeer& peer)
{
	std::uint8_t snapshot_bytes[detail::kWirePacketBytes];
	std::uint8_t runtime_bytes[detail::kWirePacketBytes];
	std::uint8_t report_bytes[
		kMultiplayerTransportPlayerCapacity][
			detail::kWirePacketBytes]{};
	std::size_t snapshot_count = 0;
	std::size_t runtime_count = 0;
	std::size_t report_count[
		kMultiplayerTransportPlayerCapacity]{};
	if (!encode_authority_snapshot(
			transport, snapshot_bytes, snapshot_count)
		|| !encode_authority_runtime(
			transport, peer, runtime_bytes, runtime_count))
	{
		return false;
	}
	std::size_t required = snapshot_count + runtime_count + 4;
	for (std::uint8_t player = 0;
		player < kMultiplayerTransportPlayerCapacity;
		++player)
	{
		const MultiplayerPlayerMissionReport& report =
			transport.mission_result.reports[player];
		if (!transport.launch_snapshot_valid
			|| !report.valid)
		{
			continue;
		}
		if (!encode_packet(
				report_bytes[player],
				report_count[player],
				PacketType::mission_result,
				transport.lobby.session_id,
				[&](Writer& writer)
				{
					writer.u8(static_cast<std::uint8_t>(
						MissionResultPayload::player_report));
					writer.u8(player);
					writer.u8(report.lobby_slot);
					write_player_mission_report(
						writer, report);
				}))
		{
			return false;
		}
		required += report_count[player] + 2;
	}
	if (required > detail::kTcpSendBytes - peer.send_count
		|| !queue_tcp_payload(
			peer, snapshot_bytes, snapshot_count)
		|| !queue_tcp_payload(
			peer, runtime_bytes, runtime_count))
	{
		return false;
	}
	for (std::uint8_t player = 0;
		player < kMultiplayerTransportPlayerCapacity;
		++player)
	{
		if (report_count[player] != 0
			&& !queue_tcp_payload(
				peer,
				report_bytes[player],
				report_count[player]))
		{
			return false;
		}
	}
	return true;
}

bool replicate_authority_state(
	MultiplayerTransport& transport,
	std::uint64_t now)
{
	std::uint8_t snapshot_bytes[detail::kWirePacketBytes];
	std::size_t snapshot_count = 0;
	if (!encode_authority_snapshot(
		transport, snapshot_bytes, snapshot_count))
	{
		return false;
	}
	std::uint8_t runtime_bytes[
		kMultiplayerTransportPlayerCapacity][
			detail::kWirePacketBytes]{};
	std::size_t runtime_count[
		kMultiplayerTransportPlayerCapacity]{};
	std::uint8_t report_bytes[
		kMultiplayerTransportPlayerCapacity][
			detail::kWirePacketBytes]{};
	std::size_t report_count[
		kMultiplayerTransportPlayerCapacity]{};
	std::size_t report_wire_bytes = 0;
	if (transport.launch_snapshot_valid)
	{
		for (std::uint8_t player = 0;
			player < kMultiplayerTransportPlayerCapacity;
			++player)
		{
			const MultiplayerPlayerMissionReport& report =
				transport.mission_result.reports[player];
			if (!report.valid)
			{
				continue;
			}
			if (!encode_packet(
					report_bytes[player],
					report_count[player],
					PacketType::mission_result,
					transport.lobby.session_id,
					[&](Writer& writer)
					{
						writer.u8(static_cast<std::uint8_t>(
							MissionResultPayload::
								player_report));
						writer.u8(player);
						writer.u8(report.lobby_slot);
						write_player_mission_report(
							writer, report);
					}))
			{
				return false;
			}
			report_wire_bytes += report_count[player] + 2;
		}
	}
	for (std::uint8_t index = 0;
		index < kMultiplayerTransportPlayerCapacity;
		++index)
	{
		const detail::MultiplayerTransportPeer& peer =
			transport.peers[index];
		if (!peer.occupied || !peer.authenticated)
		{
			continue;
		}
		if (!encode_authority_runtime(
				transport,
				peer,
				runtime_bytes[index],
				runtime_count[index])
			|| snapshot_count + runtime_count[index] + 4
					+ report_wire_bytes
				> detail::kTcpSendBytes - peer.send_count)
		{
			return false;
		}
	}
	for (std::uint8_t index = 0;
		index < kMultiplayerTransportPlayerCapacity;
		++index)
	{
		detail::MultiplayerTransportPeer& peer =
			transport.peers[index];
		if (!peer.occupied || !peer.authenticated)
		{
			continue;
		}
		if (!queue_tcp_payload(
				peer, snapshot_bytes, snapshot_count)
			|| !queue_tcp_payload(
				peer,
				runtime_bytes[index],
				runtime_count[index]))
		{
			return false;
		}
		for (std::uint8_t player = 0;
			player < kMultiplayerTransportPlayerCapacity;
			++player)
		{
			if (report_count[player] != 0
				&& !queue_tcp_payload(
					peer,
					report_bytes[player],
					report_count[player]))
			{
				return false;
			}
		}
	}
	transport.authority_snapshot_dirty = false;
	transport.last_authority_snapshot_at = now;
	return true;
}

bool apply_authority_snapshot(
	MultiplayerTransport& transport,
	Reader& reader)
{
	const MultiplayerTransportState previous_state =
		transport.state;
	const std::uint32_t epoch = reader.u32();
	const std::uint64_t token = reader.u64();
	const std::uint8_t authority_slot = reader.u8();
	const auto phase = static_cast<AuthorityPhase>(reader.u8());
	detail::MultiplayerTransportMember members[
		kMultiplayerTransportPlayerCapacity];
	for (detail::MultiplayerTransportMember& member : members)
	{
		read_member(reader, member);
	}
	MultiplayerLobbySnapshot lobby;
	if (!read_lobby(reader, lobby)
		|| !reader.done()
		|| epoch != transport.authority_epoch
		|| token != transport.authority_token
		|| authority_slot != transport.authority_lobby_slot
		|| phase > AuthorityPhase::post_mission
		|| lobby.session_id != transport.lobby.session_id
		|| transport.local_lobby_slot
			>= kMultiplayerTransportPlayerCapacity
		|| !members[transport.local_lobby_slot].connected
		|| members[transport.local_lobby_slot].identity_token
			!= transport.members[
				transport.local_lobby_slot].identity_token)
	{
		return false;
	}
	detail::MultiplayerTransportPeer& server =
		transport.peers[0];
	if (!server.occupied
		|| !server.authenticated
		|| server.address_be == 0)
	{
		return false;
	}
	// A host cannot discover its own externally observed address. Every
	// client already has that address from the connected socket, so make it
	// canonical before retaining the replicated table.
	members[authority_slot].address_be = server.address_be;
	const bool snapshot_has_launch =
		phase == AuthorityPhase::gameplay
		|| phase == AuthorityPhase::post_mission;
	std::uint8_t snapshot_mapping[
		kMultiplayerTransportPlayerCapacity];
	std::fill(
		std::begin(snapshot_mapping),
		std::end(snapshot_mapping),
		UINT8_MAX);
	std::uint8_t snapshot_player_count = 0;
	if (snapshot_has_launch)
	{
		for (std::uint8_t lobby_slot = 0;
			lobby_slot < kMultiplayerTransportPlayerCapacity;
			++lobby_slot)
		{
			const std::uint8_t gameplay_slot =
				members[lobby_slot].gameplay_slot;
			if (gameplay_slot
				>= kMultiplayerTransportPlayerCapacity)
			{
				continue;
			}
			if (snapshot_mapping[gameplay_slot] != UINT8_MAX)
			{
				return false;
			}
			snapshot_mapping[gameplay_slot] = lobby_slot;
			snapshot_player_count = std::max<std::uint8_t>(
				snapshot_player_count,
				static_cast<std::uint8_t>(
					gameplay_slot + 1));
		}
		for (std::uint8_t player = 0;
			player < snapshot_player_count;
			++player)
		{
			if (snapshot_mapping[player] == UINT8_MAX)
			{
				return false;
			}
		}
	}
	if (!valid_member_table(
			members,
			lobby,
			authority_slot,
			snapshot_has_launch,
			snapshot_mapping,
			snapshot_player_count))
	{
		return false;
	}
	std::copy(
		std::begin(members),
		std::end(members),
		std::begin(transport.members));
	if (transport.state
		== MultiplayerTransportState::post_mission)
	{
		transport.members[
			transport.local_lobby_slot].post_mission = true;
	}
	transport.lobby = lobby;
	if (lobby.rules.mode == MultiplayerSessionMode::deathmatch)
	{
		transport.prelaunch_campaign = {};
	}
	const bool retain_local_post_mission_phase =
		(phase == AuthorityPhase::gameplay
			|| phase == AuthorityPhase::post_mission)
		&& (transport.state
				== MultiplayerTransportState::gameplay
			|| transport.state
				== MultiplayerTransportState::post_mission);
	if (!retain_local_post_mission_phase)
	{
		transport.state =
			transport_state_for_authority_phase(phase, false);
	}
	server.identity_token =
		transport.members[authority_slot].identity_token;
	server.listener_port_be =
		transport.members[authority_slot].listener_port_be;
	server.udp_port_be =
		transport.members[authority_slot].udp_port_be;
	if (previous_state == MultiplayerTransportState::client_lobby
		&& phase == AuthorityPhase::prelaunch)
	{
		// The paired runtime packet carries the campaign/control image. Defer
		// the public transition event until that packet is retained so App can
		// never observe a half-restored prelaunch.
		transport.authority_prelaunch_pending = true;
	}
	else if (phase != AuthorityPhase::prelaunch)
	{
		transport.authority_prelaunch_pending = false;
	}
	MultiplayerTransportEvent event;
	event.kind = MultiplayerTransportEventKind::lobby_updated;
	(void)queue_event(transport, event);
	return true;
}

void read_partial_mission_state(
	Reader& reader,
	MultiplayerMissionResultSnapshot& result)
{
	for (MultiplayerPlayerMissionOutcome& outcome :
		result.players)
	{
		outcome =
			static_cast<MultiplayerPlayerMissionOutcome>(
				reader.u8());
	}
	for (std::int32_t& kills : result.deathmatch_kills)
	{
		kills = reader.i32();
	}
	for (std::int32_t& deaths : result.deathmatch_deaths)
	{
		deaths = reader.i32();
	}
	result.network_aborted = reader.boolean();
}

bool valid_partial_mission_state(
	const MultiplayerMissionResultSnapshot& result,
	const game::MultiplayerLaunchSnapshot& launch,
	bool launch_valid)
{
	for (std::uint8_t player = 0;
		player < kMultiplayerTransportPlayerCapacity;
		++player)
	{
		if (!valid_player_mission_outcome(result.players[player])
			|| (!launch_valid
				&& result.players[player]
					!= MultiplayerPlayerMissionOutcome::none)
			|| (launch_valid
				&& player >= launch.player_count
				&& result.players[player]
					!= MultiplayerPlayerMissionOutcome::none)
			|| (!launch_valid
				&& (result.deathmatch_kills[player] != 0
					|| result.deathmatch_deaths[player] != 0)))
		{
			return false;
		}
	}
	return !result.authoritative_valid;
}

bool apply_authority_runtime(
	MultiplayerTransport& transport,
	Reader& reader)
{
	const bool previously_launched =
		transport.launch_snapshot_valid;
	const game::MultiplayerCampaignLaunchState
		retained_personal_campaign =
			transport.prelaunch_campaign;
	const std::uint32_t epoch = reader.u32();
	const std::uint64_t token = reader.u64();
	const std::uint8_t authority_slot = reader.u8();
	game::MultiplayerCampaignLaunchState campaign_control;
	read_campaign_control(reader, campaign_control);
	const std::uint32_t launch_generation = reader.u32();
	const bool shared_bootstrap_pending = reader.boolean();
	game::MultiplayerMissionBootstrap shared_bootstrap;
	game::MultiplayerMissionBootstrap replay_bootstrap;
	read_mission_bootstrap(reader, shared_bootstrap);
	read_mission_bootstrap(reader, replay_bootstrap);
	game::MultiplayerCampaignLaunchState personal_source =
		retained_personal_campaign;
	const MultiplayerPlayerMissionReport* const local_report =
		mission_report_for_lobby_slot(
			transport.mission_result,
			transport.local_lobby_slot);
	if (!personal_source.present
		&& campaign_control.present
		&& local_report != nullptr
		&& local_report->campaign.present
		&& local_report->campaign.state.mission
			== campaign_control.state.mission)
	{
		personal_source = local_report->campaign;
	}
	game::MultiplayerCampaignLaunchState prelaunch_campaign =
		personal_source;
	const bool campaign_control_installed =
		campaign_control.present
			? install_campaign_control(
				campaign_control,
				transport.lobby.rules,
				personal_source,
				prelaunch_campaign)
			: !campaign_control.mission_25_alternate;
	std::uint8_t mapping[kMultiplayerTransportPlayerCapacity];
	for (std::uint8_t& lobby_slot : mapping)
	{
		lobby_slot = reader.u8();
	}
	const bool launch_valid = reader.boolean();
	game::MultiplayerLaunchSnapshot launch;
	if (launch_valid
		&& !read_launch(
			reader, prelaunch_campaign, launch))
	{
		return false;
	}
	if (launch_valid)
	{
		for (std::uint8_t player = 0;
			player < launch.player_count;
			++player)
		{
			if (mapping[player]
				>= kMultiplayerTransportPlayerCapacity)
			{
				return false;
			}
			for (std::uint8_t earlier = 0;
				earlier < player;
				++earlier)
			{
				if (mapping[earlier] == mapping[player])
				{
					return false;
				}
			}
			launch.players[player].connected =
				transport.members[mapping[player]].connected;
		}
	}
	MultiplayerMissionResultSnapshot partial_result;
	read_partial_mission_state(reader, partial_result);
	const bool deathmatch =
		transport.lobby.rules.mode
			== MultiplayerSessionMode::deathmatch;
	const auto valid_retained_bootstrap =
		[](const game::MultiplayerMissionBootstrap& bootstrap)
		{
			return !bootstrap.present
				|| game::valid_multiplayer_mission_bootstrap(
					bootstrap,
					bootstrap.mission,
					static_cast<std::uint8_t>(
						bootstrap.image.session_state[15]),
					false);
		};
	const bool bootstrap_state_valid =
		deathmatch
			? !shared_bootstrap.present
				&& !replay_bootstrap.present
				&& !shared_bootstrap_pending
				: valid_retained_bootstrap(shared_bootstrap)
					&& valid_retained_bootstrap(replay_bootstrap)
					&& (!shared_bootstrap_pending
						|| (shared_bootstrap.present
							&& shared_bootstrap.mission
								== transport.lobby.rules.mission))
				&& (!launch_valid
					|| (replay_bootstrap.present
						&& mission_bootstrap_equal(
							replay_bootstrap,
							launch.bootstrap)
						&& shared_bootstrap.present
						&& shared_bootstrap
								.launch_generation
							== launch.bootstrap
								.launch_generation
						&& shared_bootstrap.mission
							== launch.bootstrap.mission));
	if (!reader.done()
		|| epoch != transport.authority_epoch
		|| token != transport.authority_token
		|| authority_slot != transport.authority_lobby_slot
		|| !campaign_control_installed
		|| !bootstrap_state_valid
		|| launch_generation < transport.launch_generation
		|| (shared_bootstrap.present
			&& shared_bootstrap.launch_generation
				> launch_generation)
		|| (replay_bootstrap.present
			&& replay_bootstrap.launch_generation
				> launch_generation)
		|| ((transport.state
					== MultiplayerTransportState::client_prelaunch
				|| launch_valid)
			&& (!campaign_control.present
				|| !valid_campaign_launch(
					prelaunch_campaign,
					transport.lobby.rules)))
		|| (launch_valid
			&& !campaign_launch_equal(
				prelaunch_campaign, launch.campaign))
		|| (launch_valid
			&& (launch.role != game::MultiplayerRole::client
				|| launch.local_player
					>= launch.player_count
				|| mapping[launch.local_player]
					!= transport.local_lobby_slot))
		|| (!launch_valid
			&& std::any_of(
				std::begin(mapping),
				std::end(mapping),
				[](std::uint8_t slot)
				{
					return slot != UINT8_MAX;
				}))
		|| !valid_partial_mission_state(
			partial_result,
			launch,
			launch_valid))
	{
		return false;
	}
	if (launch_valid
		&& !valid_member_table(
			transport.members,
			transport.lobby,
			transport.authority_lobby_slot,
			true,
			mapping,
			launch.player_count))
	{
		return false;
	}
	for (std::uint8_t player = 0;
		player < kMultiplayerTransportPlayerCapacity;
		++player)
	{
		const MultiplayerPlayerMissionOutcome retained =
			transport.mission_result.players[player];
		const MultiplayerPlayerMissionOutcome received =
			partial_result.players[player];
		if (retained != MultiplayerPlayerMissionOutcome::none
			&& received != MultiplayerPlayerMissionOutcome::none
			&& retained != received)
		{
			return false;
		}
	}
	transport.prelaunch_campaign = prelaunch_campaign;
	transport.shared_bootstrap = shared_bootstrap;
	transport.replay_bootstrap = replay_bootstrap;
	transport.shared_bootstrap_pending =
		shared_bootstrap_pending;
	transport.launch_generation = launch_generation;
	std::copy(
		std::begin(mapping),
		std::end(mapping),
		std::begin(transport.gameplay_lobby_slot));
	transport.launch_snapshot = launch;
	transport.launch_snapshot_valid = launch_valid;
	transport.local_gameplay_player = launch_valid
		? launch.local_player
		: UINT8_MAX;
	for (std::uint8_t player = 0;
		player < kMultiplayerTransportPlayerCapacity;
		++player)
	{
		if (partial_result.players[player]
			!= MultiplayerPlayerMissionOutcome::none)
		{
			transport.mission_result.players[player] =
				partial_result.players[player];
		}
		transport.mission_result.deathmatch_kills[player] =
			partial_result.deathmatch_kills[player];
		transport.mission_result.deathmatch_deaths[player] =
			partial_result.deathmatch_deaths[player];
	}
	transport.mission_result.network_aborted =
		transport.mission_result.network_aborted
		|| partial_result.network_aborted;
	synchronize_member_gameplay_slots(transport);
	refresh_debrief_leader(transport);
	if (transport.authority_prelaunch_pending)
	{
		if (launch_valid
			|| transport.state
				!= MultiplayerTransportState::client_prelaunch)
		{
			return false;
		}
		transport.authority_prelaunch_pending = false;
		MultiplayerTransportEvent event;
		event.kind = MultiplayerTransportEventKind::prelaunch;
		event.player = transport.local_lobby_slot;
		(void)queue_event(transport, event);
	}
	if (launch_valid && !previously_launched)
	{
		MultiplayerTransportEvent event;
		event.kind = MultiplayerTransportEventKind::launch;
		event.player = launch.local_player;
		(void)queue_event(transport, event);
	}
	return true;
}

bool broadcast_prelaunch(
	MultiplayerTransport& transport,
	const MultiplayerLobbySnapshot& lobby,
	const game::MultiplayerCampaignLaunchState& campaign)
{
	return broadcast_tcp_packet(
		transport,
		PacketType::prelaunch,
		[&](Writer& writer)
		{
			write_lobby(writer, lobby);
			write_campaign_control(writer, campaign);
		},
		[](const detail::MultiplayerTransportPeer&)
		{
			return true;
		});
}

bool broadcast_player_mission_outcome(
	MultiplayerTransport& transport,
	std::uint8_t gameplay_player,
	std::uint8_t lobby_slot,
	MultiplayerPlayerMissionOutcome outcome,
	std::uint8_t excluded_gameplay_player)
{
	return broadcast_tcp_packet(
		transport,
		PacketType::player_mission_outcome,
		[&](Writer& writer)
		{
			writer.u8(gameplay_player);
			writer.u8(lobby_slot);
			writer.u8(static_cast<std::uint8_t>(outcome));
		},
		[&](const detail::MultiplayerTransportPeer& peer)
		{
			return peer.gameplay_slot
				!= excluded_gameplay_player;
		});
}

void apply_player_mission_outcome(
	MultiplayerTransport& transport,
	std::uint8_t gameplay_player,
	std::uint8_t lobby_slot,
	MultiplayerPlayerMissionOutcome outcome);
bool valid_player_outcome_identity(
	const MultiplayerTransport& transport,
	std::uint8_t gameplay_player,
	std::uint8_t lobby_slot);

bool player_mission_report_equal(
	const MultiplayerPlayerMissionReport& left,
	const MultiplayerPlayerMissionReport& right)
{
	if (left.valid != right.valid
		|| left.lobby_slot != right.lobby_slot)
	{
		return false;
	}
	std::uint8_t left_bytes[detail::kWirePacketBytes];
	std::uint8_t right_bytes[detail::kWirePacketBytes];
	Writer left_writer{left_bytes, sizeof(left_bytes)};
	Writer right_writer{right_bytes, sizeof(right_bytes)};
	write_player_mission_report(left_writer, left);
	write_player_mission_report(right_writer, right);
	return left_writer.valid
		&& right_writer.valid
		&& left_writer.count == right_writer.count
		&& std::memcmp(
			left_bytes, right_bytes, left_writer.count) == 0;
}

bool broadcast_player_mission_report(
	MultiplayerTransport& transport,
	std::uint8_t gameplay_player,
	std::uint8_t lobby_slot,
	const MultiplayerPlayerMissionReport& report,
	std::uint8_t excluded_gameplay_player)
{
	return broadcast_tcp_packet(
		transport,
		PacketType::mission_result,
		[&](Writer& writer)
		{
			writer.u8(static_cast<std::uint8_t>(
				MissionResultPayload::player_report));
			writer.u8(gameplay_player);
			writer.u8(lobby_slot);
			write_player_mission_report(writer, report);
		},
		[&](const detail::MultiplayerTransportPeer& peer)
		{
			return peer.gameplay_slot
				!= excluded_gameplay_player;
		});
}

bool apply_player_mission_report(
	MultiplayerTransport& transport,
	std::uint8_t gameplay_player,
	std::uint8_t lobby_slot,
	const MultiplayerPlayerMissionReport& received)
{
	if (!valid_player_outcome_identity(
			transport, gameplay_player, lobby_slot)
		|| !valid_player_mission_report(
			received, transport.launch_snapshot))
	{
		return false;
	}
	MultiplayerPlayerMissionReport report = received;
	report.lobby_slot = lobby_slot;
	MultiplayerPlayerMissionReport& retained =
		transport.mission_result.reports[gameplay_player];
	if (retained.valid)
	{
		return player_mission_report_equal(
			retained, report);
	}
	const MultiplayerPlayerMissionOutcome current =
		transport.mission_result.players[gameplay_player];
	if (current != MultiplayerPlayerMissionOutcome::none
		&& current != report.outcome)
	{
		return false;
	}
	retained = report;
	if (gameplay_player == transport.local_gameplay_player)
	{
		// Additive compatibility view: always this process's own report.
		transport.mission_result.authoritative = report.result;
		transport.mission_result.campaign = report.campaign;
		transport.mission_result.advance = report.advance;
		transport.mission_result.authoritative_valid = true;
	}
	if (current == MultiplayerPlayerMissionOutcome::none)
	{
		apply_player_mission_outcome(
			transport,
			gameplay_player,
			lobby_slot,
			report.outcome);
	}
	else
	{
		// A separately published outcome or a migration runtime snapshot can
		// arrive before the owning peer's full report. The report supplies
		// mission-25's direct-alternate distinction, so refresh leadership
		// after retaining it even when the outcome itself was already known.
		refresh_debrief_leader(transport);
	}
	MultiplayerTransportEvent event;
	event.kind =
		MultiplayerTransportEventKind::mission_result;
	event.player = gameplay_player;
	(void)queue_event(transport, event);
	mark_authority_snapshot_dirty(transport);
	return true;
}

void apply_player_mission_outcome(
	MultiplayerTransport& transport,
	std::uint8_t gameplay_player,
	std::uint8_t lobby_slot,
	MultiplayerPlayerMissionOutcome outcome)
{
	transport.mission_result.players[gameplay_player] = outcome;
	if (lobby_slot < kMultiplayerTransportPlayerCapacity
		&& transport.lobby.players[lobby_slot].connected)
	{
		transport.lobby.players[
			lobby_slot].outcome_requires_restart =
			outcome_requires_restart(outcome);
	}
	refresh_debrief_leader(transport);
	MultiplayerTransportEvent event;
	event.kind =
		MultiplayerTransportEventKind::player_mission_outcome;
	event.player = gameplay_player;
	event.mission_outcome = outcome;
	(void)queue_event(transport, event);
	mark_authority_snapshot_dirty(transport);
}

bool valid_player_outcome_identity(
	const MultiplayerTransport& transport,
	std::uint8_t gameplay_player,
	std::uint8_t lobby_slot)
{
	if (!transport.launch_snapshot_valid
		|| gameplay_player
			>= transport.launch_snapshot.player_count
		|| lobby_slot >= kMultiplayerTransportPlayerCapacity)
	{
		return false;
	}
	return transport.gameplay_lobby_slot[gameplay_player]
		== lobby_slot;
}

bool build_gameplay_lobby_mapping(
	const MultiplayerLobbySnapshot& lobby,
	std::uint8_t expected_player_count,
	std::uint8_t (&mapping)[
		kMultiplayerTransportPlayerCapacity])
{
	std::fill(
		std::begin(mapping),
		std::end(mapping),
		UINT8_MAX);
	std::uint8_t gameplay_player = 0;
	for (std::uint8_t lobby_slot = 0;
		lobby_slot < kMultiplayerTransportPlayerCapacity;
		++lobby_slot)
	{
		if (!lobby.players[lobby_slot].connected)
		{
			continue;
		}
		if (gameplay_player
			>= kMultiplayerTransportPlayerCapacity)
		{
			return false;
		}
		mapping[gameplay_player++] = lobby_slot;
	}
	return gameplay_player == expected_player_count;
}

bool assign_gameplay_lobby_mapping(
	MultiplayerTransport& transport)
{
	return transport.launch_snapshot_valid
		&& build_gameplay_lobby_mapping(
			transport.lobby,
			transport.launch_snapshot.player_count,
			transport.gameplay_lobby_slot);
}

bool post_mission_action_allowed(
	const MultiplayerTransport& transport,
	std::uint8_t actor,
	MultiplayerPostMissionAction action,
	bool require_post_mission_state = true)
{
	if ((require_post_mission_state
			&& transport.state
				!= MultiplayerTransportState::post_mission)
		|| (!require_post_mission_state
			&& transport.state
				!= MultiplayerTransportState::gameplay
			&& transport.state
				!= MultiplayerTransportState::post_mission)
		|| !transport.launch_snapshot_valid
		|| actor >= transport.launch_snapshot.player_count
		|| !transport.mission_result.reports[actor].valid
		|| actor
			!= transport.mission_result.debrief_leader_player
		|| (action != MultiplayerPostMissionAction::replay
			&& action
				!= MultiplayerPostMissionAction::continue_campaign)
		|| (action
				== MultiplayerPostMissionAction::continue_campaign
			&& !transport.mission_result.debrief_leader_valid))
	{
		return false;
	}
	if (action == MultiplayerPostMissionAction::replay)
	{
		if (!game::valid_multiplayer_mission_bootstrap(
				transport.replay_bootstrap,
				transport.launch_snapshot.authoritative_mission,
				transport.lobby.player_count,
				false))
		{
			return false;
		}
	}
	else if (!game::valid_multiplayer_mission_bootstrap(
			transport.shared_bootstrap,
			transport.launch_snapshot.authoritative_mission,
			transport.launch_snapshot.player_count,
			false)
		|| transport.mission_result.reports[
				actor].campaign.state.mission == 0
		|| transport.mission_result.reports[
				actor].campaign.state.mission
			> campaign::kMissionCount
		|| transport.launch_generation == UINT32_MAX)
	{
		return false;
	}
	if (transport.host)
	{
		for (std::uint8_t lobby_slot = 0;
			lobby_slot < kMultiplayerTransportPlayerCapacity;
			++lobby_slot)
		{
			if (lobby_slot != transport.local_lobby_slot
				&& transport.members[lobby_slot].connected
				&& peer_for_lobby_slot(
					transport, lobby_slot) == nullptr)
			{
				return false;
			}
		}
	}
	for (std::uint8_t gameplay_player = 0;
		gameplay_player
			< transport.launch_snapshot.player_count;
		++gameplay_player)
	{
		if (gameplay_player == actor
			|| !transport.launch_snapshot.players[
				gameplay_player].connected)
		{
			continue;
		}
		const std::uint8_t lobby_slot =
			transport.gameplay_lobby_slot[gameplay_player];
		if (lobby_slot >= kMultiplayerTransportPlayerCapacity
			|| !transport.lobby.players[lobby_slot].connected)
		{
			return false;
		}
		const MultiplayerLobbyPlayer& player =
			transport.lobby.players[lobby_slot];
		if (action == MultiplayerPostMissionAction::replay)
		{
			if (!player.post_mission_ready)
			{
				return false;
			}
		}
		else if (!player.outcome_requires_restart
			&& !player.post_mission_ready)
		{
			return false;
		}
	}
	return true;
}

bool broadcast_post_mission_action(
	MultiplayerTransport& transport,
	std::uint8_t actor,
	MultiplayerPostMissionAction action)
{
	return broadcast_tcp_packet(
		transport,
		PacketType::post_mission_action,
		[&](Writer& writer)
		{
			writer.u8(actor);
			writer.u8(static_cast<std::uint8_t>(action));
		},
		[](const detail::MultiplayerTransportPeer&)
		{
			return true;
		});
}

bool apply_post_mission_action(
	MultiplayerTransport& transport,
	std::uint8_t actor,
	MultiplayerPostMissionAction action)
{
	// The action packet is the synchronization point for the following
	// cooperative launch. Replay keeps the just-completed mission. Continue
	// takes only the elected actor's control mission; every peer retains its
	// own resulting campaign/profile block.
	game::MultiplayerMissionBootstrap next_bootstrap;
	game::MultiplayerCampaignLaunchState next_personal_campaign =
		transport.launch_snapshot.campaign;
	std::uint16_t next_mission =
		transport.launch_snapshot.authoritative_mission;
	if (action == MultiplayerPostMissionAction::replay)
	{
		next_bootstrap = transport.replay_bootstrap;
	}
	else if (action
		== MultiplayerPostMissionAction::continue_campaign)
	{
		const MultiplayerPlayerMissionReport& actor_report =
			transport.mission_result.reports[actor];
		next_mission =
			actor_report.campaign.state.mission;
		if (!materialize_mission_bootstrap(
				transport,
				actor_report.campaign,
				next_mission,
				&transport.shared_bootstrap,
				next_bootstrap))
		{
			return false;
		}
		transport.lobby.coop_selection = {};
		const MultiplayerPlayerMissionReport* const local_report =
			mission_report_for_lobby_slot(
				transport.mission_result,
				transport.local_lobby_slot);
		if (local_report != nullptr)
		{
			next_personal_campaign =
				local_report->campaign;
		}
		next_personal_campaign.present = true;
		next_personal_campaign.mission_25_alternate = false;
		next_personal_campaign.state.mission =
			next_mission;
		next_personal_campaign.state.difficulty =
			actor_report.campaign.state.difficulty;
	}
	transport.lobby.rules.mission = next_mission;
	transport.prelaunch_campaign = next_personal_campaign;
	transport.shared_bootstrap = next_bootstrap;
	transport.shared_bootstrap_pending = true;
	transport.launch_generation = std::max(
		transport.launch_generation,
		next_bootstrap.launch_generation);
	MultiplayerTransportEvent event;
	event.kind =
		MultiplayerTransportEventKind::post_mission_action;
	event.player = actor;
	event.post_mission_action = action;
	(void)queue_event(transport, event);
	for (MultiplayerLobbyPlayer& player :
		transport.lobby.players)
	{
		player.post_mission_ready = false;
		player.outcome_requires_restart = false;
	}
	for (detail::MultiplayerTransportPeer& peer :
		transport.peers)
	{
		peer.post_mission = false;
	}
	for (detail::MultiplayerTransportMember& member :
		transport.members)
	{
		member.post_mission = false;
	}
	transport.state = transport.host
		? MultiplayerTransportState::hosting_lobby
		: MultiplayerTransportState::client_lobby;
	transport.launch_snapshot_valid = false;
	transport.local_gameplay_player = UINT8_MAX;
	std::fill(
		std::begin(transport.gameplay_lobby_slot),
		std::end(transport.gameplay_lobby_slot),
		UINT8_MAX);
	synchronize_member_gameplay_slots(transport);
	mark_authority_snapshot_dirty(transport);
	return true;
}

bool queue_welcome(
	MultiplayerTransport& transport,
	detail::MultiplayerTransportPeer& peer)
{
	return queue_tcp_packet(
		peer,
		PacketType::server_welcome,
		transport.lobby.session_id,
		[&](Writer& writer)
		{
			writer.u8(peer.lobby_slot);
			writer.u64(peer.authentication_token);
			writer.u64(peer.identity_token);
			writer.u16(transport.session_port);
			writer.u32(transport.authority_epoch);
			writer.u64(transport.authority_token);
			writer.u8(transport.authority_lobby_slot);
			write_lobby(writer, transport.lobby);
		});
}

bool queue_hello(
	MultiplayerTransport& transport,
	detail::MultiplayerTransportPeer& peer)
{
	if (!queue_tcp_packet(
			peer,
			PacketType::client_hello,
			transport.expected_session_id,
			[&](Writer& writer)
			{
				write_player(
					writer, transport.pending_local_player);
				writer.u16(transport.local_udp_port);
				writer.u16(transport.local_listener_port);
				writer.u64(transport.local_identity_token);
			}))
	{
		return false;
	}
	return true;
}

bool queue_migration_hello(
	MultiplayerTransport& transport,
	detail::MultiplayerTransportPeer& peer)
{
	if (transport.local_lobby_slot
			>= kMultiplayerTransportPlayerCapacity
		|| transport.previous_authority_epoch == 0
		|| transport.previous_authority_token == 0)
	{
		return false;
	}
	return queue_tcp_packet(
		peer,
		PacketType::migration_hello,
		transport.lobby.session_id,
		[&](Writer& writer)
		{
			writer.u8(transport.local_lobby_slot);
			writer.u64(transport.local_identity_token);
			writer.u32(transport.previous_authority_epoch);
			writer.u64(transport.previous_authority_token);
			writer.u16(transport.local_udp_port);
			writer.u16(transport.local_listener_port);
			writer.boolean(
				transport.state
					== MultiplayerTransportState::post_mission);
			writer.boolean(
				transport.state
						== MultiplayerTransportState::post_mission
					&& transport.lobby.players[
						transport.local_lobby_slot]
							.post_mission_ready);
		});
}

bool queue_migration_welcome(
	MultiplayerTransport& transport,
	detail::MultiplayerTransportPeer& peer)
{
	return queue_tcp_packet(
		peer,
		PacketType::migration_welcome,
		transport.lobby.session_id,
		[&](Writer& writer)
		{
			writer.u8(peer.lobby_slot);
			writer.u64(peer.authentication_token);
			writer.u64(peer.identity_token);
			writer.u16(transport.local_udp_port);
			writer.u32(transport.authority_epoch);
			writer.u64(transport.authority_token);
			writer.u8(transport.authority_lobby_slot);
		});
}

bool queue_gameplay_tcp(
	detail::MultiplayerTransportPeer& peer,
	const MultiplayerSessionId& session_id,
	const mission::NetworkOutboundMessage& message)
{
	return queue_tcp_packet(
		peer,
		PacketType::gameplay,
		session_id,
		[&](Writer& writer)
		{
			write_semantic_message(writer, message);
		});
}

bool queue_post_chat_tcp(
	detail::MultiplayerTransportPeer& peer,
	const MultiplayerSessionId& session_id,
	std::uint8_t source,
	const char* text)
{
	char bounded[mission::kNetworkChatBytes]{};
	if (!copy_text(bounded, text))
	{
		return false;
	}
	return queue_tcp_packet(
		peer,
		PacketType::post_mission_chat,
		session_id,
		[&](Writer& writer)
		{
			writer.u8(source);
			writer.text(bounded);
		});
}

bool queue_lobby_chat_tcp(
	detail::MultiplayerTransportPeer& peer,
	const MultiplayerSessionId& session_id,
	std::uint8_t source,
	const char* text)
{
	char bounded[mission::kNetworkChatBytes]{};
	if (!copy_text(bounded, text))
	{
		return false;
	}
	return queue_tcp_packet(
		peer,
		PacketType::lobby_chat,
		session_id,
		[&](Writer& writer)
		{
			writer.u8(source);
			writer.text(bounded);
		});
}

void queue_udp_probe(
	MultiplayerTransport& transport,
	detail::MultiplayerTransportPeer& server)
{
	++server.udp_send_sequence;
	(void)queue_udp_packet(
		transport,
		PacketType::udp_probe,
		transport.lobby.session_id,
		server.address_be,
		server.udp_port_be,
		[&](Writer& writer)
		{
			writer.u8(transport.local_lobby_slot);
			writer.u64(server.authentication_token);
			writer.u32(server.udp_send_sequence);
		});
}

void queue_udp_probe_ack(
	MultiplayerTransport& transport,
	detail::MultiplayerTransportPeer& peer)
{
	++peer.udp_send_sequence;
	(void)queue_udp_packet(
		transport,
		PacketType::udp_probe_ack,
		transport.lobby.session_id,
		peer.address_be,
		peer.udp_port_be,
		[&](Writer& writer)
		{
			writer.u8(peer.lobby_slot);
			writer.u64(peer.authentication_token);
			writer.u32(peer.udp_send_sequence);
		});
}

bool queue_gameplay_udp(
	MultiplayerTransport& transport,
	detail::MultiplayerTransportPeer& peer,
	const mission::NetworkOutboundMessage& message)
{
	if (!peer.udp_endpoint_known)
	{
		return false;
	}
	++peer.udp_send_sequence;
	return queue_udp_packet(
		transport,
		PacketType::gameplay,
		transport.lobby.session_id,
		peer.address_be,
		peer.udp_port_be,
		[&](Writer& writer)
		{
			writer.u8(
				transport.host
					? peer.lobby_slot
					: transport.local_lobby_slot);
			writer.u64(peer.authentication_token);
			writer.u32(peer.udp_send_sequence);
			write_semantic_message(writer, message);
		});
}

bool relay_gameplay_from_host(
	MultiplayerTransport& transport,
	const mission::NetworkOutboundMessage& message,
	std::uint8_t exclude_gameplay_player)
{
	if (conditional_delivery(message.delivery))
	{
		for (detail::MultiplayerTransportPeer& peer :
			transport.peers)
		{
			if (!peer.occupied
				|| !peer.authenticated
				|| peer.gameplay_slot
					== exclude_gameplay_player
				|| (message.destination_player
						!= kBroadcastPlayer
					&& message.destination_player
						!= peer.gameplay_slot))
			{
				continue;
			}
			// Conditional retail traffic is intentionally droppable. A
			// missing endpoint or full datagram ring consumes this
			// publication rather than stalling the simulation publisher.
			(void)queue_gameplay_udp(
				transport, peer, message);
		}
		return true;
	}

	for (std::uint8_t gameplay_player = 0;
		transport.launch_snapshot_valid
			&& gameplay_player
				< transport.launch_snapshot.player_count;
		++gameplay_player)
	{
		if (gameplay_player == exclude_gameplay_player
			|| !transport.launch_snapshot.players[
				gameplay_player].connected
			|| (message.destination_player
					!= kBroadcastPlayer
				&& message.destination_player
					!= gameplay_player))
		{
			continue;
		}
		const std::uint8_t lobby_slot =
			transport.gameplay_lobby_slot[gameplay_player];
		if (lobby_slot == transport.local_lobby_slot)
		{
			continue;
		}
		if (peer_for_lobby_slot(
				transport, lobby_slot) == nullptr)
		{
			return false;
		}
	}

	std::uint8_t bytes[detail::kWirePacketBytes];
	std::size_t count = 0;
	if (!encode_packet(
			bytes,
			count,
			PacketType::gameplay,
			transport.lobby.session_id,
			[&](Writer& writer)
			{
				write_semantic_message(writer, message);
			}))
	{
		return false;
	}
	for (const detail::MultiplayerTransportPeer& peer :
		transport.peers)
	{
		if (!peer.occupied
			|| !peer.authenticated
			|| peer.gameplay_slot == exclude_gameplay_player
			|| (message.destination_player
					!= kBroadcastPlayer
				&& message.destination_player
					!= peer.gameplay_slot))
		{
			continue;
		}
		if (!can_queue_tcp_payload(peer, count))
		{
			return false;
		}
	}
	for (detail::MultiplayerTransportPeer& peer :
		transport.peers)
	{
		if (!peer.occupied
			|| !peer.authenticated
			|| peer.gameplay_slot == exclude_gameplay_player
			|| (message.destination_player
					!= kBroadcastPlayer
				&& message.destination_player
					!= peer.gameplay_slot))
		{
			continue;
		}
		if (!queue_tcp_payload(peer, bytes, count))
		{
			return false;
		}
	}
	return true;
}

void publish_departure(
	MultiplayerTransport& transport,
	std::uint8_t gameplay_player,
	std::uint8_t exclude_gameplay_player)
{
	if (!transport.launch_snapshot_valid
		|| gameplay_player
			>= transport.launch_snapshot.player_count)
	{
		return;
	}
	mission::NetworkOutboundMessage message;
	message.kind = mission::NetworkOutboundKind::gameplay;
	message.opcode =
		mission::NetworkGameplayOpcode::player_departure;
	message.delivery =
		mission::NetworkDelivery::broadcast_guaranteed;
	message.source_player = gameplay_player;
	message.destination_player = kBroadcastPlayer;
	message.departure_player = gameplay_player;
	(void)queue_gameplay(transport, message);
	(void)relay_gameplay_from_host(
		transport, message, exclude_gameplay_player);
}

void queue_peer_departure_control(
	MultiplayerTransport& transport,
	std::uint8_t lobby_slot,
	std::uint8_t gameplay_slot,
	MultiplayerDepartureReason reason)
{
	for (detail::MultiplayerTransportPeer& peer :
		transport.peers)
	{
		if (!peer.occupied
			|| !peer.authenticated
			|| peer.lobby_slot == lobby_slot)
		{
			continue;
		}
		(void)queue_tcp_packet(
			peer,
			PacketType::peer_departure,
			transport.lobby.session_id,
			[&](Writer& writer)
			{
				writer.u8(lobby_slot);
				writer.u8(gameplay_slot);
				writer.u8(static_cast<std::uint8_t>(reason));
			});
	}
}

void remove_host_peer(
	MultiplayerTransport& transport,
	std::uint8_t peer_index,
	MultiplayerDepartureReason reason)
{
	if (peer_index >= kMultiplayerTransportPlayerCapacity)
	{
		return;
	}
	detail::MultiplayerTransportPeer& peer =
		transport.peers[peer_index];
	if (!peer.occupied)
	{
		return;
	}
	const bool authenticated = peer.authenticated;
	const bool already_published = peer.departure_published;
	const std::uint8_t lobby_slot = peer.lobby_slot;
	const std::uint8_t gameplay_slot = peer.gameplay_slot;
	close_peer(peer);
	if (!authenticated
		|| lobby_slot >= kMultiplayerTransportPlayerCapacity)
	{
		return;
	}
	transport.lobby.players[lobby_slot] = {};
	transport.lobby.players[lobby_slot].selected_ship = -1;
	transport.lobby.players[lobby_slot].team = -1;
	transport.members[lobby_slot].connected = false;
	transport.members[lobby_slot].post_mission = false;
	transport.lobby.player_count =
		connected_player_count(transport.lobby);
	if (!already_published
		&& gameplay_slot < kMultiplayerTransportPlayerCapacity)
	{
		publish_departure(
			transport, gameplay_slot, gameplay_slot);
	}
	if (transport.launch_snapshot_valid
		&& gameplay_slot
			< transport.launch_snapshot.player_count
		&& !transport.launch_snapshot.deathmatch_mode
		&& transport.mission_result.players[gameplay_slot]
			== MultiplayerPlayerMissionOutcome::none)
	{
		const MultiplayerPlayerMissionOutcome outcome =
			reason == MultiplayerDepartureReason::kicked
				? MultiplayerPlayerMissionOutcome::kicked
				: MultiplayerPlayerMissionOutcome::departed;
		(void)broadcast_player_mission_outcome(
			transport,
			gameplay_slot,
			lobby_slot,
			outcome,
			gameplay_slot);
		apply_player_mission_outcome(
			transport,
			gameplay_slot,
			lobby_slot,
			outcome);
	}
	if (transport.launch_snapshot_valid
		&& gameplay_slot
			< transport.launch_snapshot.player_count)
	{
		transport.launch_snapshot.players[
			gameplay_slot].connected = false;
		if (!transport.launch_snapshot.deathmatch_mode
			&& !has_direct_mission_25_report(
				transport.mission_result))
		{
			const DebriefLeader leader =
				compute_debrief_leader(
					transport.mission_result,
					transport.launch_snapshot);
			transport.mission_result.debrief_leader_player =
				leader.player;
			transport.mission_result.debrief_leader_valid =
				leader.valid;
		}
	}
	queue_peer_departure_control(
		transport, lobby_slot, gameplay_slot, reason);
	MultiplayerTransportEvent event;
	event.kind = MultiplayerTransportEventKind::peer_departed;
	event.player = gameplay_slot
		< kMultiplayerTransportPlayerCapacity
			? gameplay_slot
			: lobby_slot;
	event.departure_reason = reason;
	(void)queue_event(transport, event);
	(void)broadcast_lobby(transport);
	mark_authority_snapshot_dirty(transport);
}

std::uint8_t elect_authority_successor(
	const MultiplayerTransport& transport,
	std::uint8_t excluded_mask)
{
	if (transport.launch_snapshot_valid)
	{
		for (std::uint8_t gameplay_player = 0;
			gameplay_player
				< transport.launch_snapshot.player_count;
			++gameplay_player)
		{
			const std::uint8_t lobby_slot =
				transport.gameplay_lobby_slot[gameplay_player];
			if (lobby_slot
					< kMultiplayerTransportPlayerCapacity
				&& (excluded_mask & (1u << lobby_slot)) == 0
				&& transport.members[lobby_slot].connected)
			{
				return lobby_slot;
			}
		}
	}
	for (std::uint8_t lobby_slot = 0;
		lobby_slot < kMultiplayerTransportPlayerCapacity;
		++lobby_slot)
	{
		if ((excluded_mask & (1u << lobby_slot)) == 0
			&& transport.members[lobby_slot].connected)
		{
			return lobby_slot;
		}
	}
	return UINT8_MAX;
}

void queue_local_departure_message(
	MultiplayerTransport& transport,
	std::uint8_t gameplay_player)
{
	if (!transport.launch_snapshot_valid
		|| gameplay_player
			>= transport.launch_snapshot.player_count)
	{
		return;
	}
	mission::NetworkOutboundMessage message;
	message.kind = mission::NetworkOutboundKind::gameplay;
	message.opcode =
		mission::NetworkGameplayOpcode::player_departure;
	message.delivery =
		mission::NetworkDelivery::broadcast_guaranteed;
	message.source_player = gameplay_player;
	message.destination_player = kBroadcastPlayer;
	message.departure_player = gameplay_player;
	(void)queue_gameplay(transport, message);
}

std::uint8_t retire_member_locally(
	MultiplayerTransport& transport,
	std::uint8_t lobby_slot,
	MultiplayerDepartureReason reason)
{
	if (lobby_slot >= kMultiplayerTransportPlayerCapacity
		|| !transport.members[lobby_slot].connected)
	{
		return UINT8_MAX;
	}
	const std::uint8_t gameplay_slot =
		transport.members[lobby_slot].gameplay_slot;
	if (transport.launch_snapshot_valid
		&& gameplay_slot
			< transport.launch_snapshot.player_count)
	{
		queue_local_departure_message(
			transport, gameplay_slot);
		if (!transport.launch_snapshot.deathmatch_mode
			&& transport.mission_result.players[gameplay_slot]
				== MultiplayerPlayerMissionOutcome::none)
		{
			apply_player_mission_outcome(
				transport,
				gameplay_slot,
				lobby_slot,
				reason == MultiplayerDepartureReason::kicked
					? MultiplayerPlayerMissionOutcome::kicked
					: MultiplayerPlayerMissionOutcome::departed);
		}
		transport.launch_snapshot.players[
			gameplay_slot].connected = false;
		if (!transport.launch_snapshot.deathmatch_mode
			&& !has_direct_mission_25_report(
				transport.mission_result))
		{
			const DebriefLeader leader =
				compute_debrief_leader(
					transport.mission_result,
					transport.launch_snapshot);
			transport.mission_result.debrief_leader_player =
				leader.player;
			transport.mission_result.debrief_leader_valid =
				leader.valid;
		}
	}
	transport.members[lobby_slot].connected = false;
	transport.members[lobby_slot].post_mission = false;
	transport.lobby.players[lobby_slot] = {};
	transport.lobby.players[lobby_slot].selected_ship = -1;
	transport.lobby.players[lobby_slot].team = -1;
	transport.lobby.player_count =
		connected_player_count(transport.lobby);
	MultiplayerTransportEvent event;
	event.kind =
		MultiplayerTransportEventKind::peer_departed;
	event.player = gameplay_slot
			< kMultiplayerTransportPlayerCapacity
		? gameplay_slot
		: lobby_slot;
	event.departure_reason = reason;
	(void)queue_event(transport, event);
	return gameplay_slot;
}

void queue_authority_changed_event(
	MultiplayerTransport& transport)
{
	MultiplayerTransportEvent event;
	event.kind =
		MultiplayerTransportEventKind::authority_changed;
	event.authority_epoch = transport.authority_epoch;
	const std::uint8_t authority_gameplay =
		authority_gameplay_player(transport);
	event.player = authority_gameplay
			< kMultiplayerTransportPlayerCapacity
		? authority_gameplay
		: transport.authority_lobby_slot;
	(void)queue_event(transport, event);
}

bool begin_migration_connection(
	MultiplayerTransport& transport,
	std::uint8_t authority_slot,
	std::uint64_t now)
{
	if (authority_slot
			>= kMultiplayerTransportPlayerCapacity
		|| authority_slot == transport.local_lobby_slot
		|| !transport.members[authority_slot].connected)
	{
		return false;
	}
	const detail::MultiplayerTransportMember& authority =
		transport.members[authority_slot];
	if (authority.address_be == 0
		|| authority.listener_port_be == 0
		|| authority.udp_port_be == 0)
	{
		return false;
	}
	close_peer(transport.peers[0]);
	const NativeSocket socket =
		::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (socket == kInvalidNativeSocket
		|| !prepare_stream_socket(socket))
	{
		close_native_socket(socket);
		return false;
	}
	sockaddr_in remote{};
	remote.sin_family = AF_INET;
	remote.sin_addr.s_addr = authority.address_be;
	remote.sin_port = authority.listener_port_be;
	const int result = connect(
		socket,
		reinterpret_cast<const sockaddr*>(&remote),
		static_cast<SocketLength>(sizeof(remote)));
	const bool connecting =
		result != 0 && socket_would_block(socket_error());
	if (result != 0 && !connecting)
	{
		close_native_socket(socket);
		return false;
	}
	detail::MultiplayerTransportPeer& server =
		transport.peers[0];
	server = {};
	server.socket = public_socket(socket);
	server.occupied = true;
	server.connecting = connecting;
	server.migration_connection = true;
	server.address_be = authority.address_be;
	server.listener_port_be = authority.listener_port_be;
	server.udp_port_be = authority.udp_port_be;
	server.last_receive_at = now;
	server.last_send_at = now;
	transport.migration_target_slot = authority_slot;
	transport.migration_started_at = now;
	if (!connecting
		&& !queue_migration_hello(transport, server))
	{
		close_peer(server);
		return false;
	}
	return true;
}

bool assume_migrated_authority(
	MultiplayerTransport& transport,
	std::uint64_t now)
{
	if (transport.local_lobby_slot
			>= kMultiplayerTransportPlayerCapacity
		|| !transport.members[
			transport.local_lobby_slot].connected
		|| transport.listen_socket
			== detail::kInvalidSocketHandle
		|| transport.udp_socket
			== detail::kInvalidSocketHandle)
	{
		return false;
	}
	for (detail::MultiplayerTransportPeer& peer : transport.peers)
	{
		close_peer(peer);
	}
	AuthorityPhase phase;
	if (!authority_phase(transport, phase))
	{
		return false;
	}
	transport.host = true;
	transport.authority_epoch =
		transport.previous_authority_epoch + 1u;
	if (transport.authority_epoch == 0)
	{
		transport.authority_epoch = 1;
	}
	transport.authority_token = random_u64();
	transport.authority_lobby_slot =
		transport.local_lobby_slot;
	transport.migration_target_slot =
		transport.local_lobby_slot;
	transport.lobby.leader_slot =
		transport.local_lobby_slot;
	transport.lobby.players[
		transport.local_lobby_slot].latency = 0;
	transport.lobby.players[
		transport.local_lobby_slot].one_way_latency = 0;
	transport.state =
		transport_state_for_authority_phase(phase, true);
	transport.session_port = transport.local_listener_port;
	transport.next_latency_smoothing_at = 0;
	if (transport.launch_snapshot_valid)
	{
		transport.launch_snapshot.role =
			game::MultiplayerRole::host;
		transport.launch_snapshot.local_player =
			transport.local_gameplay_player;
		if (transport.local_gameplay_player
			< transport.launch_snapshot.player_count)
		{
			game::MultiplayerPlayerLaunch& local_launch =
				transport.launch_snapshot.players[
					transport.local_gameplay_player];
			local_launch.latency = 0;
			local_launch.one_way_latency = 0;
		}
	}
	detail::MultiplayerTransportMember& local =
		transport.members[transport.local_lobby_slot];
	local.identity_token = transport.local_identity_token;
	local.listener_port_be =
		htons(transport.local_listener_port);
	local.udp_port_be = htons(transport.local_udp_port);
	local.connected = true;
	transport.migration_pending_mask = 0;
	for (std::uint8_t slot = 0;
		slot < kMultiplayerTransportPlayerCapacity;
		++slot)
	{
		if (slot != transport.local_lobby_slot
			&& transport.members[slot].connected)
		{
			transport.migration_pending_mask |=
				static_cast<std::uint8_t>(1u << slot);
		}
	}
	transport.migration_in_progress = true;
	transport.migration_started_at = now;
	if (transport.migration_pending_mask == 0)
	{
		transport.migration_in_progress = false;
	}
	transport.authority_snapshot_dirty = true;
	transport.last_error = MultiplayerTransportError::none;
	if (phase == AuthorityPhase::lobby
		&& transport.discovery_socket
			== detail::kInvalidSocketHandle)
	{
		const NativeSocket discovery =
			create_bound_udp(
				kMultiplayerDiscoveryPort, true, true);
		if (discovery != kInvalidNativeSocket)
		{
			transport.discovery_socket =
				public_socket(discovery);
			transport.discovery_active = true;
		}
	}
	queue_authority_changed_event(transport);
	MultiplayerTransportEvent lobby_event;
	lobby_event.kind =
		MultiplayerTransportEventKind::lobby_updated;
	(void)queue_event(transport, lobby_event);
	return true;
}

bool continue_authority_election(
	MultiplayerTransport& transport,
	std::uint64_t now)
{
	for (;;)
	{
		const std::uint8_t successor =
			elect_authority_successor(
				transport,
				transport.migration_failed_mask);
		if (successor
			== kMultiplayerTransportPlayerCapacity
			|| successor == UINT8_MAX)
		{
			return false;
		}
		transport.lobby.leader_slot = successor;
		transport.migration_target_slot = successor;
		if (successor == transport.local_lobby_slot)
		{
			return assume_migrated_authority(
				transport, now);
		}
		if (begin_migration_connection(
			transport, successor, now))
		{
			return true;
		}
		transport.migration_failed_mask |=
			static_cast<std::uint8_t>(1u << successor);
		(void)retire_member_locally(
			transport,
			successor,
			MultiplayerDepartureReason::connection_lost);
	}
}

bool begin_client_authority_migration(
	MultiplayerTransport& transport,
	std::uint64_t now,
	std::uint8_t announced_successor = UINT8_MAX)
{
	if (transport.host
		|| transport.local_lobby_slot
			>= kMultiplayerTransportPlayerCapacity
		|| transport.authority_lobby_slot
			>= kMultiplayerTransportPlayerCapacity
		|| transport.authority_epoch == 0
		|| transport.authority_token == 0)
	{
		return false;
	}
	const std::uint8_t departed_authority =
		transport.authority_lobby_slot;
	transport.previous_authority_epoch =
		transport.authority_epoch;
	transport.previous_authority_token =
		transport.authority_token;
	transport.migration_failed_mask =
		static_cast<std::uint8_t>(1u << departed_authority);
	transport.migration_in_progress = true;
	transport.migration_started_at = now;
	close_peer(transport.peers[0]);
	// Conditional datagrams queued for the departed authority carry its old
	// endpoint and per-connection token. They cannot be authenticated by the
	// successor and must not leak onto the replacement connection.
	transport.datagram_read = 0;
	transport.datagram_count = 0;
	(void)retire_member_locally(
		transport,
		departed_authority,
		MultiplayerDepartureReason::host_shutdown);
	const std::uint8_t elected =
		elect_authority_successor(
			transport, transport.migration_failed_mask);
	if (announced_successor
			< kMultiplayerTransportPlayerCapacity
		&& announced_successor != elected)
	{
		return false;
	}
	return continue_authority_election(transport, now);
}

void disconnect_client(
	MultiplayerTransport& transport,
	MultiplayerDepartureReason reason,
	MultiplayerTransportError error)
{
	if (transport.launch_snapshot_valid
		&& (transport.state
				== MultiplayerTransportState::gameplay
			|| transport.state
				== MultiplayerTransportState::post_mission))
	{
		transport.mission_result.network_aborted = true;
		if (!transport.launch_snapshot.deathmatch_mode)
		{
			for (std::uint8_t gameplay_player = 0;
				gameplay_player
					< transport.launch_snapshot.player_count;
				++gameplay_player)
			{
				if (transport.mission_result.players[
						gameplay_player]
					!= MultiplayerPlayerMissionOutcome::none)
				{
					continue;
				}
				apply_player_mission_outcome(
					transport,
					gameplay_player,
					transport.gameplay_lobby_slot[
						gameplay_player],
					MultiplayerPlayerMissionOutcome::
						network_abort);
			}
		}
	}
	close_operation_sockets(transport);
	transport.state = MultiplayerTransportState::disconnected;
	transport.last_error = error;
	MultiplayerTransportEvent event;
	event.kind = MultiplayerTransportEventKind::disconnected;
	event.error = error;
	event.departure_reason = reason;
	(void)queue_event(transport, event);
}

bool host_accepts_gameplay(
	const MultiplayerTransport& transport)
{
	return transport.state == MultiplayerTransportState::gameplay
		|| transport.state
			== MultiplayerTransportState::post_mission;
}

bool client_accepts_gameplay(
	const MultiplayerTransport& transport)
{
	return transport.state == MultiplayerTransportState::gameplay
		|| transport.state
			== MultiplayerTransportState::post_mission;
}

enum class PacketResult : std::uint8_t
{
	accepted,
	backpressure,
	migration_requested,
	disconnect_requested,
	protocol_error,
};

PacketResult handle_host_tcp_packet(
	MultiplayerTransport& transport,
	std::uint8_t peer_index,
	PacketType type,
	const MultiplayerSessionId& session_id,
	Reader& reader,
	std::uint64_t now)
{
	detail::MultiplayerTransportPeer& peer =
		transport.peers[peer_index];
	if (!peer.authenticated)
	{
		if (type == PacketType::client_hello)
		{
			if (transport.state
					!= MultiplayerTransportState::hosting_lobby
				|| !session_id_matches(
					session_id,
					transport.lobby.session_id,
					true))
			{
				return PacketResult::protocol_error;
			}
			MultiplayerLobbyPlayer player;
			read_player(reader, player);
			const std::uint16_t udp_port = reader.u16();
			const std::uint16_t listener_port = reader.u16();
			const std::uint64_t identity_token = reader.u64();
			std::uint8_t lobby_slot = UINT8_MAX;
			for (std::uint8_t candidate = 0;
				candidate < kMultiplayerTransportPlayerCapacity;
				++candidate)
			{
				if (candidate != transport.local_lobby_slot
					&& !transport.lobby.players[
						candidate].connected)
				{
					lobby_slot = candidate;
					break;
				}
			}
			bool identity_unique = identity_token != 0;
			for (const detail::MultiplayerTransportMember& member :
				transport.members)
			{
				if (member.connected
					&& member.identity_token == identity_token)
				{
					identity_unique = false;
				}
			}
			if (!reader.done()
				|| udp_port == 0
				|| listener_port == 0
				|| !identity_unique
				|| !valid_player(
					player,
					false,
					transport.lobby.rules.team_mode)
				|| lobby_slot
					>= kMultiplayerTransportPlayerCapacity)
			{
				return PacketResult::protocol_error;
			}
			player.connected = true;
			player.ready = false;
			player.post_mission_ready = false;
			player.outcome_requires_restart = false;
			player.deathmatch_kills = 0;
			player.deathmatch_deaths = 0;
			player.latency = 0;
			player.one_way_latency = 0;
			peer.authentication_token = random_u64();
			peer.identity_token = identity_token;
			peer.authenticated = true;
			peer.last_receive_at = now;
			peer.lobby_slot = lobby_slot;
			peer.listener_port_be = htons(listener_port);
			peer.udp_port_be = htons(udp_port);
			transport.lobby.players[lobby_slot] = player;
			detail::MultiplayerTransportMember& member =
				transport.members[lobby_slot];
			member = {};
			member.identity_token = identity_token;
			member.address_be = peer.address_be;
			member.listener_port_be = peer.listener_port_be;
			member.udp_port_be = peer.udp_port_be;
			member.connected = true;
			transport.lobby.player_count =
				connected_player_count(transport.lobby);
			if (!queue_welcome(transport, peer)
				|| !broadcast_lobby(transport)
				|| !queue_authority_state_for_peer(
					transport, peer))
			{
				return PacketResult::protocol_error;
			}
			mark_authority_snapshot_dirty(transport);
			MultiplayerTransportEvent joined;
			joined.kind =
				MultiplayerTransportEventKind::peer_joined;
			joined.player = lobby_slot;
			(void)queue_event(transport, joined);
			return PacketResult::accepted;
		}
		if (type != PacketType::migration_hello
			|| session_id != transport.lobby.session_id
			|| !transport.migration_in_progress)
		{
			return PacketResult::protocol_error;
		}
		const std::uint8_t lobby_slot = reader.u8();
		const std::uint64_t identity_token = reader.u64();
		const std::uint32_t previous_epoch = reader.u32();
		const std::uint64_t previous_token = reader.u64();
		const std::uint16_t udp_port = reader.u16();
		const std::uint16_t listener_port = reader.u16();
		const bool post_mission = reader.boolean();
		const bool post_mission_ready = reader.boolean();
		if (!reader.done()
			|| lobby_slot
				>= kMultiplayerTransportPlayerCapacity
			|| lobby_slot == transport.local_lobby_slot
			|| previous_epoch
				!= transport.previous_authority_epoch
			|| previous_token
				!= transport.previous_authority_token
			|| identity_token == 0
			|| identity_token
				!= transport.members[
					lobby_slot].identity_token
			|| !transport.members[lobby_slot].connected
			|| udp_port == 0
			|| listener_port == 0
			|| (post_mission_ready && !post_mission)
			|| (post_mission
				&& transport.state
					!= MultiplayerTransportState::gameplay
				&& transport.state
					!= MultiplayerTransportState::post_mission)
			|| peer_for_lobby_slot(transport, lobby_slot)
				!= nullptr)
		{
			return PacketResult::protocol_error;
		}
		peer.authentication_token = random_u64();
		peer.identity_token = identity_token;
		peer.authenticated = true;
		peer.last_receive_at = now;
		peer.lobby_slot = lobby_slot;
		peer.gameplay_slot =
			transport.members[lobby_slot].gameplay_slot;
		peer.listener_port_be = htons(listener_port);
		peer.udp_port_be = htons(udp_port);
		peer.post_mission = post_mission
			|| transport.members[lobby_slot].post_mission;
		detail::MultiplayerTransportMember& member =
			transport.members[lobby_slot];
		member.address_be = peer.address_be;
		member.listener_port_be = peer.listener_port_be;
		member.udp_port_be = peer.udp_port_be;
		member.post_mission = peer.post_mission;
		if (post_mission)
		{
			transport.lobby.players[
				lobby_slot].post_mission_ready =
				post_mission_ready;
		}
		transport.migration_pending_mask &=
			static_cast<std::uint8_t>(~(1u << lobby_slot));
		if (transport.migration_pending_mask == 0)
		{
			transport.migration_in_progress = false;
		}
		if (!queue_migration_welcome(transport, peer)
			|| !queue_authority_state_for_peer(
				transport, peer))
		{
			return PacketResult::protocol_error;
		}
		mark_authority_snapshot_dirty(transport);
		return PacketResult::accepted;
	}
	if (session_id != transport.lobby.session_id)
	{
		return PacketResult::protocol_error;
	}
	switch (type)
	{
	case PacketType::lobby_mutation:
	{
		if (transport.state
			!= MultiplayerTransportState::hosting_lobby)
		{
			return PacketResult::protocol_error;
		}
		MultiplayerLobbyPlayer proposed;
		read_player(reader, proposed);
		if (!reader.done()
			|| !valid_player(
				proposed,
				false,
				transport.lobby.rules.team_mode))
		{
			return PacketResult::protocol_error;
		}
		const MultiplayerLobbyPlayer& retained =
			transport.lobby.players[peer.lobby_slot];
		const std::uint32_t latency = retained.latency;
		const std::uint8_t one_way_latency =
			retained.one_way_latency;
		const std::int32_t deathmatch_kills =
			retained.deathmatch_kills;
		const std::int32_t deathmatch_deaths =
			retained.deathmatch_deaths;
		const bool connected = retained.connected;
		const bool post_ready = retained.post_mission_ready;
		const bool outcome_requires_restart =
			retained.outcome_requires_restart;
		MultiplayerLobbySnapshot lobby = transport.lobby;
		MultiplayerLobbyPlayer& updated =
			lobby.players[peer.lobby_slot];
		updated = proposed;
		updated.connected = connected;
		updated.latency = latency;
		updated.one_way_latency = one_way_latency;
		updated.deathmatch_kills = deathmatch_kills;
		updated.deathmatch_deaths = deathmatch_deaths;
		updated.post_mission_ready = post_ready;
		updated.outcome_requires_restart =
			outcome_requires_restart;
		if (!broadcast_lobby_snapshot(transport, lobby))
		{
			return PacketResult::backpressure;
		}
		transport.lobby = lobby;
		mark_authority_snapshot_dirty(transport);
		return PacketResult::accepted;
	}
	case PacketType::lobby_chat:
	{
		const std::uint8_t claimed_source = reader.u8();
		char text[mission::kNetworkChatBytes]{};
		reader.text(text);
		if (!reader.done()
			|| transport.state
				!= MultiplayerTransportState::hosting_lobby
			|| claimed_source != peer.lobby_slot
			|| !valid_utf8(text, sizeof(text)))
		{
			return PacketResult::protocol_error;
		}
		if (transport.lobby_chat_count
				>= kMultiplayerLobbyChatCapacity
			|| !broadcast_tcp_packet(
				transport,
				PacketType::lobby_chat,
				[&](Writer& writer)
				{
					writer.u8(peer.lobby_slot);
					writer.text(text);
				},
				[&](const detail::MultiplayerTransportPeer&
					destination)
				{
					return destination.lobby_slot
						!= peer.lobby_slot;
				}))
		{
			return PacketResult::backpressure;
		}
		if (!queue_lobby_chat(
			transport, peer.lobby_slot, text))
		{
			return PacketResult::protocol_error;
		}
		return PacketResult::accepted;
	}
	case PacketType::prelaunch_loadout:
	{
		const std::uint8_t claimed_slot = reader.u8();
		MultiplayerLobbyPlayer proposed;
		read_player(reader, proposed);
		const MultiplayerLobbyPlayer& current =
			transport.lobby.players[peer.lobby_slot];
		if (!reader.done()
			|| transport.state
				!= MultiplayerTransportState::host_prelaunch
			|| claimed_slot != peer.lobby_slot
			|| !current.connected
			|| !proposed.connected
			|| !proposed.ready
			|| proposed.post_mission_ready
			|| proposed.outcome_requires_restart
			|| proposed.team != current.team
			|| std::strncmp(
				proposed.name,
				current.name,
				sizeof(proposed.name)) != 0
			|| !valid_player(
				proposed,
				true,
				transport.lobby.rules.team_mode))
		{
			return PacketResult::protocol_error;
		}
		MultiplayerLobbySnapshot lobby = transport.lobby;
		MultiplayerLobbyPlayer& retained =
			lobby.players[peer.lobby_slot];
		retained = proposed;
		retained.latency = current.latency;
		retained.one_way_latency = current.one_way_latency;
		retained.deathmatch_kills =
			current.deathmatch_kills;
		retained.deathmatch_deaths =
			current.deathmatch_deaths;
		if (!broadcast_lobby_snapshot(transport, lobby))
		{
			return PacketResult::backpressure;
		}
		transport.lobby = lobby;
		mark_authority_snapshot_dirty(transport);
		return PacketResult::accepted;
	}
	case PacketType::gameplay:
	{
		if (!host_accepts_gameplay(transport)
			|| peer.gameplay_slot
				>= kMultiplayerTransportPlayerCapacity)
		{
			return PacketResult::protocol_error;
		}
		mission::NetworkOutboundMessage message;
		if (!read_semantic_message(reader, message)
			|| !reader.done()
			|| conditional_delivery(message.delivery)
			|| !validate_authenticated_gameplay(
				transport, message, peer.gameplay_slot))
		{
			return PacketResult::protocol_error;
		}
		const bool deliver_local =
			message.destination_player == kBroadcastPlayer
			|| message.destination_player
				== transport.local_gameplay_player;
		if (deliver_local
			&& !can_queue_gameplay(transport, message))
		{
			return PacketResult::backpressure;
		}
		if (!relay_gameplay_from_host(
			transport, message, peer.gameplay_slot))
		{
			return PacketResult::backpressure;
		}
		if (deliver_local
			&& !queue_gameplay(transport, message))
		{
			return PacketResult::protocol_error;
		}
		retain_deathmatch_player_stats(transport, message);
		if (message.kind
				== mission::NetworkOutboundKind::gameplay
			&& message.opcode
				== mission::NetworkGameplayOpcode::
					player_departure
			&& message.departure_player
				== peer.gameplay_slot)
		{
			peer.departure_published = true;
		}
		return PacketResult::accepted;
	}
	case PacketType::player_mission_outcome:
	{
		const std::uint8_t gameplay_player = reader.u8();
		const std::uint8_t lobby_slot = reader.u8();
		const auto outcome =
			static_cast<MultiplayerPlayerMissionOutcome>(
				reader.u8());
		if (!reader.done()
			|| !host_accepts_gameplay(transport)
			|| !transport.launch_snapshot_valid
			|| transport.launch_snapshot.deathmatch_mode
			|| gameplay_player != peer.gameplay_slot
			|| lobby_slot != peer.lobby_slot
			|| !valid_player_outcome_identity(
				transport, gameplay_player, lobby_slot)
			|| !reportable_player_mission_outcome(outcome))
		{
			return PacketResult::protocol_error;
		}
		const MultiplayerPlayerMissionOutcome current =
			transport.mission_result.players[gameplay_player];
		if (current == outcome)
		{
			return PacketResult::accepted;
		}
		if (current != MultiplayerPlayerMissionOutcome::none)
		{
			return PacketResult::protocol_error;
		}
		if (!broadcast_player_mission_outcome(
			transport,
			gameplay_player,
			lobby_slot,
			outcome,
			UINT8_MAX))
		{
			return PacketResult::backpressure;
		}
		apply_player_mission_outcome(
			transport,
			gameplay_player,
			lobby_slot,
			outcome);
		mark_authority_snapshot_dirty(transport);
		return PacketResult::accepted;
	}
	case PacketType::mission_result:
	{
		const auto payload =
			static_cast<MissionResultPayload>(reader.u8());
		const std::uint8_t gameplay_player = reader.u8();
		const std::uint8_t lobby_slot = reader.u8();
		MultiplayerPlayerMissionReport report;
		if (payload != MissionResultPayload::player_report
			|| !transport.launch_snapshot_valid
			|| transport.launch_snapshot.deathmatch_mode
			|| gameplay_player != peer.gameplay_slot
			|| lobby_slot != peer.lobby_slot
			|| !read_player_mission_report(
				reader, transport.launch_snapshot, report)
			|| !reader.done())
		{
			return PacketResult::protocol_error;
		}
		report.lobby_slot = lobby_slot;
		const MultiplayerPlayerMissionReport& retained =
			transport.mission_result.reports[
				gameplay_player];
		if (retained.valid)
		{
			return player_mission_report_equal(
				retained, report)
				? PacketResult::accepted
				: PacketResult::protocol_error;
		}
		const MultiplayerPlayerMissionOutcome current =
			transport.mission_result.players[
				gameplay_player];
		if (!valid_player_outcome_identity(
				transport, gameplay_player, lobby_slot)
			|| (current
					!= MultiplayerPlayerMissionOutcome::none
				&& current != report.outcome))
		{
			return PacketResult::protocol_error;
		}
		if (!broadcast_player_mission_report(
			transport,
			gameplay_player,
			lobby_slot,
			report,
			gameplay_player))
		{
			return PacketResult::backpressure;
		}
		return apply_player_mission_report(
			transport,
			gameplay_player,
			lobby_slot,
			report)
			? PacketResult::accepted
			: PacketResult::protocol_error;
	}
	case PacketType::post_mission_status:
	{
		const bool entered = reader.boolean();
		const bool ready = reader.boolean();
		if (!reader.done()
			|| !entered
			|| !transport.launch_snapshot_valid
			|| peer.gameplay_slot
				>= kMultiplayerTransportPlayerCapacity
			|| !transport.mission_result.reports[
				peer.gameplay_slot].valid)
		{
			return PacketResult::protocol_error;
		}
		MultiplayerLobbySnapshot lobby = transport.lobby;
		lobby.players[
			peer.lobby_slot].post_mission_ready = ready;
		if (!broadcast_lobby_snapshot(transport, lobby))
		{
			return PacketResult::backpressure;
		}
		peer.post_mission = true;
		transport.members[
			peer.lobby_slot].post_mission = true;
		transport.lobby = lobby;
		mark_authority_snapshot_dirty(transport);
		MultiplayerTransportEvent event;
		event.kind = MultiplayerTransportEventKind::
			post_mission_updated;
		event.player = peer.gameplay_slot;
		(void)queue_event(transport, event);
		return PacketResult::accepted;
	}
	case PacketType::post_mission_action:
	{
		const std::uint8_t actor = reader.u8();
		const auto action =
			static_cast<MultiplayerPostMissionAction>(
				reader.u8());
		if (!reader.done()
			|| actor != peer.gameplay_slot
			|| !peer.post_mission
			|| !post_mission_action_allowed(
				transport, actor, action))
		{
			return PacketResult::protocol_error;
		}
		if (!broadcast_post_mission_action(
			transport, actor, action))
		{
			return PacketResult::backpressure;
		}
		if (!apply_post_mission_action(
				transport, actor, action))
		{
			return PacketResult::protocol_error;
		}
		return PacketResult::accepted;
	}
	case PacketType::post_mission_chat:
	{
		char text[mission::kNetworkChatBytes]{};
		const std::uint8_t claimed_source = reader.u8();
		reader.text(text);
		if (!reader.done()
			|| claimed_source != peer.gameplay_slot
			|| !peer.post_mission
			|| !valid_utf8(text, sizeof(text)))
		{
			return PacketResult::protocol_error;
		}
		const std::uint8_t source = peer.gameplay_slot;
		const bool deliver_local =
			transport.state
				== MultiplayerTransportState::post_mission;
		if ((deliver_local
				&& transport.post_mission_chat_count
					>= kMultiplayerPostMissionChatCapacity)
			|| !broadcast_tcp_packet(
				transport,
				PacketType::post_mission_chat,
				[&](Writer& writer)
				{
					writer.u8(source);
					writer.text(text);
				},
				[&](const detail::MultiplayerTransportPeer&
					destination)
				{
					return destination.post_mission
						&& destination.lobby_slot
							!= peer.lobby_slot;
				}))
		{
			return PacketResult::backpressure;
		}
		if (deliver_local
			&& !queue_post_mission_chat(
				transport, source, text))
		{
			return PacketResult::protocol_error;
		}
		return PacketResult::accepted;
	}
	case PacketType::pong:
	{
		const std::uint32_t nonce = reader.u32();
		if (!reader.done()
			|| nonce != peer.ping_nonce
			|| peer.ping_sent_at == 0)
		{
			return PacketResult::protocol_error;
		}
		const std::uint64_t elapsed =
			now >= peer.ping_sent_at
				? now - peer.ping_sent_at
				: 0;
		// FUN_004b6790 measures RTT with the 100 Hz retail clock and then
		// shifts it right once. floor(RTT milliseconds / 20) is the exact
		// equivalent one-way 100 Hz tick value.
		peer.raw_one_way_latency_ticks =
			static_cast<std::uint32_t>(
				std::min<std::uint64_t>(
					elapsed / 20u, UINT32_MAX));
		peer.ping_sent_at = 0;
		return PacketResult::accepted;
	}
	case PacketType::disconnect:
	{
		const auto reason =
			static_cast<MultiplayerDepartureReason>(reader.u8());
		if (!reader.done()
			|| reason > MultiplayerDepartureReason::kicked)
		{
			return PacketResult::protocol_error;
		}
		peer.requested_departure_reason = reason;
		return PacketResult::disconnect_requested;
	}
	default:
		return PacketResult::protocol_error;
	}
}

PacketResult handle_client_tcp_packet(
	MultiplayerTransport& transport,
	PacketType type,
	const MultiplayerSessionId& session_id,
	Reader& reader,
	std::uint64_t now)
{
	detail::MultiplayerTransportPeer& server =
		transport.peers[0];
	if (!server.authenticated)
	{
		if (type == PacketType::server_welcome)
		{
			if (!session_id_matches(
				transport.expected_session_id,
				session_id,
				true))
			{
				return PacketResult::protocol_error;
			}
			const std::uint8_t local_slot = reader.u8();
			const std::uint64_t token = reader.u64();
			const std::uint64_t identity_token = reader.u64();
			const std::uint16_t udp_port = reader.u16();
			const std::uint32_t authority_epoch = reader.u32();
			const std::uint64_t authority_token = reader.u64();
			const std::uint8_t authority_slot = reader.u8();
			MultiplayerLobbySnapshot lobby;
			if (!read_lobby(reader, lobby)
				|| !reader.done()
				|| lobby.session_id != session_id
				|| local_slot
					>= kMultiplayerTransportPlayerCapacity
				|| local_slot == authority_slot
				|| authority_slot
					>= kMultiplayerTransportPlayerCapacity
				|| lobby.leader_slot != authority_slot
				|| !lobby.players[local_slot].connected
				|| identity_token
					!= transport.local_identity_token
				|| token == 0
				|| udp_port == 0
				|| authority_epoch == 0
				|| authority_token == 0)
			{
				return PacketResult::protocol_error;
			}
			transport.lobby = lobby;
			if (lobby.rules.mode
				== MultiplayerSessionMode::deathmatch)
			{
				transport.prelaunch_campaign = {};
			}
			transport.expected_session_id = session_id;
			transport.local_lobby_slot = local_slot;
			transport.authority_lobby_slot = authority_slot;
			transport.authority_epoch = authority_epoch;
			transport.authority_token = authority_token;
			transport.session_port = udp_port;
			detail::MultiplayerTransportMember& local_member =
				transport.members[local_slot];
			local_member = {};
			local_member.identity_token = identity_token;
			local_member.listener_port_be =
				htons(transport.local_listener_port);
			local_member.udp_port_be =
				htons(transport.local_udp_port);
			local_member.connected = true;
			server.authentication_token = token;
			server.authenticated = true;
			server.listener_port_be = htons(udp_port);
			server.udp_port_be = htons(udp_port);
			server.last_receive_at = now;
			transport.state =
				MultiplayerTransportState::client_lobby;
			queue_udp_probe(transport, server);
			MultiplayerTransportEvent connected;
			connected.kind =
				MultiplayerTransportEventKind::connected;
			connected.player = local_slot;
			(void)queue_event(transport, connected);
			MultiplayerTransportEvent lobby_event;
			lobby_event.kind =
				MultiplayerTransportEventKind::lobby_updated;
			(void)queue_event(transport, lobby_event);
			return PacketResult::accepted;
		}
		if (type != PacketType::migration_welcome
			|| !server.migration_connection
			|| session_id != transport.lobby.session_id)
		{
			return PacketResult::protocol_error;
		}
		const std::uint8_t local_slot = reader.u8();
		const std::uint64_t authentication_token = reader.u64();
		const std::uint64_t identity_token = reader.u64();
		const std::uint16_t udp_port = reader.u16();
		const std::uint32_t authority_epoch = reader.u32();
		const std::uint64_t authority_token = reader.u64();
		const std::uint8_t authority_slot = reader.u8();
		std::uint32_t expected_authority_epoch =
			transport.previous_authority_epoch + 1u;
		if (expected_authority_epoch == 0)
		{
			expected_authority_epoch = 1;
		}
		if (!reader.done()
			|| local_slot != transport.local_lobby_slot
			|| identity_token != transport.local_identity_token
			|| authentication_token == 0
			|| udp_port == 0
			|| authority_epoch != expected_authority_epoch
			|| authority_epoch == 0
			|| authority_token == 0
			|| authority_slot
				!= transport.migration_target_slot)
		{
			return PacketResult::protocol_error;
		}
		transport.authority_epoch = authority_epoch;
		transport.authority_token = authority_token;
		transport.authority_lobby_slot = authority_slot;
		transport.session_port = udp_port;
		server.authentication_token = authentication_token;
		server.authenticated = true;
		server.listener_port_be = htons(udp_port);
		server.udp_port_be = htons(udp_port);
		server.last_receive_at = now;
		transport.migration_in_progress = false;
		transport.migration_target_slot = UINT8_MAX;
		transport.last_error = MultiplayerTransportError::none;
		queue_udp_probe(transport, server);
		const MultiplayerPlayerMissionReport* const local_report =
			mission_report_for_lobby_slot(
				transport.mission_result,
				transport.local_lobby_slot);
		if (transport.launch_snapshot_valid
			&& local_report != nullptr)
		{
			std::uint8_t gameplay_player = UINT8_MAX;
			for (std::uint8_t player = 0;
				player < kMultiplayerTransportPlayerCapacity;
				++player)
			{
				if (&transport.mission_result.reports[player]
					== local_report)
				{
					gameplay_player = player;
					break;
				}
			}
			if (gameplay_player
					>= kMultiplayerTransportPlayerCapacity
				|| !queue_tcp_packet(
					server,
					PacketType::mission_result,
					transport.lobby.session_id,
					[&](Writer& writer)
					{
						writer.u8(static_cast<std::uint8_t>(
							MissionResultPayload::
								player_report));
						writer.u8(gameplay_player);
						writer.u8(
							transport.local_lobby_slot);
						write_player_mission_report(
							writer, *local_report);
					}))
			{
				return PacketResult::protocol_error;
			}
		}
		else if (transport.launch_snapshot_valid
			&& transport.local_gameplay_player
				< kMultiplayerTransportPlayerCapacity
			&& transport.mission_result.players[
				transport.local_gameplay_player]
				!= MultiplayerPlayerMissionOutcome::none
			&& !queue_tcp_packet(
				server,
				PacketType::player_mission_outcome,
				transport.lobby.session_id,
				[&](Writer& writer)
				{
					writer.u8(
						transport.local_gameplay_player);
					writer.u8(
						transport.local_lobby_slot);
					writer.u8(static_cast<std::uint8_t>(
						transport.mission_result.players[
							transport.local_gameplay_player]));
				}))
		{
			return PacketResult::protocol_error;
		}
		MultiplayerTransportEvent authority_event;
		authority_event.kind =
			MultiplayerTransportEventKind::authority_changed;
		authority_event.authority_epoch = authority_epoch;
		authority_event.player =
			transport.members[authority_slot].gameplay_slot
					< kMultiplayerTransportPlayerCapacity
				? transport.members[
					authority_slot].gameplay_slot
				: authority_slot;
		(void)queue_event(transport, authority_event);
		return PacketResult::accepted;
	}
	if (session_id != transport.lobby.session_id)
	{
		return PacketResult::protocol_error;
	}
	switch (type)
	{
	case PacketType::authority_snapshot:
		return apply_authority_snapshot(transport, reader)
			? PacketResult::accepted
			: PacketResult::protocol_error;
	case PacketType::authority_runtime:
		return apply_authority_runtime(transport, reader)
			? PacketResult::accepted
			: PacketResult::protocol_error;
	case PacketType::authority_handoff:
	{
		const std::uint32_t epoch = reader.u32();
		const std::uint64_t token = reader.u64();
		const std::uint8_t successor = reader.u8();
		if (!reader.done()
			|| epoch != transport.authority_epoch
			|| token != transport.authority_token
			|| successor
				>= kMultiplayerTransportPlayerCapacity
			|| successor == transport.authority_lobby_slot
			|| !transport.members[successor].connected)
		{
			return PacketResult::protocol_error;
		}
		transport.migration_target_slot = successor;
		return PacketResult::migration_requested;
	}
	case PacketType::lobby_snapshot:
	{
		MultiplayerLobbySnapshot lobby;
		if (!read_lobby(reader, lobby)
			|| !reader.done()
			|| lobby.session_id
				!= transport.lobby.session_id
			|| transport.local_lobby_slot
				>= kMultiplayerTransportPlayerCapacity
			|| !lobby.players[
				transport.local_lobby_slot].connected)
		{
			return PacketResult::protocol_error;
		}
		transport.lobby = lobby;
		if (lobby.rules.mode
			== MultiplayerSessionMode::deathmatch)
		{
			transport.prelaunch_campaign = {};
		}
		MultiplayerTransportEvent event;
		event.kind =
			MultiplayerTransportEventKind::lobby_updated;
		(void)queue_event(transport, event);
		return PacketResult::accepted;
	}
	case PacketType::lobby_chat:
	{
		const std::uint8_t source = reader.u8();
		char text[mission::kNetworkChatBytes]{};
		reader.text(text);
			if (!reader.done()
				|| transport.state
					!= MultiplayerTransportState::client_lobby
				|| source >= kMultiplayerTransportPlayerCapacity
				|| !transport.lobby.players[source].connected
				|| !valid_utf8(text, sizeof(text)))
			{
				return PacketResult::protocol_error;
			}
			if (transport.lobby_chat_count
				>= kMultiplayerLobbyChatCapacity)
			{
				return PacketResult::backpressure;
			}
			if (!queue_lobby_chat(transport, source, text))
			{
				return PacketResult::protocol_error;
			}
			return PacketResult::accepted;
	}
	case PacketType::prelaunch:
	{
		MultiplayerLobbySnapshot lobby;
		game::MultiplayerCampaignLaunchState campaign_control;
		const bool ordinary_start =
			transport.state
				== MultiplayerTransportState::client_lobby;
		const bool possible_alternate_start =
			transport.state
				== MultiplayerTransportState::gameplay
			&& has_direct_mission_25_report(
				transport.mission_result);
		if ((!ordinary_start && !possible_alternate_start)
			|| !read_lobby(reader, lobby))
		{
			return PacketResult::protocol_error;
		}
		read_campaign_control(reader, campaign_control);
		if (!reader.done()
			|| lobby.session_id
				!= transport.lobby.session_id
			|| transport.local_lobby_slot
				>= kMultiplayerTransportPlayerCapacity
			|| !lobby.players[
				transport.local_lobby_slot].connected
			|| !valid_campaign_control(
				campaign_control, lobby.rules)
			|| (possible_alternate_start
				&& !std::any_of(
					std::begin(
						transport.mission_result.reports),
					std::end(
						transport.mission_result.reports),
					[&](const MultiplayerPlayerMissionReport&
						report)
					{
						return report.valid
							&& report.campaign
								.mission_25_alternate
							&& campaign_launch_equal(
								campaign_control,
								report.campaign);
					})))
		{
			return PacketResult::protocol_error;
		}
		game::MultiplayerCampaignLaunchState personal_source =
			transport.prelaunch_campaign;
		const MultiplayerPlayerMissionReport* const local_report =
			mission_report_for_lobby_slot(
				transport.mission_result,
				transport.local_lobby_slot);
		if (possible_alternate_start
			&& local_report != nullptr
			&& local_report->campaign.present
			&& local_report->campaign.state.mission == 25)
		{
			personal_source = local_report->campaign;
		}
		else if (campaign_control.mission_25_alternate
			&& personal_source.present
			&& personal_source.state.mission == 25)
		{
			personal_source.mission_25_alternate = true;
		}
		game::MultiplayerCampaignLaunchState personal_campaign;
		if (!install_campaign_control(
				campaign_control,
				lobby.rules,
				personal_source,
				personal_campaign))
		{
			return PacketResult::protocol_error;
		}
		transport.lobby = lobby;
		transport.prelaunch_campaign = personal_campaign;
		transport.launch_snapshot = {};
		transport.launch_snapshot_valid = false;
		transport.authority_prelaunch_pending = false;
		transport.shared_bootstrap_pending = false;
		transport.local_gameplay_player = UINT8_MAX;
		std::fill(
			std::begin(transport.gameplay_lobby_slot),
			std::end(transport.gameplay_lobby_slot),
			UINT8_MAX);
		synchronize_member_gameplay_slots(transport);
		server.gameplay_slot = UINT8_MAX;
		server.post_mission = false;
		transport.state =
			MultiplayerTransportState::client_prelaunch;
		MultiplayerTransportEvent prelaunch_event;
		prelaunch_event.kind =
			MultiplayerTransportEventKind::prelaunch;
		prelaunch_event.player =
			transport.local_lobby_slot;
		(void)queue_event(transport, prelaunch_event);
		MultiplayerTransportEvent lobby_event;
		lobby_event.kind =
			MultiplayerTransportEventKind::lobby_updated;
		(void)queue_event(transport, lobby_event);
		return PacketResult::accepted;
	}
	case PacketType::launch:
	{
		game::MultiplayerLaunchSnapshot launch;
		if (!read_launch(
				reader,
				transport.prelaunch_campaign,
				launch)
			|| !reader.done()
			|| launch.role != game::MultiplayerRole::client
			|| !launch_matches_prelaunch(transport, launch))
		{
			return PacketResult::protocol_error;
		}
		transport.launch_snapshot = launch;
		transport.launch_snapshot_valid = true;
		transport.shared_bootstrap = launch.bootstrap;
		transport.replay_bootstrap = launch.bootstrap;
		transport.launch_generation =
			launch.bootstrap.present
				? std::max(
					transport.launch_generation,
					launch.bootstrap.launch_generation)
				: transport.launch_generation;
		transport.shared_bootstrap_pending = false;
		transport.authority_prelaunch_pending = false;
		if (!assign_gameplay_lobby_mapping(transport))
		{
			transport.launch_snapshot = {};
			transport.launch_snapshot_valid = false;
			return PacketResult::protocol_error;
		}
		transport.local_gameplay_player =
			launch.local_player;
		synchronize_member_gameplay_slots(transport);
		for (MultiplayerLobbyPlayer& player :
			transport.lobby.players)
		{
			player.post_mission_ready = false;
			player.outcome_requires_restart = false;
			player.deathmatch_kills = 0;
			player.deathmatch_deaths = 0;
		}
		transport.mission_result = {};
		transport.state = MultiplayerTransportState::gameplay;
		server.gameplay_slot =
			authority_gameplay_player(transport);
		MultiplayerTransportEvent event;
		event.kind = MultiplayerTransportEventKind::launch;
		event.player = launch.local_player;
		(void)queue_event(transport, event);
		return PacketResult::accepted;
	}
	case PacketType::gameplay:
	{
		if (!client_accepts_gameplay(transport))
		{
			return PacketResult::protocol_error;
		}
		mission::NetworkOutboundMessage message;
		if (!read_semantic_message(reader, message)
			|| !reader.done()
			|| conditional_delivery(message.delivery)
			|| !valid_semantic_message(message)
			|| !valid_destination(transport, message)
			|| !gameplay_player_connected(
				transport, message.source_player))
		{
			return PacketResult::protocol_error;
		}
		if (message.destination_player == kBroadcastPlayer
			|| message.destination_player
				== transport.local_gameplay_player)
		{
			if (!can_queue_gameplay(transport, message))
			{
				return PacketResult::backpressure;
			}
			if (!queue_gameplay(transport, message))
			{
				return PacketResult::protocol_error;
			}
		}
		retain_deathmatch_player_stats(transport, message);
		return PacketResult::accepted;
	}
	case PacketType::player_mission_outcome:
	{
		const std::uint8_t gameplay_player = reader.u8();
		const std::uint8_t lobby_slot = reader.u8();
		const auto outcome =
			static_cast<MultiplayerPlayerMissionOutcome>(
				reader.u8());
		if (!reader.done()
			|| !client_accepts_gameplay(transport)
			|| !transport.launch_snapshot_valid
			|| transport.launch_snapshot.deathmatch_mode
			|| !valid_player_outcome_identity(
				transport, gameplay_player, lobby_slot)
			|| !valid_player_mission_outcome(outcome)
			|| outcome == MultiplayerPlayerMissionOutcome::none)
		{
			return PacketResult::protocol_error;
		}
		const MultiplayerPlayerMissionOutcome current =
			transport.mission_result.players[gameplay_player];
		if (current != MultiplayerPlayerMissionOutcome::none
			&& current != outcome)
		{
			return PacketResult::protocol_error;
		}
		if (current == MultiplayerPlayerMissionOutcome::none)
		{
			apply_player_mission_outcome(
				transport,
				gameplay_player,
				lobby_slot,
				outcome);
		}
		return PacketResult::accepted;
	}
	case PacketType::mission_result:
	{
		const auto payload =
			static_cast<MissionResultPayload>(reader.u8());
		if (!client_accepts_gameplay(transport)
			|| !transport.launch_snapshot_valid)
		{
			return PacketResult::protocol_error;
		}
		if (payload == MissionResultPayload::player_report)
		{
			const std::uint8_t gameplay_player = reader.u8();
			const std::uint8_t lobby_slot = reader.u8();
			MultiplayerPlayerMissionReport report;
			if (transport.launch_snapshot.deathmatch_mode
				|| !read_player_mission_report(
					reader,
					transport.launch_snapshot,
					report)
				|| !reader.done()
				|| !valid_player_outcome_identity(
					transport,
					gameplay_player,
					lobby_slot))
			{
				return PacketResult::protocol_error;
			}
			report.lobby_slot = lobby_slot;
			return apply_player_mission_report(
				transport,
				gameplay_player,
				lobby_slot,
				report)
				? PacketResult::accepted
				: PacketResult::protocol_error;
		}
		if (payload != MissionResultPayload::deathmatch_scores)
		{
			return PacketResult::protocol_error;
		}
		MultiplayerMissionResultSnapshot result;
		if (!transport.launch_snapshot.deathmatch_mode
			|| !read_mission_result_snapshot(
				reader, transport.launch_snapshot, result)
			|| !reader.done())
		{
			return PacketResult::protocol_error;
		}
		transport.mission_result = result;
		for (std::uint8_t player = 0;
			player < transport.launch_snapshot.player_count;
			++player)
		{
			const std::uint8_t lobby_slot =
				transport.gameplay_lobby_slot[player];
			if (lobby_slot
					< kMultiplayerTransportPlayerCapacity
				&& transport.lobby.players[
					lobby_slot].connected)
			{
				transport.lobby.players[
					lobby_slot].outcome_requires_restart =
					outcome_requires_restart(
						result.players[player]);
				transport.lobby.players[
					lobby_slot].deathmatch_kills =
					result.deathmatch_kills[player];
				transport.lobby.players[
					lobby_slot].deathmatch_deaths =
					result.deathmatch_deaths[player];
			}
		}
		MultiplayerTransportEvent event;
		event.kind =
			MultiplayerTransportEventKind::mission_result;
		(void)queue_event(transport, event);
		transport.state =
			MultiplayerTransportState::client_lobby;
		transport.launch_snapshot_valid = false;
		transport.local_gameplay_player = UINT8_MAX;
		std::fill(
			std::begin(transport.gameplay_lobby_slot),
			std::end(transport.gameplay_lobby_slot),
			UINT8_MAX);
		synchronize_member_gameplay_slots(transport);
		return PacketResult::accepted;
	}
	case PacketType::peer_departure:
	{
		const std::uint8_t lobby_slot = reader.u8();
		const std::uint8_t gameplay_slot = reader.u8();
		const auto reason =
			static_cast<MultiplayerDepartureReason>(reader.u8());
		if (!reader.done()
			|| lobby_slot
				>= kMultiplayerTransportPlayerCapacity
			|| (gameplay_slot
					>= kMultiplayerTransportPlayerCapacity
				&& gameplay_slot != UINT8_MAX)
			|| reason > MultiplayerDepartureReason::kicked)
		{
			return PacketResult::protocol_error;
		}
		if (lobby_slot
			< kMultiplayerTransportPlayerCapacity)
		{
			transport.lobby.players[lobby_slot] = {};
			transport.lobby.players[
				lobby_slot].selected_ship = -1;
			transport.lobby.players[lobby_slot].team = -1;
			transport.lobby.player_count =
				connected_player_count(transport.lobby);
			transport.members[lobby_slot].connected = false;
			transport.members[lobby_slot].post_mission = false;
		}
		if (transport.launch_snapshot_valid
			&& gameplay_slot
				< transport.launch_snapshot.player_count)
		{
			if (!transport.launch_snapshot.deathmatch_mode
				&& transport.mission_result.players[
					gameplay_slot]
					== MultiplayerPlayerMissionOutcome::none)
			{
				apply_player_mission_outcome(
					transport,
					gameplay_slot,
					lobby_slot,
					reason
							== MultiplayerDepartureReason::kicked
						? MultiplayerPlayerMissionOutcome::kicked
						: MultiplayerPlayerMissionOutcome::
							departed);
			}
			transport.launch_snapshot.players[
				gameplay_slot].connected = false;
			if (!transport.launch_snapshot.deathmatch_mode
				&& !has_direct_mission_25_report(
					transport.mission_result))
			{
				const DebriefLeader leader =
					compute_debrief_leader(
						transport.mission_result,
						transport.launch_snapshot);
				transport.mission_result.debrief_leader_player =
					leader.player;
				transport.mission_result.debrief_leader_valid =
					leader.valid;
			}
		}
		MultiplayerTransportEvent event;
		event.kind =
			MultiplayerTransportEventKind::peer_departed;
		event.player = gameplay_slot
			< kMultiplayerTransportPlayerCapacity
				? gameplay_slot
				: lobby_slot;
		event.departure_reason = reason;
		(void)queue_event(transport, event);
		return PacketResult::accepted;
	}
	case PacketType::post_mission_action:
	{
		const std::uint8_t actor = reader.u8();
		const auto action =
			static_cast<MultiplayerPostMissionAction>(
				reader.u8());
		if (!reader.done()
			|| !post_mission_action_allowed(
				transport, actor, action, false))
		{
			return PacketResult::protocol_error;
		}
		if (!apply_post_mission_action(
				transport, actor, action))
		{
			return PacketResult::protocol_error;
		}
		return PacketResult::accepted;
	}
	case PacketType::post_mission_chat:
	{
		const std::uint8_t source = reader.u8();
		char text[mission::kNetworkChatBytes]{};
		reader.text(text);
			if (!reader.done()
				|| transport.state
					!= MultiplayerTransportState::post_mission
				|| !gameplay_player_connected(transport, source)
				|| !valid_utf8(text, sizeof(text)))
			{
				return PacketResult::protocol_error;
			}
			if (transport.post_mission_chat_count
				>= kMultiplayerPostMissionChatCapacity)
			{
				return PacketResult::backpressure;
			}
			if (!queue_post_mission_chat(
				transport, source, text))
			{
				return PacketResult::protocol_error;
			}
			return PacketResult::accepted;
	}
	case PacketType::ping:
	{
		const std::uint32_t nonce = reader.u32();
		if (!reader.done()
			|| !queue_tcp_packet(
				server,
				PacketType::pong,
				transport.lobby.session_id,
				[&](Writer& writer)
				{
					writer.u32(nonce);
				}))
		{
			return PacketResult::protocol_error;
		}
		return PacketResult::accepted;
	}
	case PacketType::disconnect:
	{
		const auto reason =
			static_cast<MultiplayerDepartureReason>(reader.u8());
		if (!reader.done()
			|| reason > MultiplayerDepartureReason::kicked)
		{
			return PacketResult::protocol_error;
		}
		server.requested_departure_reason = reason;
		return PacketResult::disconnect_requested;
	}
	default:
		return PacketResult::protocol_error;
	}
}

PacketResult handle_tcp_payload(
	MultiplayerTransport& transport,
	std::uint8_t peer_index,
	const std::uint8_t* bytes,
	std::size_t count,
	std::uint64_t now)
{
	Reader reader{bytes, count};
	PacketType type{};
	MultiplayerSessionId session_id;
	if (!read_header(reader, type, session_id))
	{
		return PacketResult::protocol_error;
	}
	return transport.host
		? handle_host_tcp_packet(
			transport,
			peer_index,
			type,
			session_id,
			reader,
			now)
		: handle_client_tcp_packet(
			transport, type, session_id, reader, now);
}
}
#endif
#if !defined(__EMSCRIPTEN__)
namespace
{
bool finish_nonblocking_connect(
	MultiplayerTransport& transport,
	detail::MultiplayerTransportPeer& peer)
{
	if (!peer.connecting)
	{
		return true;
	}
	fd_set writable;
	fd_set failed;
	FD_ZERO(&writable);
	FD_ZERO(&failed);
	const NativeSocket socket = native_socket(peer.socket);
	FD_SET(socket, &writable);
	FD_SET(socket, &failed);
	timeval timeout{};
#if defined(_WIN32)
	const int descriptor_count = 0;
#else
	const int descriptor_count = socket + 1;
#endif
	const int selected = select(
		descriptor_count,
		nullptr,
		&writable,
		&failed,
		&timeout);
	if (selected == 0)
	{
		return true;
	}
	if (selected < 0)
	{
		return socket_interrupted(socket_error());
	}
	int error = 0;
	SocketLength length = sizeof(error);
#if defined(_WIN32)
	const int result = getsockopt(
		socket,
		SOL_SOCKET,
		SO_ERROR,
		reinterpret_cast<char*>(&error),
		&length);
#else
	const int result = getsockopt(
		socket,
		SOL_SOCKET,
		SO_ERROR,
		&error,
		&length);
#endif
	if (result != 0)
	{
		return false;
	}
	if (error != 0)
	{
		return socket_would_block(error);
	}
	peer.connecting = false;
	return peer.migration_connection
		? queue_migration_hello(transport, peer)
		: queue_hello(transport, peer);
}

PacketResult process_peer_frames(
	MultiplayerTransport& transport,
	std::uint8_t peer_index,
	std::uint64_t now)
{
	detail::MultiplayerTransportPeer& peer =
		transport.peers[peer_index];
	while (peer.receive_count >= 2)
	{
		const std::size_t frame_length =
			(static_cast<std::size_t>(peer.receive[0]) << 8u)
			| peer.receive[1];
		if (frame_length == 0
			|| frame_length > detail::kWirePacketBytes)
		{
			return PacketResult::protocol_error;
		}
		if (peer.receive_count < frame_length + 2)
		{
			return PacketResult::accepted;
		}
		const PacketResult result = handle_tcp_payload(
			transport,
			peer_index,
			peer.receive + 2,
			frame_length,
			now);
		if (result != PacketResult::accepted)
		{
			return result;
		}
		const std::size_t consumed = frame_length + 2;
		const std::size_t remaining =
			peer.receive_count - consumed;
		if (remaining != 0)
		{
			std::memmove(
				peer.receive,
				peer.receive + consumed,
				remaining);
		}
		peer.receive_count =
			static_cast<std::uint16_t>(remaining);
	}
	return PacketResult::accepted;
}

PacketResult receive_peer(
	MultiplayerTransport& transport,
	std::uint8_t peer_index,
	std::uint64_t now)
{
	detail::MultiplayerTransportPeer& peer =
		transport.peers[peer_index];
	if (!finish_nonblocking_connect(transport, peer))
	{
		return PacketResult::protocol_error;
	}
	if (peer.connecting)
	{
		return PacketResult::accepted;
	}
	if (peer.receive_count >= 2)
	{
		const PacketResult buffered =
			process_peer_frames(
				transport, peer_index, now);
		if (buffered != PacketResult::accepted)
		{
			return buffered;
		}
	}
	for (;;)
	{
		if (peer.receive_count >= detail::kTcpReceiveBytes)
		{
			return PacketResult::protocol_error;
		}
		const int received = recv(
			native_socket(peer.socket),
			reinterpret_cast<char*>(
				peer.receive + peer.receive_count),
			static_cast<int>(
				detail::kTcpReceiveBytes
				- peer.receive_count),
			0);
		if (received > 0)
		{
			peer.receive_count =
				static_cast<std::uint16_t>(
					peer.receive_count + received);
			peer.last_receive_at = now;
			const PacketResult result =
				process_peer_frames(
					transport, peer_index, now);
			if (result != PacketResult::accepted)
			{
				return result;
			}
			continue;
		}
		if (received == 0)
		{
			return PacketResult::disconnect_requested;
		}
		const int error = socket_error();
		if (socket_interrupted(error))
		{
			continue;
		}
		return socket_would_block(error)
			? PacketResult::accepted
			: PacketResult::disconnect_requested;
	}
}

void accept_host_peers(
	MultiplayerTransport& transport,
	std::uint64_t now)
{
	const NativeSocket listener =
		native_socket(transport.listen_socket);
	if (listener == kInvalidNativeSocket)
	{
		return;
	}
	for (;;)
	{
		sockaddr_in remote{};
		SocketLength remote_length = sizeof(remote);
		const NativeSocket socket = accept(
			listener,
			reinterpret_cast<sockaddr*>(&remote),
			&remote_length);
		if (socket == kInvalidNativeSocket)
		{
			const int error = socket_error();
			if (socket_interrupted(error))
			{
				continue;
			}
			return;
		}
		std::uint8_t slot = UINT8_MAX;
		for (std::uint8_t candidate = 0;
			candidate < kMultiplayerTransportPlayerCapacity;
			++candidate)
		{
			if (!transport.peers[candidate].occupied)
			{
				slot = candidate;
				break;
			}
		}
		if (slot == UINT8_MAX
			|| !transport.host
			|| remote.sin_family != AF_INET
			|| !prepare_stream_socket(socket))
		{
			close_native_socket(socket);
			continue;
		}
		detail::MultiplayerTransportPeer& peer =
			transport.peers[slot];
		peer = {};
		peer.socket = public_socket(socket);
		peer.occupied = true;
		peer.address_be = remote.sin_addr.s_addr;
		peer.last_receive_at = now;
		peer.last_send_at = now;
	}
}

void service_ping(
	MultiplayerTransport& transport,
	detail::MultiplayerTransportPeer& peer,
	std::uint64_t now)
{
	if (!transport.host
		|| !peer.authenticated
		|| peer.ping_sent_at != 0
		|| now - peer.last_ping_at < kPingIntervalMilliseconds)
	{
		return;
	}
	++peer.ping_nonce;
	if (queue_tcp_packet(
		peer,
		PacketType::ping,
		transport.lobby.session_id,
		[&](Writer& writer)
		{
			writer.u32(peer.ping_nonce);
		}))
	{
		peer.ping_sent_at = now;
		peer.last_ping_at = now;
	}
}

std::uint32_t current_raw_one_way_latency_ticks(
	const detail::MultiplayerTransportPeer& peer,
	std::uint64_t now)
{
	if (peer.ping_sent_at == 0)
	{
		return peer.raw_one_way_latency_ticks;
	}
	const std::uint64_t elapsed =
		now >= peer.ping_sent_at
			? now - peer.ping_sent_at
			: 0;
	return static_cast<std::uint32_t>(
		std::min<std::uint64_t>(
			elapsed / 20u, UINT32_MAX));
}

void service_latency_smoothing(
	MultiplayerTransport& transport,
	std::uint64_t now)
{
	if (!transport.host
		|| (transport.next_latency_smoothing_at != 0
			&& now < transport.next_latency_smoothing_at))
	{
		return;
	}
	const std::uint64_t interval =
		kLatencySmoothingIntervalMilliseconds;
	const std::uint64_t until_next =
		interval - now % interval;
	transport.next_latency_smoothing_at =
		now <= std::numeric_limits<std::uint64_t>::max()
				- until_next
			? now + until_next
			: std::numeric_limits<std::uint64_t>::max();

	bool changed = false;
	for (const detail::MultiplayerTransportPeer& peer :
		transport.peers)
	{
		if (!peer.occupied
			|| !peer.authenticated
			|| peer.lobby_slot
				>= kMultiplayerTransportPlayerCapacity)
		{
			continue;
		}
		MultiplayerLobbyPlayer& player =
			transport.lobby.players[peer.lobby_slot];
		if (!player.connected)
		{
			continue;
		}
		const std::uint32_t raw =
			current_raw_one_way_latency_ticks(peer, now);
		const std::uint8_t encoded_raw =
			static_cast<std::uint8_t>(
				std::min<std::uint32_t>(raw, UINT8_MAX));
		const std::uint32_t smoothed =
			static_cast<std::uint32_t>(
				(std::uint64_t{raw}
					+ std::uint64_t{player.latency} * 5u)
				/ 6u);
		if (player.one_way_latency == encoded_raw
			&& player.latency == smoothed)
		{
			continue;
		}
		player.one_way_latency = encoded_raw;
		player.latency = smoothed;
		changed = true;

		if (transport.launch_snapshot_valid
			&& peer.gameplay_slot
				< transport.launch_snapshot.player_count)
		{
			game::MultiplayerPlayerLaunch& launch_player =
				transport.launch_snapshot.players[
					peer.gameplay_slot];
			launch_player.one_way_latency = encoded_raw;
			launch_player.latency = smoothed;
		}
	}
	if (changed)
	{
		(void)broadcast_lobby(transport);
		mark_authority_snapshot_dirty(transport);
	}
}

bool recover_client_authority(
	MultiplayerTransport& transport,
	std::uint64_t now,
	std::uint8_t announced_successor = UINT8_MAX)
{
	if (transport.migration_in_progress)
	{
		const std::uint8_t failed =
			transport.migration_target_slot;
		close_peer(transport.peers[0]);
		if (failed < kMultiplayerTransportPlayerCapacity)
		{
			transport.migration_failed_mask |=
				static_cast<std::uint8_t>(1u << failed);
			(void)retire_member_locally(
				transport,
				failed,
				MultiplayerDepartureReason::connection_lost);
		}
		return continue_authority_election(
			transport, now);
	}
	return begin_client_authority_migration(
		transport, now, announced_successor);
}

void service_tcp(MultiplayerTransport& transport, std::uint64_t now)
{
	if (transport.host)
	{
		accept_host_peers(transport, now);
	}
	for (std::uint8_t index = 0;
		index < kMultiplayerTransportPlayerCapacity;
		++index)
	{
		detail::MultiplayerTransportPeer& peer =
			transport.peers[index];
		if (!peer.occupied)
		{
			continue;
		}
		const PacketResult receive_result =
			receive_peer(transport, index, now);
		if (receive_result != PacketResult::accepted
			&& receive_result != PacketResult::backpressure)
		{
			if (transport.host)
			{
				remove_host_peer(
					transport,
					index,
					receive_result
							== PacketResult::disconnect_requested
						? peer.requested_departure_reason
						: receive_result
								== PacketResult::protocol_error
							? MultiplayerDepartureReason::
								protocol_error
							: MultiplayerDepartureReason::
								connection_lost);
			}
			else
			{
				const std::uint8_t announced_successor =
					receive_result
							== PacketResult::migration_requested
						? transport.migration_target_slot
						: UINT8_MAX;
					const bool kicked =
						receive_result
								== PacketResult::disconnect_requested
							&& peer.requested_departure_reason
								== MultiplayerDepartureReason::kicked;
					const bool protocol_failure =
						receive_result
								== PacketResult::protocol_error
							|| (receive_result
									== PacketResult::
										disconnect_requested
								&& peer.requested_departure_reason
									== MultiplayerDepartureReason::
										protocol_error);
					const bool recoverable =
						!kicked
						&& !protocol_failure
						&& transport.local_lobby_slot
						< kMultiplayerTransportPlayerCapacity
					&& transport.members[
						transport.local_lobby_slot].connected
					&& recover_client_authority(
						transport,
						now,
						announced_successor);
				if (!recoverable)
				{
					disconnect_client(
						transport,
						kicked
							? MultiplayerDepartureReason::kicked
							: receive_result
									== PacketResult::protocol_error
								? MultiplayerDepartureReason::
									protocol_error
								: MultiplayerDepartureReason::
									connection_lost,
						kicked
							? MultiplayerTransportError::
								connection_lost
							: receive_result
									== PacketResult::protocol_error
								? MultiplayerTransportError::
									protocol_violation
								: MultiplayerTransportError::
									connection_lost);
				}
				return;
			}
			continue;
		}
		if (!peer.occupied)
		{
			continue;
		}
		service_ping(transport, peer, now);
		if (!flush_peer(peer, now))
		{
			if (transport.host)
			{
				remove_host_peer(
					transport,
					index,
					MultiplayerDepartureReason::
						connection_lost);
			}
			else
			{
				if (!recover_client_authority(
					transport, now))
				{
					disconnect_client(
						transport,
						MultiplayerDepartureReason::
							connection_lost,
						MultiplayerTransportError::
							connection_lost);
				}
				return;
			}
			continue;
		}
		if (now - peer.last_receive_at
			> kConnectionTimeoutMilliseconds)
		{
			if (transport.host)
			{
				remove_host_peer(
					transport,
					index,
					MultiplayerDepartureReason::timeout);
			}
			else
			{
				if (!recover_client_authority(
					transport, now))
				{
					disconnect_client(
						transport,
						MultiplayerDepartureReason::timeout,
						MultiplayerTransportError::
							connection_timeout);
				}
				return;
			}
		}
	}
}

detail::MultiplayerTransportPeer* authenticated_udp_peer(
	MultiplayerTransport& transport,
	std::uint8_t lobby_slot,
	std::uint64_t token,
	const sockaddr_in& source)
{
	if (lobby_slot >= kMultiplayerTransportPlayerCapacity)
	{
		return nullptr;
	}
	detail::MultiplayerTransportPeer* const peer =
		peer_for_lobby_slot(transport, lobby_slot);
	if (peer == nullptr
		|| peer->authentication_token != token
		|| peer->address_be != source.sin_addr.s_addr)
	{
		return nullptr;
	}
	return peer;
}

void service_host_udp_packet(
	MultiplayerTransport& transport,
	PacketType type,
	Reader& reader,
	const sockaddr_in& source)
{
	const std::uint8_t lobby_slot = reader.u8();
	const std::uint64_t token = reader.u64();
	const std::uint32_t sequence = reader.u32();
	detail::MultiplayerTransportPeer* const peer =
		authenticated_udp_peer(
			transport, lobby_slot, token, source);
	if (peer == nullptr
		|| (peer->udp_sequence_seen
			&& !sequence_newer(
				sequence, peer->udp_receive_sequence)))
	{
		return;
	}
		if (type == PacketType::udp_probe)
		{
		if (!reader.done())
		{
			return;
		}
			peer->udp_receive_sequence = sequence;
			peer->udp_sequence_seen = true;
			peer->udp_port_be = source.sin_port;
			peer->udp_endpoint_known = true;
			transport.members[
				peer->lobby_slot].udp_port_be =
				source.sin_port;
			mark_authority_snapshot_dirty(transport);
			queue_udp_probe_ack(transport, *peer);
			return;
	}
	if (type != PacketType::gameplay
		|| !host_accepts_gameplay(transport)
		|| !peer->udp_endpoint_known
		|| source.sin_port != peer->udp_port_be
		|| peer->gameplay_slot
			>= kMultiplayerTransportPlayerCapacity)
	{
		return;
	}
	mission::NetworkOutboundMessage message;
	if (!read_semantic_message(reader, message)
		|| !reader.done()
		|| !conditional_delivery(message.delivery)
		|| !validate_authenticated_gameplay(
			transport, message, peer->gameplay_slot))
	{
		return;
	}
	peer->udp_receive_sequence = sequence;
	peer->udp_sequence_seen = true;
	retain_deathmatch_player_stats(transport, message);
	const bool deliver_local =
		message.destination_player == kBroadcastPlayer
		|| message.destination_player
			== transport.local_gameplay_player;
	if (deliver_local
		&& !queue_gameplay(transport, message))
	{
		return;
	}
	(void)relay_gameplay_from_host(
		transport, message, peer->gameplay_slot);
}

void service_client_udp_packet(
	MultiplayerTransport& transport,
	PacketType type,
	Reader& reader,
	const sockaddr_in& source)
{
	detail::MultiplayerTransportPeer& server =
		transport.peers[0];
	if (!server.occupied
		|| !server.authenticated
		|| source.sin_addr.s_addr != server.address_be
		|| (source.sin_port != server.udp_port_be
			&& (type != PacketType::udp_probe_ack
				|| server.udp_endpoint_known)))
	{
		return;
	}
	const std::uint8_t lobby_slot = reader.u8();
	const std::uint64_t token = reader.u64();
	const std::uint32_t sequence = reader.u32();
	if (lobby_slot != transport.local_lobby_slot
		|| token != server.authentication_token
		|| (server.udp_sequence_seen
			&& !sequence_newer(
				sequence, server.udp_receive_sequence)))
	{
		return;
	}
	if (type == PacketType::udp_probe_ack)
	{
		if (!reader.done())
		{
			return;
		}
			server.udp_receive_sequence = sequence;
			server.udp_sequence_seen = true;
			server.udp_port_be = source.sin_port;
			server.udp_endpoint_known = true;
			if (transport.authority_lobby_slot
				< kMultiplayerTransportPlayerCapacity)
			{
				transport.members[
					transport.authority_lobby_slot]
						.udp_port_be = source.sin_port;
			}
			return;
	}
	if (type != PacketType::gameplay
		|| !client_accepts_gameplay(transport))
	{
		return;
	}
	mission::NetworkOutboundMessage message;
	if (!read_semantic_message(reader, message)
		|| !reader.done()
		|| !conditional_delivery(message.delivery)
		|| !valid_semantic_message(message)
		|| !valid_destination(transport, message)
		|| !gameplay_player_connected(
			transport, message.source_player))
	{
		return;
	}
	server.udp_receive_sequence = sequence;
	server.udp_sequence_seen = true;
	retain_deathmatch_player_stats(transport, message);
	if (message.destination_player == kBroadcastPlayer
		|| message.destination_player
			== transport.local_gameplay_player)
	{
		(void)queue_gameplay(transport, message);
	}
}

void service_udp(MultiplayerTransport& transport)
{
	const NativeSocket socket =
		native_socket(transport.udp_socket);
	if (socket == kInvalidNativeSocket
		|| id_is_zero(transport.lobby.session_id))
	{
		return;
	}
	for (;;)
	{
		std::uint8_t bytes[detail::kWirePacketBytes];
		sockaddr_in source{};
		SocketLength source_length = sizeof(source);
		const int received = recvfrom(
			socket,
			reinterpret_cast<char*>(bytes),
			static_cast<int>(sizeof(bytes)),
			0,
			reinterpret_cast<sockaddr*>(&source),
			&source_length);
		if (received < 0)
		{
			const int error = socket_error();
			if (socket_interrupted(error))
			{
				continue;
			}
			return;
		}
		if (received == 0
			|| source.sin_family != AF_INET)
		{
			continue;
		}
		Reader reader{
			bytes, static_cast<std::size_t>(received)};
		PacketType type{};
		MultiplayerSessionId session_id;
		if (!read_header(reader, type, session_id)
			|| session_id != transport.lobby.session_id)
		{
			continue;
		}
		if (transport.host)
		{
			service_host_udp_packet(
				transport, type, reader, source);
		}
		else
		{
			service_client_udp_packet(
				transport, type, reader, source);
		}
	}
}

void relay_retired_member_departure(
	MultiplayerTransport& transport,
	std::uint8_t gameplay_player)
{
	if (!transport.launch_snapshot_valid
		|| gameplay_player
			>= transport.launch_snapshot.player_count)
	{
		return;
	}
	mission::NetworkOutboundMessage message;
	message.kind = mission::NetworkOutboundKind::gameplay;
	message.opcode =
		mission::NetworkGameplayOpcode::player_departure;
	message.delivery =
		mission::NetworkDelivery::broadcast_guaranteed;
	message.source_player = gameplay_player;
	message.destination_player = kBroadcastPlayer;
	message.departure_player = gameplay_player;
	(void)relay_gameplay_from_host(
		transport, message, gameplay_player);
}

void service_authority_state(
	MultiplayerTransport& transport,
	std::uint64_t now)
{
	if (!transport.host)
	{
		return;
	}
	if (transport.state
			== MultiplayerTransportState::hosting_lobby
		&& transport.discovery_socket
			== detail::kInvalidSocketHandle)
	{
		const NativeSocket discovery =
			create_bound_udp(
				kMultiplayerDiscoveryPort, true, true);
		if (discovery != kInvalidNativeSocket)
		{
			transport.discovery_socket =
				public_socket(discovery);
			transport.discovery_active = true;
		}
	}
	if (transport.migration_pending_mask != 0
		&& now - transport.migration_started_at
			> kConnectionTimeoutMilliseconds)
	{
		for (std::uint8_t lobby_slot = 0;
			lobby_slot < kMultiplayerTransportPlayerCapacity;
			++lobby_slot)
		{
			const std::uint8_t bit =
				static_cast<std::uint8_t>(1u << lobby_slot);
			if ((transport.migration_pending_mask & bit) == 0
				|| peer_for_lobby_slot(
					transport, lobby_slot) != nullptr)
			{
				transport.migration_pending_mask &=
					static_cast<std::uint8_t>(~bit);
				continue;
			}
			const std::uint8_t gameplay_slot =
				retire_member_locally(
					transport,
					lobby_slot,
					MultiplayerDepartureReason::timeout);
			queue_peer_departure_control(
				transport,
				lobby_slot,
				gameplay_slot,
				MultiplayerDepartureReason::timeout);
			relay_retired_member_departure(
				transport, gameplay_slot);
			transport.migration_pending_mask &=
				static_cast<std::uint8_t>(~bit);
		}
		(void)broadcast_lobby(transport);
		mark_authority_snapshot_dirty(transport);
		transport.migration_in_progress = false;
	}
	if (transport.authority_snapshot_dirty
		|| now - transport.last_authority_snapshot_at
			>= kAuthoritySnapshotIntervalMilliseconds)
	{
		(void)replicate_authority_state(transport, now);
	}
}

bool queue_graceful_authority_handoff(
	MultiplayerTransport& transport,
	std::uint64_t now)
{
	if (!transport.host
		|| transport.local_lobby_slot
			>= kMultiplayerTransportPlayerCapacity)
	{
		return false;
	}
	const std::uint8_t successor =
		elect_authority_successor(
			transport,
			static_cast<std::uint8_t>(
				1u << transport.local_lobby_slot));
	if (successor >= kMultiplayerTransportPlayerCapacity)
	{
		return false;
	}
	// Put the latest lobby, launch, partial outcomes, scores, debrief READY
	// bits, and endpoint table ahead of the handoff marker in every TCP
	// stream. If an individual stream is already backpressured, EOF still
	// triggers the same deterministic election from its last snapshot.
	(void)replicate_authority_state(transport, now);
	bool queued = false;
	for (detail::MultiplayerTransportPeer& peer :
		transport.peers)
	{
		if (!peer.occupied || !peer.authenticated)
		{
			continue;
		}
		if (queue_tcp_packet(
			peer,
			PacketType::authority_handoff,
			transport.lobby.session_id,
			[&](Writer& writer)
			{
				writer.u32(transport.authority_epoch);
				writer.u64(transport.authority_token);
				writer.u8(successor);
			}))
		{
			queued = true;
		}
		(void)flush_peer(peer, now);
	}
	return queued;
}
}
#endif

#if defined(__EMSCRIPTEN__)
namespace
{
detail::MultiplayerTransportPeer* peer_for_lobby_slot(
	MultiplayerTransport& transport,
	std::uint8_t lobby_slot)
{
	(void)transport;
	(void)lobby_slot;
	return nullptr;
}

const detail::MultiplayerTransportPeer* peer_for_lobby_slot(
	const MultiplayerTransport& transport,
	std::uint8_t lobby_slot)
{
	(void)transport;
	(void)lobby_slot;
	return nullptr;
}

void mark_authority_snapshot_dirty(MultiplayerTransport& transport)
{
	(void)transport;
}

bool player_mission_report_equal(
	const MultiplayerPlayerMissionReport& left,
	const MultiplayerPlayerMissionReport& right)
{
	(void)left;
	(void)right;
	return false;
}

bool post_mission_action_allowed(
	const MultiplayerTransport& transport,
	std::uint8_t actor,
	MultiplayerPostMissionAction action,
	bool require_post_mission_state = true)
{
	(void)transport;
	(void)actor;
	(void)action;
	(void)require_post_mission_state;
	return false;
}
}
#endif

bool multiplayer_transport_supported()
{
#if defined(__EMSCRIPTEN__)
	return false;
#else
	return true;
#endif
}

void multiplayer_transport_initialize(MultiplayerTransport& transport)
{
	if (transport.initialized)
	{
		return;
	}
	transport = {};
	transport.initialized = true;
#if defined(__EMSCRIPTEN__)
	transport.state = MultiplayerTransportState::unsupported;
	transport.last_error =
		MultiplayerTransportError::unsupported_platform;
#else
	if (!socket_runtime_start(transport))
	{
		transport.state = MultiplayerTransportState::error;
		transport.last_error =
			MultiplayerTransportError::socket_runtime;
		return;
	}
	transport.state = MultiplayerTransportState::idle;
#endif
}

void multiplayer_transport_shutdown(
	MultiplayerTransport& transport,
	MultiplayerDepartureReason reason)
{
	if (!transport.initialized)
	{
		return;
	}
#if !defined(__EMSCRIPTEN__)
	const bool handed_off =
		transport.host
		&& queue_graceful_authority_handoff(
			transport, transport.started_at);
	if (transport.host)
	{
		if (!handed_off)
		{
			for (detail::MultiplayerTransportPeer& peer :
				transport.peers)
			{
				if (!peer.occupied || !peer.authenticated)
				{
					continue;
				}
				(void)queue_tcp_packet(
					peer,
					PacketType::disconnect,
					transport.lobby.session_id,
					[&](Writer& writer)
					{
						writer.u8(
							static_cast<std::uint8_t>(
								reason));
					});
				(void)flush_peer(peer, transport.started_at);
			}
		}
	}
	else if (transport.peers[0].occupied
		&& transport.peers[0].authenticated)
	{
		detail::MultiplayerTransportPeer& server =
			transport.peers[0];
		(void)queue_tcp_packet(
			server,
			PacketType::disconnect,
			transport.lobby.session_id,
			[&](Writer& writer)
			{
				writer.u8(static_cast<std::uint8_t>(reason));
			});
		(void)flush_peer(server, transport.started_at);
	}
	close_operation_sockets(transport);
	socket_runtime_stop(transport);
#endif
	transport.initialized = false;
	transport.host = false;
	transport.state = multiplayer_transport_supported()
		? MultiplayerTransportState::disconnected
		: MultiplayerTransportState::unsupported;
}

bool multiplayer_transport_begin_discovery(
	MultiplayerTransport& transport,
	std::uint64_t now)
{
	if (!transport.initialized)
	{
		multiplayer_transport_initialize(transport);
	}
#if defined(__EMSCRIPTEN__)
	(void)now;
	transport.state = MultiplayerTransportState::unsupported;
	transport.last_error =
		MultiplayerTransportError::unsupported_platform;
	return false;
#else
	if (transport.state != MultiplayerTransportState::idle
		&& transport.state
			!= MultiplayerTransportState::disconnected
		&& transport.state
			!= MultiplayerTransportState::discovering)
	{
		queue_error(
			transport,
			MultiplayerTransportError::invalid_state,
			false);
		return false;
	}
	if (transport.discovery_active)
	{
		if (!transport.discovery_targeted)
		{
			return true;
		}
	}
	return start_discovery(
		transport, now, false, 0, 0);
#endif
}

bool multiplayer_transport_begin_ipv4_discovery(
	MultiplayerTransport& transport,
	const char* address,
	std::uint16_t discovery_port,
	std::uint64_t now)
{
	if (!transport.initialized)
	{
		multiplayer_transport_initialize(transport);
	}
#if defined(__EMSCRIPTEN__)
	(void)address;
	(void)discovery_port;
	(void)now;
	transport.state = MultiplayerTransportState::unsupported;
	transport.last_error =
		MultiplayerTransportError::unsupported_platform;
	return false;
#else
	in_addr parsed{};
	if (address == nullptr
		|| inet_pton(AF_INET, address, &parsed) != 1)
	{
		queue_error(
			transport,
			MultiplayerTransportError::address_invalid,
			false);
		return false;
	}
	const std::uint32_t parsed_host_order =
		ntohl(parsed.s_addr);
	if (parsed_host_order == INADDR_ANY
		|| (parsed_host_order & 0xf0000000u)
			== 0xe0000000u
		|| (parsed_host_order & 0xf0000000u)
			== 0xf0000000u)
	{
		queue_error(
			transport,
			MultiplayerTransportError::address_invalid,
			false);
		return false;
	}
	if (transport.state != MultiplayerTransportState::idle
		&& transport.state
			!= MultiplayerTransportState::disconnected
		&& transport.state
			!= MultiplayerTransportState::discovering)
	{
		queue_error(
			transport,
			MultiplayerTransportError::invalid_state,
			false);
		return false;
	}
	return start_discovery(
		transport,
		now,
		true,
		parsed.s_addr,
		htons(
			discovery_port == 0
				? kMultiplayerDiscoveryPort
				: discovery_port));
#endif
}

void multiplayer_transport_end_discovery(
	MultiplayerTransport& transport)
{
#if !defined(__EMSCRIPTEN__)
	if (transport.discovery_active && !transport.host)
	{
		close_socket(transport.discovery_socket);
		transport.discovery_active = false;
		transport.discovery_targeted = false;
		transport.discovery_target_address_be = 0;
		transport.discovery_target_port_be = 0;
		transport.datagram_count = 0;
		transport.datagram_read = 0;
		if (transport.state
			== MultiplayerTransportState::discovering)
		{
			transport.state = MultiplayerTransportState::idle;
		}
	}
#else
	(void)transport;
#endif
}

MultiplayerDiscoveryView multiplayer_transport_discovery_view(
	const MultiplayerTransport& transport)
{
	std::uint32_t count = 0;
	while (count < kMultiplayerDiscoveryCapacity
		&& !id_is_zero(
			transport.discovered[count].session_id))
	{
		++count;
	}
	return {
		transport.discovered,
		count,
		transport.discovery_generation,
	};
}

bool multiplayer_transport_host(
	MultiplayerTransport& transport,
	const char* session_name,
	const MultiplayerLobbyPlayer& local_player,
	const MultiplayerLobbyRules& rules,
	std::uint16_t port,
	std::uint64_t now)
{
	if (!transport.initialized)
	{
		multiplayer_transport_initialize(transport);
	}
#if defined(__EMSCRIPTEN__)
	(void)session_name;
	(void)local_player;
	(void)rules;
	(void)port;
	(void)now;
	transport.last_error =
		MultiplayerTransportError::unsupported_platform;
	return false;
#else
	if (!valid_utf8(session_name, kMultiplayerSessionNameBytes)
		|| !copy_text(transport.lobby.session_name, session_name)
		|| !valid_rules(rules)
		|| !valid_player(local_player, false, rules.team_mode)
		|| (transport.state != MultiplayerTransportState::idle
			&& transport.state
				!= MultiplayerTransportState::discovering
			&& transport.state
				!= MultiplayerTransportState::disconnected
			&& transport.state
				!= MultiplayerTransportState::error))
	{
		queue_error(
			transport,
			MultiplayerTransportError::invalid_lobby,
			false);
		return false;
	}
	char retained_session_name[kMultiplayerSessionNameBytes]{};
	(void)copy_text(retained_session_name, session_name);
	const game::MultiplayerCampaignLaunchState
		retained_personal_campaign =
			transport.prelaunch_campaign;
	close_operation_sockets(transport);
	transport.lobby = {};
	std::fill(
		std::begin(transport.members),
		std::end(transport.members),
		detail::MultiplayerTransportMember{});
	(void)copy_text(
		transport.lobby.session_name,
		retained_session_name);
	generate_session_id(transport.lobby.session_id);
	transport.lobby.rules = rules;
	transport.lobby.leader_slot = 0;
	transport.lobby.players[0] = local_player;
	transport.lobby.players[0].connected = true;
	transport.lobby.players[0].post_mission_ready = false;
	transport.lobby.players[0].outcome_requires_restart = false;
	transport.lobby.players[0].deathmatch_kills = 0;
	transport.lobby.players[0].deathmatch_deaths = 0;
	transport.lobby.players[0].latency = 0;
	transport.lobby.players[0].one_way_latency = 0;
	transport.lobby.player_count = 1;

	const std::uint16_t requested_port =
		port == 0 ? kMultiplayerDefaultSessionPort : port;
	std::uint16_t bound_port{};
	const NativeSocket listener =
		create_listener(requested_port, bound_port);
	const NativeSocket udp =
		create_bound_udp(bound_port, false, false);
	const NativeSocket discovery =
		create_bound_udp(
			kMultiplayerDiscoveryPort, true, true);
	if (listener == kInvalidNativeSocket
		|| udp == kInvalidNativeSocket
		|| discovery == kInvalidNativeSocket)
	{
		close_native_socket(listener);
		close_native_socket(udp);
		close_native_socket(discovery);
		queue_error(
			transport,
			MultiplayerTransportError::bind_failed,
			true);
		return false;
	}
	transport.listen_socket = public_socket(listener);
	transport.udp_socket = public_socket(udp);
	transport.discovery_socket = public_socket(discovery);
	transport.session_port = bound_port;
	transport.local_udp_port = bound_port;
	transport.local_listener_port = bound_port;
	transport.local_lobby_slot = 0;
	transport.local_gameplay_player = UINT8_MAX;
	std::fill(
		std::begin(transport.gameplay_lobby_slot),
		std::end(transport.gameplay_lobby_slot),
		UINT8_MAX);
	transport.host = true;
	transport.next_latency_smoothing_at = 0;
	transport.local_identity_token = random_u64();
	transport.authority_epoch = 1;
	transport.authority_token = random_u64();
	transport.authority_lobby_slot = 0;
	transport.previous_authority_epoch = 0;
	transport.previous_authority_token = 0;
	transport.migration_target_slot = UINT8_MAX;
	transport.migration_failed_mask = 0;
	transport.migration_pending_mask = 0;
	transport.migration_in_progress = false;
	transport.members[0].identity_token =
		transport.local_identity_token;
	transport.members[0].listener_port_be =
		htons(bound_port);
	transport.members[0].udp_port_be = htons(bound_port);
	transport.members[0].gameplay_slot = UINT8_MAX;
	transport.members[0].connected = true;
	transport.discovery_active = true;
	transport.prelaunch_campaign =
		rules.mode == MultiplayerSessionMode::cooperative
			? retained_personal_campaign
			: game::MultiplayerCampaignLaunchState{};
	transport.shared_bootstrap = {};
	transport.replay_bootstrap = {};
	transport.launch_generation = 0;
	transport.shared_bootstrap_pending = false;
	transport.mission_result = {};
	transport.launch_snapshot = {};
	transport.launch_snapshot_valid = false;
	transport.authority_snapshot_dirty = true;
	transport.last_authority_snapshot_at = now;
	transport.last_error = MultiplayerTransportError::none;
	transport.state =
		MultiplayerTransportState::hosting_lobby;
	transport.started_at = now;
	MultiplayerTransportEvent connected;
	connected.kind =
		MultiplayerTransportEventKind::connected;
	connected.player = 0;
	(void)queue_event(transport, connected);
	MultiplayerTransportEvent lobby_event;
	lobby_event.kind =
		MultiplayerTransportEventKind::lobby_updated;
	(void)queue_event(transport, lobby_event);
	return true;
#endif
}

bool multiplayer_transport_join(
	MultiplayerTransport& transport,
	const MultiplayerDiscoverySession& session,
	const MultiplayerLobbyPlayer& local_player,
	std::uint64_t now)
{
	if (id_is_zero(session.session_id)
		|| !terminated(
			session.address, sizeof(session.address))
		|| session.port == 0)
	{
		if (!transport.initialized)
		{
			multiplayer_transport_initialize(transport);
		}
		queue_error(
			transport,
			MultiplayerTransportError::address_invalid,
			false);
		return false;
	}
	return multiplayer_transport_join_ipv4(
		transport,
		session.address,
		session.port,
		&session.session_id,
		local_player,
		now);
}

bool multiplayer_transport_join_ipv4(
	MultiplayerTransport& transport,
	const char* address,
	std::uint16_t port,
	const MultiplayerSessionId* expected_session,
	const MultiplayerLobbyPlayer& local_player,
	std::uint64_t now)
{
	if (!transport.initialized)
	{
		multiplayer_transport_initialize(transport);
	}
#if defined(__EMSCRIPTEN__)
	(void)address;
	(void)port;
	(void)expected_session;
	(void)local_player;
	(void)now;
	transport.last_error =
		MultiplayerTransportError::unsupported_platform;
	return false;
#else
	if (!terminated(address, kMultiplayerIpv4TextBytes)
		|| port == 0
		|| !valid_player(local_player, false, false)
		|| (transport.state != MultiplayerTransportState::idle
			&& transport.state
				!= MultiplayerTransportState::discovering
			&& transport.state
				!= MultiplayerTransportState::disconnected
			&& transport.state
				!= MultiplayerTransportState::error))
	{
		queue_error(
			transport,
			MultiplayerTransportError::address_invalid,
			false);
		return false;
	}
	sockaddr_in remote{};
	remote.sin_family = AF_INET;
	remote.sin_port = htons(port);
	if (inet_pton(AF_INET, address, &remote.sin_addr) != 1)
	{
		queue_error(
			transport,
			MultiplayerTransportError::address_invalid,
			false);
		return false;
	}
	const game::MultiplayerCampaignLaunchState
		retained_personal_campaign =
			transport.prelaunch_campaign;
	close_operation_sockets(transport);
	std::uint16_t local_listener_port{};
	const NativeSocket listener =
		create_listener(0, local_listener_port);
	const NativeSocket udp =
		listener == kInvalidNativeSocket
			? kInvalidNativeSocket
			: create_bound_udp(
				local_listener_port,
				false,
				false,
				&transport.local_udp_port);
	const NativeSocket socket =
		::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (listener == kInvalidNativeSocket
		|| udp == kInvalidNativeSocket
		|| socket == kInvalidNativeSocket
		|| !prepare_stream_socket(socket))
	{
		close_native_socket(listener);
		close_native_socket(udp);
		close_native_socket(socket);
		queue_error(
			transport,
			MultiplayerTransportError::socket_create,
			true);
		return false;
	}
	const int result = connect(
		socket,
		reinterpret_cast<const sockaddr*>(&remote),
		static_cast<SocketLength>(sizeof(remote)));
	const bool connecting =
		result != 0 && socket_would_block(socket_error());
	if (result != 0 && !connecting)
	{
		close_native_socket(listener);
		close_native_socket(udp);
		close_native_socket(socket);
		queue_error(
			transport,
			MultiplayerTransportError::connect_failed,
			true);
		return false;
	}
	transport.lobby = {};
	std::fill(
		std::begin(transport.members),
		std::end(transport.members),
		detail::MultiplayerTransportMember{});
	transport.expected_session_id =
		expected_session != nullptr
			? *expected_session
			: MultiplayerSessionId{};
	transport.pending_local_player = local_player;
	transport.pending_local_player.connected = true;
	transport.pending_local_player.ready = false;
	transport.pending_local_player.post_mission_ready = false;
	transport.pending_local_player.outcome_requires_restart = false;
	transport.pending_local_player.deathmatch_kills = 0;
	transport.pending_local_player.deathmatch_deaths = 0;
	transport.listen_socket = public_socket(listener);
	transport.udp_socket = public_socket(udp);
	transport.session_port = port;
	transport.local_listener_port = local_listener_port;
	transport.local_lobby_slot = UINT8_MAX;
	transport.local_gameplay_player = UINT8_MAX;
	std::fill(
		std::begin(transport.gameplay_lobby_slot),
		std::end(transport.gameplay_lobby_slot),
		UINT8_MAX);
	transport.host = false;
	transport.local_identity_token = random_u64();
	transport.authority_epoch = 0;
	transport.authority_token = 0;
	transport.authority_lobby_slot = UINT8_MAX;
	transport.previous_authority_epoch = 0;
	transport.previous_authority_token = 0;
	transport.migration_target_slot = UINT8_MAX;
	transport.migration_failed_mask = 0;
	transport.migration_pending_mask = 0;
	transport.migration_in_progress = false;
	transport.discovery_active = false;
	transport.prelaunch_campaign =
		retained_personal_campaign;
	transport.shared_bootstrap = {};
	transport.replay_bootstrap = {};
	transport.launch_generation = 0;
	transport.shared_bootstrap_pending = false;
	transport.mission_result = {};
	transport.launch_snapshot = {};
	transport.launch_snapshot_valid = false;
	detail::MultiplayerTransportPeer& server =
		transport.peers[0];
	server = {};
	server.socket = public_socket(socket);
	server.occupied = true;
	server.connecting = connecting;
	server.address_be = remote.sin_addr.s_addr;
	server.listener_port_be = remote.sin_port;
	server.udp_port_be = remote.sin_port;
	server.last_receive_at = now;
	server.last_send_at = now;
	transport.state = MultiplayerTransportState::connecting;
	transport.started_at = now;
	transport.last_error = MultiplayerTransportError::none;
	if (!connecting && !queue_hello(transport, server))
	{
		disconnect_client(
			transport,
			MultiplayerDepartureReason::protocol_error,
			MultiplayerTransportError::queue_full);
		return false;
	}
	return true;
#endif
}

void multiplayer_transport_poll(
	MultiplayerTransport& transport,
	std::uint64_t now)
{
	if (!transport.initialized
		|| transport.state
			== MultiplayerTransportState::unsupported)
	{
		return;
	}
#if !defined(__EMSCRIPTEN__)
	if (transport.discovery_active)
	{
		if (!transport.host
			&& now - transport.last_discovery_query_at
				>= kDiscoveryIntervalMilliseconds)
		{
			send_discovery_query(transport);
			transport.last_discovery_query_at = now;
		}
		service_discovery_receive(transport, now);
		if (!transport.host)
		{
			expire_discovery(transport, now);
		}
	}
	service_tcp(transport, now);
	if (transport.state
			== MultiplayerTransportState::disconnected
		|| transport.state
			== MultiplayerTransportState::error)
	{
		return;
	}
	service_latency_smoothing(transport, now);
	service_authority_state(transport, now);
	service_udp(transport);
	if (!transport.host
		&& transport.peers[0].occupied
		&& transport.peers[0].authenticated
		&& !transport.peers[0].udp_endpoint_known
		&& now - transport.peers[0].last_send_at
			>= kDiscoveryIntervalMilliseconds)
	{
		queue_udp_probe(transport, transport.peers[0]);
		transport.peers[0].last_send_at = now;
	}
	(void)flush_datagrams(transport);
#else
	(void)now;
#endif
}

bool multiplayer_transport_set_local_player(
	MultiplayerTransport& transport,
	const MultiplayerLobbyPlayer& player)
{
	const bool lobby_state =
		transport.state
			== MultiplayerTransportState::hosting_lobby
		|| transport.state
			== MultiplayerTransportState::client_lobby;
	if (!lobby_state
		|| transport.local_lobby_slot
			>= kMultiplayerTransportPlayerCapacity
		|| !valid_player(
			player, false, transport.lobby.rules.team_mode))
	{
		transport.last_error =
			MultiplayerTransportError::invalid_lobby;
		return false;
	}
	MultiplayerLobbyPlayer proposed = player;
	const MultiplayerLobbyPlayer& current =
		transport.lobby.players[transport.local_lobby_slot];
	proposed.connected = true;
	proposed.latency = current.latency;
	proposed.one_way_latency = current.one_way_latency;
	proposed.deathmatch_kills =
		current.deathmatch_kills;
	proposed.deathmatch_deaths =
		current.deathmatch_deaths;
	proposed.post_mission_ready =
		current.post_mission_ready;
	proposed.outcome_requires_restart =
		current.outcome_requires_restart;
	if (transport.host)
	{
		transport.lobby.players[
			transport.local_lobby_slot] = proposed;
#if !defined(__EMSCRIPTEN__)
		const bool published = broadcast_lobby(transport);
		if (published)
		{
			mark_authority_snapshot_dirty(transport);
		}
		return published;
#else
		return false;
#endif
	}
#if !defined(__EMSCRIPTEN__)
	detail::MultiplayerTransportPeer& server =
		transport.peers[0];
	if (!server.occupied
		|| !server.authenticated
		|| !queue_tcp_packet(
			server,
			PacketType::lobby_mutation,
			transport.lobby.session_id,
			[&](Writer& writer)
			{
				write_player(writer, proposed);
			}))
	{
		transport.last_error =
			MultiplayerTransportError::queue_full;
		return false;
	}
	transport.lobby.players[
		transport.local_lobby_slot] = proposed;
	return true;
#else
	return false;
#endif
}

bool multiplayer_transport_set_rules(
	MultiplayerTransport& transport,
	const MultiplayerLobbyRules& rules)
{
	if (!transport.host
		|| transport.state
			!= MultiplayerTransportState::hosting_lobby)
	{
		transport.last_error =
			MultiplayerTransportError::not_authority;
		return false;
	}
	if (!valid_rules(rules))
	{
		transport.last_error =
			MultiplayerTransportError::invalid_lobby;
		return false;
	}
#if !defined(__EMSCRIPTEN__)
	MultiplayerLobbySnapshot proposed = transport.lobby;
	const bool shared_mission_changed =
		rules.mode != transport.lobby.rules.mode
		|| rules.mission != transport.lobby.rules.mission;
	proposed.rules = rules;
	if (proposed.coop_selection.present
		&& (rules.mode != MultiplayerSessionMode::cooperative
			|| proposed.coop_selection.mission
				!= rules.mission))
	{
		proposed.coop_selection = {};
	}
	if (!valid_lobby(proposed)
		|| !broadcast_lobby_snapshot(transport, proposed))
	{
		transport.last_error =
			MultiplayerTransportError::queue_full;
		return false;
	}
	transport.lobby = proposed;
	if (rules.mode == MultiplayerSessionMode::deathmatch)
	{
		transport.prelaunch_campaign = {};
		transport.shared_bootstrap = {};
		transport.replay_bootstrap = {};
		transport.shared_bootstrap_pending = false;
	}
	else if (shared_mission_changed)
	{
		// A manual mission change is a new launch, not Replay. Retain the
		// current PILO/ALPH/session image as continuity, but force a fresh
		// generation to be materialized at begin_prelaunch().
		transport.shared_bootstrap_pending = false;
	}
	mark_authority_snapshot_dirty(transport);
	transport.last_error = MultiplayerTransportError::none;
	return true;
#else
	return false;
#endif
}

bool multiplayer_transport_set_coop_mission_selection(
	MultiplayerTransport& transport,
	const char* mission_name,
	const char* presentation_title,
	std::uint8_t mission)
{
	if (!transport.host
		|| transport.state
			!= MultiplayerTransportState::hosting_lobby
		|| transport.lobby.rules.mode
			!= MultiplayerSessionMode::cooperative
		|| mission == 0
		|| mission != transport.lobby.rules.mission
		|| !valid_utf8(
			mission_name, kMultiplayerCoopMissionNameBytes)
		|| !valid_utf8(
			presentation_title,
			kMultiplayerCoopMissionTitleBytes))
	{
		transport.last_error =
			MultiplayerTransportError::invalid_lobby;
		return false;
	}
#if defined(__EMSCRIPTEN__)
	(void)mission_name;
	(void)presentation_title;
	return false;
#else
	MultiplayerLobbySnapshot proposed = transport.lobby;
	proposed.coop_selection = {};
	if (!copy_text(
			proposed.coop_selection.mission_name,
			mission_name)
		|| !copy_text(
			proposed.coop_selection.presentation_title,
			presentation_title))
	{
		transport.last_error =
			MultiplayerTransportError::invalid_lobby;
		return false;
	}
	proposed.coop_selection.mission = mission;
	proposed.coop_selection.present = true;
	if (!valid_lobby(proposed)
		|| !broadcast_lobby_snapshot(transport, proposed))
	{
		transport.last_error =
			MultiplayerTransportError::queue_full;
		return false;
	}
	transport.lobby = proposed;
	mark_authority_snapshot_dirty(transport);
	transport.last_error = MultiplayerTransportError::none;
	return true;
#endif
}

bool multiplayer_transport_set_personal_campaign(
	MultiplayerTransport& transport,
	const game::MultiplayerCampaignLaunchState& campaign_state)
{
	if (!transport.initialized)
	{
		multiplayer_transport_initialize(transport);
	}
#if defined(__EMSCRIPTEN__)
	(void)campaign_state;
	transport.last_error =
		MultiplayerTransportError::unsupported_platform;
	return false;
#else
	const bool active_lobby =
		transport.state
				== MultiplayerTransportState::hosting_lobby
		|| transport.state
				== MultiplayerTransportState::client_lobby;
	const bool preconnection =
		transport.state == MultiplayerTransportState::idle
		|| transport.state
			== MultiplayerTransportState::discovering
		|| transport.state
			== MultiplayerTransportState::connecting
		|| transport.state
			== MultiplayerTransportState::disconnected
		|| transport.state
			== MultiplayerTransportState::error;
	const bool deathmatch = active_lobby
		&& transport.lobby.rules.mode
			== MultiplayerSessionMode::deathmatch;
	if ((!active_lobby && !preconnection)
		|| (deathmatch
			&& (campaign_state.present
				|| campaign_state.mission_25_alternate))
		|| (!deathmatch
			&& (!campaign_state.present
				|| !valid_campaign_state(
					campaign_state.state, true)
				|| (campaign_state.mission_25_alternate
					&& campaign_state.state.mission
						!= 25))))
	{
		transport.last_error =
			MultiplayerTransportError::invalid_lobby;
		return false;
	}
	transport.prelaunch_campaign = campaign_state;
	transport.last_error = MultiplayerTransportError::none;
	return true;
#endif
}

bool multiplayer_transport_set_session_name(
	MultiplayerTransport& transport,
	const char* session_name)
{
	if (!transport.host
		|| transport.state
			!= MultiplayerTransportState::hosting_lobby
		|| transport.lobby.rules.mode
			!= MultiplayerSessionMode::cooperative)
	{
		transport.last_error =
			MultiplayerTransportError::not_authority;
		return false;
	}
	if (!valid_utf8(session_name, kMultiplayerSessionNameBytes)
		|| bounded_text_length(
			session_name, kMultiplayerSessionNameBytes)
			> kMultiplayerRetailSessionNameCharacters)
	{
		transport.last_error =
			MultiplayerTransportError::invalid_lobby;
		return false;
	}
#if defined(__EMSCRIPTEN__)
	(void)session_name;
	return false;
#else
	MultiplayerLobbySnapshot proposed = transport.lobby;
	if (!copy_text(proposed.session_name, session_name)
		|| !valid_lobby(proposed)
		|| !broadcast_lobby_snapshot(transport, proposed))
	{
		transport.last_error =
			MultiplayerTransportError::queue_full;
		return false;
	}
	transport.lobby = proposed;
	mark_authority_snapshot_dirty(transport);
	transport.last_error = MultiplayerTransportError::none;
	return true;
#endif
}

bool multiplayer_transport_submit_lobby_chat(
	MultiplayerTransport& transport,
	const char* text)
{
	if ((transport.state
			!= MultiplayerTransportState::hosting_lobby
			&& transport.state
				!= MultiplayerTransportState::client_lobby)
			|| !valid_utf8(text, mission::kNetworkChatBytes)
			|| transport.local_lobby_slot
				>= kMultiplayerTransportPlayerCapacity
			|| transport.lobby_chat_count
				>= kMultiplayerLobbyChatCapacity)
	{
		return false;
	}
	char bounded[mission::kNetworkChatBytes]{};
	if (!copy_text(bounded, text))
	{
		return false;
	}
#if !defined(__EMSCRIPTEN__)
	bool sent = false;
	if (transport.host)
	{
		sent = broadcast_tcp_packet(
			transport,
			PacketType::lobby_chat,
			[&](Writer& writer)
			{
				writer.u8(transport.local_lobby_slot);
				writer.text(bounded);
			},
			[](const detail::MultiplayerTransportPeer&)
			{
				return true;
			});
	}
	else
	{
		detail::MultiplayerTransportPeer& server =
			transport.peers[0];
		sent = server.occupied
			&& server.authenticated
			&& queue_lobby_chat_tcp(
				server,
				transport.lobby.session_id,
				transport.local_lobby_slot,
				bounded);
	}
	return sent
		&& queue_lobby_chat(
			transport, transport.local_lobby_slot, bounded);
#else
	return false;
#endif
}

bool multiplayer_transport_pop_lobby_chat(
	MultiplayerTransport& transport,
	MultiplayerLobbyChat& message)
{
	if (transport.lobby_chat_count == 0)
	{
		return false;
	}
	message =
		transport.lobby_chat[transport.lobby_chat_read];
	transport.lobby_chat[transport.lobby_chat_read] = {};
	transport.lobby_chat_read = static_cast<std::uint8_t>(
		(transport.lobby_chat_read + 1)
		% kMultiplayerLobbyChatCapacity);
	--transport.lobby_chat_count;
	if (transport.lobby_chat_count == 0)
	{
		transport.lobby_chat_read = 0;
	}
	return true;
}

bool multiplayer_transport_host_kick(
	MultiplayerTransport& transport,
	std::uint8_t lobby_slot)
{
	if (!transport.host
		|| lobby_slot == transport.local_lobby_slot
		|| lobby_slot >= kMultiplayerTransportPlayerCapacity)
	{
		transport.last_error =
			MultiplayerTransportError::not_authority;
		return false;
	}
#if !defined(__EMSCRIPTEN__)
	detail::MultiplayerTransportPeer* const peer =
		peer_for_lobby_slot(transport, lobby_slot);
	if (peer == nullptr)
	{
		return false;
	}
	(void)queue_tcp_packet(
		*peer,
		PacketType::disconnect,
		transport.lobby.session_id,
		[&](Writer& writer)
		{
			writer.u8(static_cast<std::uint8_t>(
				MultiplayerDepartureReason::kicked));
		});
	(void)flush_peer(*peer, transport.started_at);
	std::uint8_t peer_index = UINT8_MAX;
	for (std::uint8_t index = 0;
		index < kMultiplayerTransportPlayerCapacity;
		++index)
	{
		if (&transport.peers[index] == peer)
		{
			peer_index = index;
			break;
		}
	}
	if (peer_index == UINT8_MAX)
	{
		return false;
	}
	remove_host_peer(
		transport,
		peer_index,
		MultiplayerDepartureReason::kicked);
	return true;
#else
	return false;
#endif
}

bool multiplayer_transport_begin_prelaunch(
	MultiplayerTransport& transport,
	const game::MultiplayerCampaignLaunchState& campaign)
{
	if (!transport.host)
	{
		transport.last_error =
			MultiplayerTransportError::not_authority;
		return false;
	}
	const bool ordinary_cooperative_start =
		transport.state
			== MultiplayerTransportState::hosting_lobby
		&& transport.lobby.rules.mode
			== MultiplayerSessionMode::cooperative;
	const bool mission_25_alternate_start =
		transport.state == MultiplayerTransportState::gameplay
		&& transport.launch_snapshot_valid
		&& !transport.launch_snapshot.deathmatch_mode
		&& std::any_of(
			std::begin(transport.mission_result.reports),
			std::end(transport.mission_result.reports),
			[&](const MultiplayerPlayerMissionReport& report)
			{
				return report.valid
					&& report.outcome
						== MultiplayerPlayerMissionOutcome::
							survived
					&& report.campaign.mission_25_alternate
					&& campaign_launch_equal(
						campaign, report.campaign);
			});
	if ((!ordinary_cooperative_start
			&& !mission_25_alternate_start)
		|| !valid_lobby(transport.lobby)
		|| !transport.lobby.coop_selection.present
		|| !valid_campaign_launch(
			campaign, transport.lobby.rules)
		|| !multiplayer_transport_udp_ready(transport))
	{
		transport.last_error =
			MultiplayerTransportError::invalid_lobby;
		return false;
	}
#if defined(__EMSCRIPTEN__)
	(void)campaign;
	return false;
#else
	game::MultiplayerMissionBootstrap bootstrap =
		transport.shared_bootstrap;
	if (!transport.shared_bootstrap_pending)
	{
		const game::MultiplayerMissionBootstrap* const continuity =
			transport.shared_bootstrap.present
				? &transport.shared_bootstrap
				: nullptr;
		if (!materialize_mission_bootstrap(
				transport,
				campaign,
				transport.lobby.rules.mission,
				continuity,
				bootstrap))
		{
			transport.last_error =
				MultiplayerTransportError::invalid_lobby;
			return false;
		}
	}
	else if (!reconcile_pending_bootstrap_roster(
		transport, bootstrap))
	{
		transport.last_error =
			MultiplayerTransportError::invalid_lobby;
		return false;
	}
	MultiplayerLobbySnapshot proposed = transport.lobby;
	for (MultiplayerLobbyPlayer& player : proposed.players)
	{
		if (!player.connected)
		{
			continue;
		}
		player.ready = false;
		player.post_mission_ready = false;
		player.outcome_requires_restart = false;
	}
	if (!broadcast_prelaunch(transport, proposed, campaign))
	{
		transport.last_error =
			MultiplayerTransportError::queue_full;
		return false;
	}
	transport.lobby = proposed;
	transport.prelaunch_campaign = campaign;
	transport.shared_bootstrap = bootstrap;
	transport.shared_bootstrap_pending = true;
	transport.launch_generation = std::max(
		transport.launch_generation,
		bootstrap.launch_generation);
	transport.launch_snapshot = {};
	transport.launch_snapshot_valid = false;
	transport.local_gameplay_player = UINT8_MAX;
	std::fill(
		std::begin(transport.gameplay_lobby_slot),
		std::end(transport.gameplay_lobby_slot),
		UINT8_MAX);
	synchronize_member_gameplay_slots(transport);
	for (detail::MultiplayerTransportPeer& peer :
		transport.peers)
	{
		peer.gameplay_slot = UINT8_MAX;
		peer.post_mission = false;
	}
	transport.state =
		MultiplayerTransportState::host_prelaunch;
	mark_authority_snapshot_dirty(transport);
	transport.last_error = MultiplayerTransportError::none;
	MultiplayerTransportEvent event;
	event.kind = MultiplayerTransportEventKind::prelaunch;
	event.player = transport.local_lobby_slot;
	(void)queue_event(transport, event);
	MultiplayerTransportEvent lobby_event;
	lobby_event.kind =
		MultiplayerTransportEventKind::lobby_updated;
	(void)queue_event(transport, lobby_event);
	return true;
#endif
}

bool multiplayer_transport_submit_prelaunch_loadout(
	MultiplayerTransport& transport,
	const MultiplayerLobbyPlayer& player)
{
	const bool valid_state =
		transport.state
			== MultiplayerTransportState::host_prelaunch
		|| transport.state
			== MultiplayerTransportState::client_prelaunch;
	if (!valid_state
		|| transport.local_lobby_slot
			>= kMultiplayerTransportPlayerCapacity)
	{
		transport.last_error =
			MultiplayerTransportError::invalid_state;
		return false;
	}
	const MultiplayerLobbyPlayer& current =
		transport.lobby.players[transport.local_lobby_slot];
	MultiplayerLobbyPlayer proposed = player;
	proposed.connected = true;
	proposed.ready = true;
	proposed.post_mission_ready = false;
	proposed.outcome_requires_restart = false;
	proposed.latency = current.latency;
	proposed.one_way_latency = current.one_way_latency;
	proposed.deathmatch_kills =
		current.deathmatch_kills;
	proposed.deathmatch_deaths =
		current.deathmatch_deaths;
	if (!current.connected
		|| proposed.team != current.team
		|| std::strncmp(
			proposed.name,
			current.name,
			sizeof(proposed.name)) != 0
		|| !valid_player(
			proposed,
			true,
			transport.lobby.rules.team_mode))
	{
		transport.last_error =
			MultiplayerTransportError::invalid_lobby;
		return false;
	}
#if defined(__EMSCRIPTEN__)
	return false;
#else
	if (transport.host)
	{
		MultiplayerLobbySnapshot lobby = transport.lobby;
		lobby.players[
			transport.local_lobby_slot] = proposed;
		if (!broadcast_lobby_snapshot(transport, lobby))
		{
			transport.last_error =
				MultiplayerTransportError::queue_full;
			return false;
		}
		transport.lobby = lobby;
		mark_authority_snapshot_dirty(transport);
	}
	else
	{
		detail::MultiplayerTransportPeer& server =
			transport.peers[0];
		if (!server.occupied
			|| !server.authenticated
			|| !queue_tcp_packet(
				server,
				PacketType::prelaunch_loadout,
				transport.lobby.session_id,
				[&](Writer& writer)
				{
					writer.u8(
						transport.local_lobby_slot);
					write_player(writer, proposed);
				}))
		{
			transport.last_error =
				MultiplayerTransportError::queue_full;
			return false;
		}
		transport.lobby.players[
			transport.local_lobby_slot] = proposed;
	}
	transport.last_error = MultiplayerTransportError::none;
	return true;
#endif
}

bool multiplayer_transport_start_game(
	MultiplayerTransport& transport)
{
	const bool cooperative_prelaunch =
		transport.state
			== MultiplayerTransportState::host_prelaunch
		&& transport.lobby.rules.mode
			== MultiplayerSessionMode::cooperative;
	const bool deathmatch_lobby =
		transport.state
			== MultiplayerTransportState::hosting_lobby
		&& transport.lobby.rules.mode
			== MultiplayerSessionMode::deathmatch;
	if (!transport.host
		|| (!cooperative_prelaunch && !deathmatch_lobby))
	{
		transport.last_error =
			MultiplayerTransportError::not_authority;
		return false;
	}
#if defined(__EMSCRIPTEN__)
	return false;
#else
	if (cooperative_prelaunch)
	{
		game::MultiplayerMissionBootstrap finalized =
			transport.shared_bootstrap;
		if (!reconcile_pending_bootstrap_roster(
				transport, finalized))
		{
			transport.last_error =
				MultiplayerTransportError::invalid_lobby;
			return false;
		}
		transport.shared_bootstrap = finalized;
	}
	if (!valid_lobby(transport.lobby)
		|| !valid_campaign_launch(
			transport.prelaunch_campaign,
			transport.lobby.rules)
		|| !multiplayer_transport_udp_ready(transport))
	{
		transport.last_error =
			MultiplayerTransportError::invalid_lobby;
		return false;
	}
	for (const MultiplayerLobbyPlayer& player :
		transport.lobby.players)
	{
		if (player.connected
			&& (!player.ready
				|| !valid_player(
					player,
					true,
					transport.lobby.rules.team_mode)))
		{
			transport.last_error =
				MultiplayerTransportError::invalid_lobby;
			return false;
		}
	}
	game::MultiplayerLaunchSnapshot host_launch;
	if (!make_launch_snapshot(
			transport,
			transport.local_lobby_slot,
			game::MultiplayerRole::host,
			host_launch))
	{
		transport.last_error =
			MultiplayerTransportError::invalid_lobby;
		return false;
	}
	std::uint8_t gameplay_lobby_slot[
		kMultiplayerTransportPlayerCapacity];
	if (!build_gameplay_lobby_mapping(
		transport.lobby,
		host_launch.player_count,
		gameplay_lobby_slot))
	{
		transport.last_error =
			MultiplayerTransportError::invalid_lobby;
		return false;
	}
	std::uint8_t payload[
		kMultiplayerTransportPlayerCapacity][
			detail::kWirePacketBytes]{};
	std::size_t payload_size[
		kMultiplayerTransportPlayerCapacity]{};
	for (std::uint8_t index = 0;
		index < kMultiplayerTransportPlayerCapacity;
		++index)
	{
		const detail::MultiplayerTransportPeer& peer =
			transport.peers[index];
		if (!peer.occupied || !peer.authenticated)
		{
			continue;
		}
		game::MultiplayerLaunchSnapshot client_launch;
		if (!make_launch_snapshot(
			transport,
				peer.lobby_slot,
				game::MultiplayerRole::client,
				client_launch)
			|| !encode_packet(
				payload[index],
				payload_size[index],
				PacketType::launch,
				transport.lobby.session_id,
				[&](Writer& writer)
				{
					write_launch(writer, client_launch);
				})
			|| !can_queue_tcp_payload(
				peer, payload_size[index]))
		{
			transport.last_error =
				MultiplayerTransportError::queue_full;
			return false;
		}
	}
	for (std::uint8_t index = 0;
		index < kMultiplayerTransportPlayerCapacity;
		++index)
	{
		detail::MultiplayerTransportPeer& peer =
			transport.peers[index];
		if (peer.occupied
			&& peer.authenticated
			&& !queue_tcp_payload(
				peer,
				payload[index],
				payload_size[index]))
		{
			transport.last_error =
				MultiplayerTransportError::queue_full;
			return false;
		}
	}
	for (MultiplayerLobbyPlayer& player :
		transport.lobby.players)
	{
		player.post_mission_ready = false;
		player.outcome_requires_restart = false;
		player.deathmatch_kills = 0;
		player.deathmatch_deaths = 0;
	}
	transport.launch_snapshot = host_launch;
	transport.launch_snapshot_valid = true;
	transport.shared_bootstrap = host_launch.bootstrap;
	transport.replay_bootstrap = host_launch.bootstrap;
	transport.shared_bootstrap_pending = false;
	if (host_launch.bootstrap.present)
	{
		transport.launch_generation = std::max(
			transport.launch_generation,
			host_launch.bootstrap.launch_generation);
	}
	std::copy(
		std::begin(gameplay_lobby_slot),
		std::end(gameplay_lobby_slot),
		std::begin(transport.gameplay_lobby_slot));
	transport.local_gameplay_player =
		host_launch.local_player;
	synchronize_member_gameplay_slots(transport);
	for (detail::MultiplayerTransportPeer& peer :
		transport.peers)
	{
		peer.gameplay_slot = UINT8_MAX;
		peer.post_mission = false;
		if (!peer.occupied || !peer.authenticated)
		{
			continue;
		}
		for (std::uint8_t gameplay_player = 0;
			gameplay_player < host_launch.player_count;
			++gameplay_player)
		{
			if (transport.gameplay_lobby_slot[
					gameplay_player]
				== peer.lobby_slot)
			{
				peer.gameplay_slot = gameplay_player;
				break;
			}
		}
	}
	transport.mission_result = {};
	transport.state = MultiplayerTransportState::gameplay;
	mark_authority_snapshot_dirty(transport);
	transport.last_error = MultiplayerTransportError::none;
	MultiplayerTransportEvent event;
	event.kind = MultiplayerTransportEventKind::launch;
	event.player = host_launch.local_player;
	(void)queue_event(transport, event);
	return true;
#endif
}

bool multiplayer_transport_launch_snapshot(
	const MultiplayerTransport& transport,
	game::MultiplayerLaunchSnapshot& snapshot)
{
	if (!transport.launch_snapshot_valid)
	{
		return false;
	}
	snapshot = transport.launch_snapshot;
	return true;
}

const game::MultiplayerCampaignLaunchState&
multiplayer_transport_prelaunch_campaign(
	const MultiplayerTransport& transport)
{
	return transport.prelaunch_campaign;
}

bool multiplayer_transport_update_mission_bootstrap(
	MultiplayerTransport& transport,
	const game::MultiplayerMissionBootstrap& bootstrap)
{
#if defined(__EMSCRIPTEN__)
	(void)bootstrap;
	transport.last_error =
		MultiplayerTransportError::unsupported_platform;
	return false;
#else
	if ((transport.state != MultiplayerTransportState::gameplay
			&& transport.state
				!= MultiplayerTransportState::post_mission)
		|| !transport.launch_snapshot_valid
		|| transport.launch_snapshot.deathmatch_mode
		|| !bootstrap.present
		|| bootstrap.launch_generation
			!= transport.launch_snapshot.bootstrap
				.launch_generation
		|| bootstrap.mission
			!= transport.launch_snapshot.authoritative_mission
		|| !game::valid_multiplayer_mission_bootstrap(
			bootstrap,
			transport.launch_snapshot.authoritative_mission,
			transport.launch_snapshot.player_count,
			false))
	{
		transport.last_error =
			MultiplayerTransportError::invalid_state;
		return false;
	}
	if (mission_bootstrap_equal(
		transport.shared_bootstrap, bootstrap))
	{
		transport.last_error =
			MultiplayerTransportError::none;
		return true;
	}
	transport.shared_bootstrap = bootstrap;
	mark_authority_snapshot_dirty(transport);
	transport.last_error = MultiplayerTransportError::none;
	return true;
#endif
}

const game::MultiplayerMissionBootstrap&
multiplayer_transport_mission_bootstrap(
	const MultiplayerTransport& transport)
{
	return transport.shared_bootstrap;
}

const MultiplayerLobbySnapshot& multiplayer_transport_lobby(
	const MultiplayerTransport& transport)
{
	return transport.lobby;
}

std::uint8_t multiplayer_transport_lobby_slot_for_gameplay_player(
	const MultiplayerTransport& transport,
	std::uint8_t gameplay_player)
{
	return gameplay_player < kMultiplayerTransportPlayerCapacity
		? transport.gameplay_lobby_slot[gameplay_player]
		: UINT8_MAX;
}

std::uint8_t multiplayer_transport_local_gameplay_player(
	const MultiplayerTransport& transport)
{
	return transport.local_gameplay_player;
}

bool multiplayer_transport_gameplay_player_latency(
	const MultiplayerTransport& transport,
	std::uint8_t gameplay_player,
	MultiplayerGameplayPlayerLatency& latency)
{
	latency = {};
	if (!transport.launch_snapshot_valid
		|| gameplay_player >= transport.launch_snapshot.player_count
		|| !transport.launch_snapshot.players[
			gameplay_player].connected)
	{
		return false;
	}
	const std::uint8_t lobby_slot =
		transport.gameplay_lobby_slot[gameplay_player];
	if (lobby_slot >= kMultiplayerTransportPlayerCapacity
		|| !transport.lobby.players[lobby_slot].connected
		|| !transport.members[lobby_slot].connected
		|| transport.members[lobby_slot].gameplay_slot
			!= gameplay_player)
	{
		return false;
	}
	const MultiplayerLobbyPlayer& player =
		transport.lobby.players[lobby_slot];
	latency.smoothed_one_way_ticks = player.latency;
	latency.raw_one_way_ticks = player.one_way_latency;
	return true;
}

bool multiplayer_transport_gameplay_writable(
	const MultiplayerTransport& transport,
	const mission::NetworkOutboundMessage& message)
{
	if ((transport.state
			!= MultiplayerTransportState::gameplay
			&& transport.state
				!= MultiplayerTransportState::post_mission)
		|| !transport.launch_snapshot_valid
		|| transport.local_gameplay_player
			>= kMultiplayerTransportPlayerCapacity)
	{
		return false;
	}
#if defined(__EMSCRIPTEN__)
	(void)message;
	return false;
#else
	mission::NetworkOutboundMessage authenticated = message;
	if (!validate_authenticated_gameplay(
		transport,
		authenticated,
		transport.local_gameplay_player))
	{
		return false;
	}
	if (conditional_delivery(authenticated.delivery))
	{
		return true;
	}
	std::uint8_t bytes[detail::kWirePacketBytes];
	std::size_t count = 0;
	if (!encode_packet(
		bytes,
		count,
		PacketType::gameplay,
		transport.lobby.session_id,
		[&](Writer& writer)
		{
			write_semantic_message(writer, authenticated);
		}))
	{
		return false;
	}
	if (!transport.host)
	{
		const detail::MultiplayerTransportPeer& server =
			transport.peers[0];
		return server.occupied
			&& server.authenticated
			&& can_queue_tcp_payload(server, count);
	}
	for (std::uint8_t gameplay_player = 0;
		gameplay_player
			< transport.launch_snapshot.player_count;
		++gameplay_player)
	{
		if (gameplay_player
				== transport.local_gameplay_player
			|| !transport.launch_snapshot.players[
				gameplay_player].connected
			|| (authenticated.destination_player
					!= kBroadcastPlayer
				&& authenticated.destination_player
					!= gameplay_player))
		{
			continue;
		}
		const std::uint8_t lobby_slot =
			transport.gameplay_lobby_slot[gameplay_player];
		const detail::MultiplayerTransportPeer* const peer =
			peer_for_lobby_slot(transport, lobby_slot);
		if (peer == nullptr
			|| !can_queue_tcp_payload(*peer, count))
		{
			return false;
		}
	}
	return true;
#endif
}

bool multiplayer_transport_submit_gameplay(
	MultiplayerTransport& transport,
	const mission::NetworkOutboundMessage& message)
{
	if ((transport.state
			!= MultiplayerTransportState::gameplay
			&& transport.state
				!= MultiplayerTransportState::post_mission)
			|| !transport.launch_snapshot_valid
			|| transport.local_gameplay_player
			>= kMultiplayerTransportPlayerCapacity)
	{
		transport.last_error =
			MultiplayerTransportError::invalid_state;
		return false;
	}
#if defined(__EMSCRIPTEN__)
	(void)message;
	return false;
#else
	mission::NetworkOutboundMessage authenticated = message;
	if (!validate_authenticated_gameplay(
			transport,
			authenticated,
			transport.local_gameplay_player))
	{
		transport.last_error =
			MultiplayerTransportError::protocol_violation;
		return false;
	}
	if (transport.host)
	{
		const bool queued = relay_gameplay_from_host(
			transport,
			authenticated,
			transport.local_gameplay_player);
		if (queued)
		{
			retain_deathmatch_player_stats(
				transport, authenticated);
		}
		transport.last_error = queued
			? MultiplayerTransportError::none
			: MultiplayerTransportError::queue_full;
		return queued;
	}
	detail::MultiplayerTransportPeer& server =
		transport.peers[0];
	if (!server.occupied || !server.authenticated)
	{
		transport.last_error =
			MultiplayerTransportError::connection_lost;
		return false;
	}
	if (conditional_delivery(authenticated.delivery))
	{
		if (server.udp_endpoint_known)
		{
			(void)queue_gameplay_udp(
				transport, server, authenticated);
		}
		retain_deathmatch_player_stats(
			transport, authenticated);
		transport.last_error = MultiplayerTransportError::none;
		return true;
	}
	const bool queued = queue_gameplay_tcp(
		server, transport.lobby.session_id, authenticated);
	if (queued)
	{
		retain_deathmatch_player_stats(
			transport, authenticated);
	}
	transport.last_error = queued
		? MultiplayerTransportError::none
		: MultiplayerTransportError::queue_full;
	return queued;
#endif
}

bool multiplayer_transport_pop_gameplay(
	MultiplayerTransport& transport,
	mission::NetworkOutboundMessage& message)
{
	if (transport.gameplay_count == 0)
	{
		return false;
	}
	message = transport.gameplay[transport.gameplay_read];
	transport.gameplay[transport.gameplay_read] = {};
	transport.gameplay_read = static_cast<std::uint8_t>(
		(transport.gameplay_read + 1)
		% kMultiplayerGameplayQueueCapacity);
	--transport.gameplay_count;
	if (transport.gameplay_count == 0)
	{
		transport.gameplay_read = 0;
	}
	return true;
}

bool multiplayer_transport_report_player_mission_outcome(
	MultiplayerTransport& transport,
	MultiplayerPlayerMissionOutcome outcome)
{
	if ((transport.state
			!= MultiplayerTransportState::gameplay
			&& transport.state
				!= MultiplayerTransportState::post_mission)
		|| !transport.launch_snapshot_valid
		|| transport.launch_snapshot.deathmatch_mode
		|| transport.local_gameplay_player
			>= transport.launch_snapshot.player_count
		|| transport.local_lobby_slot
			>= kMultiplayerTransportPlayerCapacity
		|| !reportable_player_mission_outcome(outcome))
	{
		transport.last_error =
			MultiplayerTransportError::invalid_state;
		return false;
	}
	const std::uint8_t gameplay_player =
		transport.local_gameplay_player;
	const MultiplayerPlayerMissionOutcome current =
		transport.mission_result.players[gameplay_player];
	if (current == outcome)
	{
		return true;
	}
	if (current != MultiplayerPlayerMissionOutcome::none)
	{
		transport.last_error =
			MultiplayerTransportError::protocol_violation;
		return false;
	}
#if defined(__EMSCRIPTEN__)
	return false;
#else
	if (transport.host)
	{
		if (!broadcast_player_mission_outcome(
			transport,
			gameplay_player,
			transport.local_lobby_slot,
			outcome,
			UINT8_MAX))
		{
			transport.last_error =
				MultiplayerTransportError::queue_full;
			return false;
		}
		apply_player_mission_outcome(
			transport,
			gameplay_player,
			transport.local_lobby_slot,
			outcome);
	}
	else
	{
		detail::MultiplayerTransportPeer& server =
			transport.peers[0];
		if (!server.occupied
			|| !server.authenticated
			|| !queue_tcp_packet(
				server,
				PacketType::player_mission_outcome,
				transport.lobby.session_id,
				[&](Writer& writer)
				{
					writer.u8(gameplay_player);
					writer.u8(
						transport.local_lobby_slot);
					writer.u8(
						static_cast<std::uint8_t>(
							outcome));
				}))
		{
			transport.last_error =
				MultiplayerTransportError::queue_full;
			return false;
		}
	}
	transport.last_error = MultiplayerTransportError::none;
	return true;
#endif
}

bool multiplayer_transport_publish_mission_result(
	MultiplayerTransport& transport,
	const game::SessionResult& result,
	const game::MultiplayerCampaignLaunchState& campaign_state,
	const campaign::AdvanceResult& advance,
	MultiplayerPlayerMissionOutcome outcome)
{
	if ((transport.state
			!= MultiplayerTransportState::gameplay
			&& transport.state
				!= MultiplayerTransportState::post_mission)
		|| !transport.launch_snapshot_valid
		|| transport.launch_snapshot.deathmatch_mode
		|| transport.local_gameplay_player
			>= transport.launch_snapshot.player_count
		|| transport.local_lobby_slot
			>= kMultiplayerTransportPlayerCapacity)
	{
		transport.last_error =
			MultiplayerTransportError::invalid_state;
		return false;
	}
	const std::uint8_t gameplay_player =
		transport.local_gameplay_player;
	if (outcome == MultiplayerPlayerMissionOutcome::none)
	{
		outcome =
			transport.mission_result.players[gameplay_player];
		if (outcome == MultiplayerPlayerMissionOutcome::none)
		{
			outcome = outcome_for_mission_report(
				result, campaign_state, advance);
		}
	}
	MultiplayerPlayerMissionReport report;
	report.result = result;
	report.campaign = campaign_state;
	report.advance = advance;
	report.outcome = outcome;
	report.lobby_slot = transport.local_lobby_slot;
	report.valid = true;
	if (!valid_player_mission_report(
			report, transport.launch_snapshot))
	{
		transport.last_error =
			MultiplayerTransportError::protocol_violation;
		return false;
	}
	const MultiplayerPlayerMissionReport& retained =
		transport.mission_result.reports[gameplay_player];
	if (retained.valid)
	{
		return player_mission_report_equal(retained, report);
	}
	const MultiplayerPlayerMissionOutcome current =
		transport.mission_result.players[gameplay_player];
	if (current != MultiplayerPlayerMissionOutcome::none
		&& current != report.outcome)
	{
		transport.last_error =
			MultiplayerTransportError::protocol_violation;
		return false;
	}
#if defined(__EMSCRIPTEN__)
	return false;
#else
	bool published = false;
	if (transport.host)
	{
		published = broadcast_player_mission_report(
			transport,
			gameplay_player,
			transport.local_lobby_slot,
			report,
			UINT8_MAX);
	}
	else
	{
		detail::MultiplayerTransportPeer& server =
			transport.peers[0];
		published = server.occupied
			&& server.authenticated
			&& queue_tcp_packet(
				server,
				PacketType::mission_result,
				transport.lobby.session_id,
				[&](Writer& writer)
				{
					writer.u8(static_cast<std::uint8_t>(
						MissionResultPayload::
							player_report));
					writer.u8(gameplay_player);
					writer.u8(
						transport.local_lobby_slot);
					write_player_mission_report(
						writer, report);
				});
	}
	if (!published)
	{
		transport.last_error =
			MultiplayerTransportError::queue_full;
		return false;
	}
	if (!apply_player_mission_report(
		transport,
		gameplay_player,
		transport.local_lobby_slot,
		report))
	{
		transport.last_error =
			MultiplayerTransportError::protocol_violation;
		return false;
	}
	transport.last_error = MultiplayerTransportError::none;
	return true;
#endif
}

bool multiplayer_transport_publish_deathmatch_result(
	MultiplayerTransport& transport,
	const std::int32_t (&kills)[
		kMultiplayerTransportPlayerCapacity],
	const std::int32_t (&deaths)[
		kMultiplayerTransportPlayerCapacity],
	bool network_aborted)
{
	if (!transport.host)
	{
		transport.last_error =
			MultiplayerTransportError::not_authority;
		return false;
	}
	if (transport.state
			!= MultiplayerTransportState::gameplay
		|| !transport.launch_snapshot_valid
		|| !transport.launch_snapshot.deathmatch_mode
		|| transport.mission_result.authoritative_valid)
	{
		transport.last_error =
			MultiplayerTransportError::invalid_state;
		return false;
	}
	MultiplayerMissionResultSnapshot proposed;
	proposed.authoritative_valid = true;
	proposed.network_aborted = network_aborted;
	proposed.deathmatch_scores_valid = true;
	for (std::uint8_t player = 0;
		player < kMultiplayerTransportPlayerCapacity;
		++player)
	{
		const bool retain_departed_score =
			player < transport.launch_snapshot.player_count
			&& !transport.launch_snapshot.players[
				player].connected;
		proposed.deathmatch_kills[player] =
			retain_departed_score
				? transport.mission_result
					.deathmatch_kills[player]
				: kills[player];
		proposed.deathmatch_deaths[player] =
			retain_departed_score
				? transport.mission_result
					.deathmatch_deaths[player]
				: deaths[player];
	}
	if (!valid_mission_result_snapshot(
		proposed, transport.launch_snapshot))
	{
		transport.last_error =
			MultiplayerTransportError::protocol_violation;
		return false;
	}
#if defined(__EMSCRIPTEN__)
	return false;
#else
	if (!broadcast_tcp_packet(
		transport,
		PacketType::mission_result,
		[&](Writer& writer)
		{
			writer.u8(static_cast<std::uint8_t>(
				MissionResultPayload::deathmatch_scores));
			write_mission_result_snapshot(
				writer, proposed);
		},
		[](const detail::MultiplayerTransportPeer&)
		{
			return true;
		}))
	{
		transport.last_error =
			MultiplayerTransportError::queue_full;
		return false;
	}
	transport.mission_result = proposed;
	for (std::uint8_t player = 0;
		player < transport.launch_snapshot.player_count;
		++player)
	{
		const std::uint8_t lobby_slot =
			transport.gameplay_lobby_slot[player];
		if (lobby_slot >= kMultiplayerTransportPlayerCapacity)
		{
			continue;
		}
		transport.lobby.players[
			lobby_slot].deathmatch_kills =
			proposed.deathmatch_kills[player];
		transport.lobby.players[
			lobby_slot].deathmatch_deaths =
			proposed.deathmatch_deaths[player];
		transport.lobby.players[
			lobby_slot].post_mission_ready = false;
		transport.lobby.players[
			lobby_slot].outcome_requires_restart = false;
	}
	for (detail::MultiplayerTransportPeer& peer :
		transport.peers)
	{
		peer.post_mission = false;
	}
	for (detail::MultiplayerTransportMember& member :
		transport.members)
	{
		member.post_mission = false;
	}
	transport.state =
		MultiplayerTransportState::hosting_lobby;
	transport.launch_snapshot_valid = false;
	transport.local_gameplay_player = UINT8_MAX;
	std::fill(
		std::begin(transport.gameplay_lobby_slot),
		std::end(transport.gameplay_lobby_slot),
		UINT8_MAX);
	synchronize_member_gameplay_slots(transport);
	mark_authority_snapshot_dirty(transport);
	MultiplayerTransportEvent event;
	event.kind =
		MultiplayerTransportEventKind::mission_result;
	(void)queue_event(transport, event);
	transport.last_error = MultiplayerTransportError::none;
	return true;
#endif
}

const MultiplayerMissionResultSnapshot&
multiplayer_transport_mission_result(
	const MultiplayerTransport& transport)
{
	return transport.mission_result;
}

const MultiplayerPlayerMissionReport*
multiplayer_transport_player_mission_report(
	const MultiplayerTransport& transport,
	std::uint8_t gameplay_player)
{
	if (gameplay_player
		>= kMultiplayerTransportPlayerCapacity
		|| !transport.mission_result.reports[
			gameplay_player].valid)
	{
		return nullptr;
	}
	return &transport.mission_result.reports[
		gameplay_player];
}

const MultiplayerPlayerMissionReport*
multiplayer_transport_local_mission_report(
	const MultiplayerTransport& transport)
{
	return mission_report_for_lobby_slot(
		transport.mission_result,
		transport.local_lobby_slot);
}

MultiplayerDebriefParticipantView
multiplayer_transport_debrief_participants(
	const MultiplayerTransport& transport)
{
	MultiplayerDebriefParticipantView view;
	if (!transport.launch_snapshot_valid
		|| transport.launch_snapshot.deathmatch_mode
		|| transport.mission_result.debrief_leader_player
			>= transport.launch_snapshot.player_count)
	{
		return view;
	}
	for (std::uint8_t gameplay_player = 0;
		gameplay_player < transport.launch_snapshot.player_count;
		++gameplay_player)
	{
		if (!transport.launch_snapshot.players[
				gameplay_player].connected)
		{
			continue;
		}
		const std::uint8_t lobby_slot =
			transport.gameplay_lobby_slot[gameplay_player];
		if (lobby_slot >= kMultiplayerTransportPlayerCapacity
			|| !transport.lobby.players[lobby_slot].connected)
		{
			continue;
		}
		MultiplayerDebriefParticipant& participant =
			view.participants[view.count];
		participant.lobby_player =
			transport.lobby.players[lobby_slot];
		participant.outcome =
			transport.mission_result.players[gameplay_player];
		participant.gameplay_player = gameplay_player;
		participant.lobby_slot = lobby_slot;
		if (gameplay_player
			== transport.mission_result.debrief_leader_player)
		{
			view.leader_participant = view.count;
		}
		++view.count;
	}
	view.leader_valid =
		transport.mission_result.debrief_leader_valid
		&& view.leader_participant != UINT8_MAX;
	return view;
}

bool multiplayer_transport_enter_post_mission(
	MultiplayerTransport& transport)
{
	const MultiplayerPlayerMissionReport* const local_report =
		mission_report_for_lobby_slot(
			transport.mission_result,
			transport.local_lobby_slot);
	if (transport.state
		!= MultiplayerTransportState::gameplay
		|| local_report == nullptr
		|| transport.mission_result.deathmatch_scores_valid
		|| local_report->campaign.mission_25_alternate
		|| local_report->advance.campaign_complete
		|| transport.local_lobby_slot
			>= kMultiplayerTransportPlayerCapacity)
	{
		return false;
	}
	for (std::uint8_t gameplay_player = 0;
		gameplay_player < transport.launch_snapshot.player_count;
		++gameplay_player)
	{
		if (transport.launch_snapshot.players[
				gameplay_player].connected
			&& transport.mission_result.players[
				gameplay_player]
				== MultiplayerPlayerMissionOutcome::none)
		{
			return false;
		}
	}
#if !defined(__EMSCRIPTEN__)
	if (!transport.host)
	{
		detail::MultiplayerTransportPeer& server =
			transport.peers[0];
		if (!queue_tcp_packet(
				server,
				PacketType::post_mission_status,
				transport.lobby.session_id,
				[&](Writer& writer)
				{
					writer.boolean(true);
					writer.boolean(false);
				}))
			{
				transport.last_error =
					MultiplayerTransportError::queue_full;
				return false;
			}
		}
#endif
	transport.state = MultiplayerTransportState::post_mission;
	transport.lobby.players[
		transport.local_lobby_slot].post_mission_ready = false;
	transport.members[
		transport.local_lobby_slot].post_mission = true;
	mark_authority_snapshot_dirty(transport);
	transport.last_error = MultiplayerTransportError::none;
	MultiplayerTransportEvent event;
	event.kind =
		MultiplayerTransportEventKind::post_mission_updated;
	event.player = transport.local_gameplay_player;
	(void)queue_event(transport, event);
	return true;
}

bool multiplayer_transport_set_post_mission_ready(
	MultiplayerTransport& transport,
	bool ready)
{
	if (transport.state
		!= MultiplayerTransportState::post_mission
		|| transport.local_lobby_slot
			>= kMultiplayerTransportPlayerCapacity)
	{
		return false;
	}
#if !defined(__EMSCRIPTEN__)
	if (transport.host)
	{
		MultiplayerLobbySnapshot lobby = transport.lobby;
		lobby.players[
			transport.local_lobby_slot].post_mission_ready =
			ready;
		if (!broadcast_lobby_snapshot(transport, lobby))
		{
			transport.last_error =
				MultiplayerTransportError::queue_full;
			return false;
		}
		transport.lobby = lobby;
		mark_authority_snapshot_dirty(transport);
		transport.last_error =
			MultiplayerTransportError::none;
		return true;
	}
	detail::MultiplayerTransportPeer& server =
		transport.peers[0];
	if (!queue_tcp_packet(
		server,
		PacketType::post_mission_status,
		transport.lobby.session_id,
		[&](Writer& writer)
		{
			writer.boolean(true);
			writer.boolean(ready);
		}))
	{
		transport.last_error =
			MultiplayerTransportError::queue_full;
		return false;
	}
	transport.lobby.players[
		transport.local_lobby_slot].post_mission_ready = ready;
	transport.last_error = MultiplayerTransportError::none;
	return true;
#else
	return false;
#endif
}

bool multiplayer_transport_submit_post_mission_chat(
	MultiplayerTransport& transport,
	const char* text)
{
	if (transport.state
			!= MultiplayerTransportState::post_mission
			|| !valid_utf8(text, mission::kNetworkChatBytes)
			|| transport.local_gameplay_player
				>= kMultiplayerTransportPlayerCapacity
			|| transport.post_mission_chat_count
				>= kMultiplayerPostMissionChatCapacity)
	{
		return false;
	}
	char bounded[mission::kNetworkChatBytes]{};
	if (!copy_text(bounded, text))
	{
		return false;
	}
#if defined(__EMSCRIPTEN__)
	return false;
#else
	bool sent = false;
	if (transport.host)
	{
		sent = broadcast_tcp_packet(
			transport,
			PacketType::post_mission_chat,
			[&](Writer& writer)
			{
				writer.u8(
					transport.local_gameplay_player);
				writer.text(bounded);
			},
			[](const detail::MultiplayerTransportPeer& peer)
			{
				return peer.post_mission;
			});
	}
	else
	{
		detail::MultiplayerTransportPeer& server =
			transport.peers[0];
		sent = server.occupied
			&& server.authenticated
			&& queue_post_chat_tcp(
				server,
				transport.lobby.session_id,
				transport.local_gameplay_player,
				bounded);
	}
	return sent
		&& queue_post_mission_chat(
			transport, transport.local_gameplay_player, bounded);
#endif
}

bool multiplayer_transport_pop_post_mission_chat(
	MultiplayerTransport& transport,
	MultiplayerPostMissionChat& message)
{
	if (transport.post_mission_chat_count == 0)
	{
		return false;
	}
	message = transport.post_mission_chat[
		transport.post_mission_chat_read];
	transport.post_mission_chat[
		transport.post_mission_chat_read] = {};
	transport.post_mission_chat_read =
		static_cast<std::uint8_t>(
			(transport.post_mission_chat_read + 1)
			% kMultiplayerPostMissionChatCapacity);
	--transport.post_mission_chat_count;
	if (transport.post_mission_chat_count == 0)
	{
		transport.post_mission_chat_read = 0;
	}
	return true;
}

bool multiplayer_transport_post_mission_action(
	MultiplayerTransport& transport,
	MultiplayerPostMissionAction action)
{
	const std::uint8_t actor =
		transport.local_gameplay_player;
	if (!post_mission_action_allowed(
		transport, actor, action))
	{
		transport.last_error =
			MultiplayerTransportError::not_authority;
		return false;
	}
#if defined(__EMSCRIPTEN__)
	return false;
#else
	if (transport.host)
	{
		if (!broadcast_post_mission_action(
			transport, actor, action))
		{
			transport.last_error =
				MultiplayerTransportError::queue_full;
			return false;
		}
		if (!apply_post_mission_action(
				transport, actor, action))
		{
			transport.last_error =
				MultiplayerTransportError::protocol_violation;
			return false;
		}
		transport.last_error =
			MultiplayerTransportError::none;
		return true;
	}
	detail::MultiplayerTransportPeer& server =
		transport.peers[0];
	if (!server.occupied
		|| !server.authenticated
		|| !queue_tcp_packet(
			server,
			PacketType::post_mission_action,
			transport.lobby.session_id,
			[&](Writer& writer)
			{
				writer.u8(actor);
				writer.u8(static_cast<std::uint8_t>(action));
			}))
	{
		transport.last_error =
			MultiplayerTransportError::queue_full;
		return false;
	}
	transport.last_error = MultiplayerTransportError::none;
	return true;
#endif
}

bool multiplayer_transport_leave(MultiplayerTransport& transport)
{
	if (!transport.initialized)
	{
		return false;
	}
#if !defined(__EMSCRIPTEN__)
	if (!transport.host
		&& transport.peers[0].occupied
		&& transport.peers[0].authenticated)
	{
		if (transport.launch_snapshot_valid
			&& transport.local_gameplay_player
				< kMultiplayerTransportPlayerCapacity)
		{
			mission::NetworkOutboundMessage departure;
			departure.kind =
				mission::NetworkOutboundKind::gameplay;
			departure.opcode =
				mission::NetworkGameplayOpcode::
					player_departure;
			departure.delivery =
				mission::NetworkDelivery::
					broadcast_guaranteed;
			departure.source_player =
				transport.local_gameplay_player;
			departure.destination_player = kBroadcastPlayer;
			departure.departure_player =
				transport.local_gameplay_player;
			(void)queue_gameplay_tcp(
				transport.peers[0],
				transport.lobby.session_id,
				departure);
		}
		(void)flush_peer(
			transport.peers[0], transport.started_at);
	}
#endif
	multiplayer_transport_shutdown(
		transport,
		transport.host
			? MultiplayerDepartureReason::host_shutdown
			: MultiplayerDepartureReason::leave);
	return true;
}

bool multiplayer_transport_pop_event(
	MultiplayerTransport& transport,
	MultiplayerTransportEvent& event)
{
	if (transport.event_count == 0)
	{
		return false;
	}
	event = transport.events[transport.event_read];
	transport.events[transport.event_read] = {};
	transport.event_read = static_cast<std::uint8_t>(
		(transport.event_read + 1)
		% kMultiplayerEventCapacity);
	--transport.event_count;
	if (transport.event_count == 0)
	{
		transport.event_read = 0;
	}
	return true;
}

bool multiplayer_transport_udp_ready(
	const MultiplayerTransport& transport)
{
	if (!transport.initialized
		|| transport.udp_socket
			== detail::kInvalidSocketHandle)
	{
		return false;
	}
	if (!transport.host)
	{
		return transport.peers[0].occupied
			&& transport.peers[0].authenticated
			&& transport.peers[0].udp_endpoint_known;
	}
	for (std::uint8_t lobby_slot = 0;
		lobby_slot < kMultiplayerTransportPlayerCapacity;
		++lobby_slot)
	{
		if (lobby_slot == transport.local_lobby_slot
			|| !transport.members[lobby_slot].connected)
		{
			continue;
		}
		const detail::MultiplayerTransportPeer* const peer =
			peer_for_lobby_slot(transport, lobby_slot);
		if (peer == nullptr || !peer->udp_endpoint_known)
		{
			return false;
		}
	}
	return true;
}

bool multiplayer_transport_is_host(
	const MultiplayerTransport& transport)
{
	return transport.initialized && transport.host;
}

MultiplayerTransportState multiplayer_transport_state(
	const MultiplayerTransport& transport)
{
	return transport.state;
}

MultiplayerTransportError multiplayer_transport_last_error(
	const MultiplayerTransport& transport)
{
	return transport.last_error;
}
}

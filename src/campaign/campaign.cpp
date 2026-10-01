#include "campaign/campaign.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>

namespace sl_open::campaign
{
namespace
{
constexpr std::size_t kFileBytes = 1024;

struct Writer
{
	std::uint8_t* bytes{};
	std::size_t capacity{};
	std::size_t offset{};
	bool ready{true};
};

struct Reader
{
	const std::uint8_t* bytes{};
	std::size_t size{};
	std::size_t offset{};
	bool ready{true};
};

void write_u8(Writer& writer, std::uint8_t value)
{
	if (!writer.ready || writer.offset >= writer.capacity)
	{
		writer.ready = false;
		return;
	}
	writer.bytes[writer.offset++] = value;
}

void write_u16(Writer& writer, std::uint16_t value)
{
	write_u8(writer, static_cast<std::uint8_t>(value >> 8));
	write_u8(writer, static_cast<std::uint8_t>(value));
}

void write_i16(Writer& writer, std::int16_t value)
{
	write_u16(writer, static_cast<std::uint16_t>(value));
}

void write_u32(Writer& writer, std::uint32_t value)
{
	write_u8(writer, static_cast<std::uint8_t>(value >> 24));
	write_u8(writer, static_cast<std::uint8_t>(value >> 16));
	write_u8(writer, static_cast<std::uint8_t>(value >> 8));
	write_u8(writer, static_cast<std::uint8_t>(value));
}

void write_i32(Writer& writer, std::int32_t value)
{
	write_u32(writer, static_cast<std::uint32_t>(value));
}

void write_bytes(Writer& writer, const void* source, std::size_t size)
{
	if (!writer.ready || source == nullptr
		|| size > writer.capacity - writer.offset)
	{
		writer.ready = false;
		return;
	}
	std::memcpy(writer.bytes + writer.offset, source, size);
	writer.offset += size;
}

std::uint8_t read_u8(Reader& reader)
{
	if (!reader.ready || reader.offset >= reader.size)
	{
		reader.ready = false;
		return 0;
	}
	return reader.bytes[reader.offset++];
}

std::uint16_t read_u16(Reader& reader)
{
	const std::uint16_t high = read_u8(reader);
	const std::uint16_t low = read_u8(reader);
	return static_cast<std::uint16_t>((high << 8) | low);
}

std::int16_t read_i16(Reader& reader)
{
	return static_cast<std::int16_t>(read_u16(reader));
}

std::uint32_t read_u32(Reader& reader)
{
	const std::uint32_t a = read_u8(reader);
	const std::uint32_t b = read_u8(reader);
	const std::uint32_t c = read_u8(reader);
	const std::uint32_t d = read_u8(reader);
	return (a << 24) | (b << 16) | (c << 8) | d;
}

std::int32_t read_i32(Reader& reader)
{
	return static_cast<std::int32_t>(read_u32(reader));
}

void read_bytes(Reader& reader, void* destination, std::size_t size)
{
	if (!reader.ready || destination == nullptr
		|| size > reader.size - reader.offset)
	{
		reader.ready = false;
		return;
	}
	std::memcpy(destination, reader.bytes + reader.offset, size);
	reader.offset += size;
}

void copy_text(char* destination, std::size_t capacity, const char* source)
{
	if (capacity == 0)
	{
		return;
	}
	std::snprintf(destination, capacity, "%s", source == nullptr ? "" : source);
}

void set_default_branch_variables(CampaignState& state)
{
	constexpr std::int32_t defaults[kBranchVariableCount] = {
		1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0,
		0, 1, 0, 0, 0, 0, 1, 1, 1, 1, 0, 1,
	};
	std::copy(std::begin(defaults), std::end(defaults),
		std::begin(state.branch_variables));
}

bool valid_id(const char* id)
{
	if (id == nullptr || std::strlen(id) != kCampaignIdCharacters)
	{
		return false;
	}
	for (std::size_t index = 0; index < kCampaignIdCharacters; ++index)
	{
		const char value = id[index];
		if (!((value >= '0' && value <= '9')
			|| (value >= 'a' && value <= 'f')))
		{
			return false;
		}
	}
	return true;
}

bool build_path(
	const CampaignStore& store,
	const char* kind,
	const char* id,
	std::uint16_t slot,
	bool temporary,
	char* output,
	std::size_t capacity)
{
	if (store.directory[0] == '\0')
	{
		return false;
	}
	int length = 0;
	if (std::strcmp(kind, "index") == 0)
	{
		length = std::snprintf(
			output,
			capacity,
			"%scampaigns.idx%s",
			store.directory,
			temporary ? ".tmp" : "");
	}
	else if (std::strcmp(kind, "profile") == 0 && valid_id(id))
	{
		length = std::snprintf(
			output,
			capacity,
			"%scampaign-%s.profile%s",
			store.directory,
			id,
			temporary ? ".tmp" : "");
	}
	else if (std::strcmp(kind, "save") == 0
		&& valid_id(id)
		&& (slot < kMaxSaveSlots
			|| slot == kCheckpointSaveSlot))
	{
		length = std::snprintf(
			output,
			capacity,
			"%scampaign-%s-save-%02u.sav%s",
			store.directory,
			id,
			slot,
			temporary ? ".tmp" : "");
	}
	return length > 0 && static_cast<std::size_t>(length) < capacity;
}

bool valid_save_slot(std::uint16_t slot)
{
	return slot < kMaxSaveSlots || slot == kCheckpointSaveSlot;
}

bool atomic_write(
	SDL_EMFS_Context* filesystem,
	const char* path,
	const char* temporary,
	const std::uint8_t* bytes,
	std::size_t size)
{
	if (!SDL_EMFS_SaveUserFile(filesystem, temporary, bytes, size))
	{
		SDL_Log(
			"Could not write campaign temporary file %s: %s",
			temporary,
			SDL_GetError());
		return false;
	}
	if (!SDL_EMFS_RenameUserPath(filesystem, temporary, path))
	{
		SDL_Log(
			"Could not replace campaign file %s: %s",
			path,
			SDL_GetError());
		SDL_EMFS_RemoveUserPath(filesystem, temporary);
		return false;
	}
	return true;
}

bool read_file(
	SDL_EMFS_Context* filesystem,
	const char* path,
	std::uint8_t* bytes,
	std::size_t& size)
{
	std::size_t loaded_size = 0;
	void* loaded = SDL_EMFS_LoadFile(
		filesystem, SDL_EMFS_ROOT_USER, path, &loaded_size);
	if (loaded == nullptr || loaded_size > kFileBytes)
	{
		SDL_free(loaded);
		return false;
	}
	std::memcpy(bytes, loaded, loaded_size);
	size = loaded_size;
	SDL_free(loaded);
	return true;
}

void write_magic(Writer& writer, const char (&magic)[5])
{
	write_bytes(writer, magic, 4);
}

bool read_magic(Reader& reader, const char (&magic)[5])
{
	char actual[4];
	read_bytes(reader, actual, sizeof(actual));
	return reader.ready && std::memcmp(actual, magic, sizeof(actual)) == 0;
}

void write_state(Writer& writer, const CampaignState& state)
{
	write_bytes(writer, state.id, kCampaignIdCharacters);
	write_bytes(writer, state.callsign, sizeof(state.callsign));
	write_u8(writer, static_cast<std::uint8_t>(state.difficulty));
	write_u8(writer, static_cast<std::uint8_t>(state.pilot));
	write_u16(writer, state.mission);
	write_i32(writer, state.score);
	write_u8(writer, state.rank);
	write_u8(writer, state.progression);
	for (bool medal : state.medals) write_u8(writer, medal ? 1 : 0);
	for (bool bar : state.bars) write_u8(writer, bar ? 1 : 0);
	for (MissionGrade result : state.mission_results)
	{
		write_i16(writer, static_cast<std::int16_t>(result));
	}
	for (std::uint16_t value : state.mission_score_events)
	{
		write_u16(writer, value);
	}
	for (std::uint16_t value : state.retry_history)
	{
		write_u16(writer, value);
	}
	write_u16(writer, state.retry_count);
	write_i16(writer, state.selected_ship);
	for (std::int16_t item : state.loadout) write_i16(writer, item);
	for (std::uint8_t rank : state.mission_best_ranks)
	{
		write_u8(writer, rank);
	}
	for (std::int32_t value : state.branch_variables)
	{
		write_i32(writer, value);
	}
	write_u32(writer, state.leaderboard_seed);
}

bool read_state(Reader& reader, CampaignState& state)
{
	CampaignState decoded;
	read_bytes(reader, decoded.id, kCampaignIdCharacters);
	decoded.id[kCampaignIdCharacters] = '\0';
	read_bytes(reader, decoded.callsign, sizeof(decoded.callsign));
	decoded.callsign[sizeof(decoded.callsign) - 1] = '\0';
	decoded.difficulty = static_cast<Difficulty>(read_u8(reader));
	decoded.pilot = static_cast<Pilot>(read_u8(reader));
	decoded.mission = read_u16(reader);
	decoded.score = read_i32(reader);
	decoded.rank = read_u8(reader);
	decoded.progression = read_u8(reader);
	for (bool& medal : decoded.medals) medal = read_u8(reader) != 0;
	for (bool& bar : decoded.bars) bar = read_u8(reader) != 0;
	for (MissionGrade& result : decoded.mission_results)
	{
		result = static_cast<MissionGrade>(read_i16(reader));
	}
	for (std::uint16_t& value : decoded.mission_score_events)
	{
		value = read_u16(reader);
	}
	for (std::uint16_t& value : decoded.retry_history)
	{
		value = read_u16(reader);
	}
	decoded.retry_count = read_u16(reader);
	decoded.selected_ship = read_i16(reader);
	for (std::int16_t& item : decoded.loadout) item = read_i16(reader);
	for (std::uint8_t& rank : decoded.mission_best_ranks)
	{
		rank = read_u8(reader);
	}
	for (std::int32_t& value : decoded.branch_variables)
	{
		value = read_i32(reader);
	}
	decoded.leaderboard_seed = read_u32(reader);

	if (!reader.ready || !valid_id(decoded.id)
		|| decoded.callsign[0] == '\0'
		|| static_cast<unsigned>(decoded.difficulty) > 2
		|| static_cast<unsigned>(decoded.pilot) > 1
		|| decoded.mission < 1 || decoded.mission > 29
		|| decoded.rank > 8 || decoded.progression > 8)
	{
		return false;
	}
	state = decoded;
	return true;
}

bool encode_profile(
	const CampaignState& state,
	std::uint8_t* bytes,
	std::size_t& size)
{
	Writer writer{bytes, kFileBytes};
	write_magic(writer, "SLCP");
	write_state(writer, state);
	size = writer.offset;
	return writer.ready;
}

bool decode_profile(
	const std::uint8_t* bytes,
	std::size_t size,
	CampaignState& state)
{
	Reader reader{bytes, size};
	return read_magic(reader, "SLCP") && read_state(reader, state)
		&& reader.offset == size;
}

bool encode_save(
	const CampaignState& state,
	const char* display_name,
	std::uint8_t* bytes,
	std::size_t& size)
{
	char safe_name[kSaveNameBytes]{};
	copy_text(safe_name, sizeof(safe_name), display_name);
	Writer writer{bytes, kFileBytes};
	write_magic(writer, "SLCS");
	write_bytes(writer, safe_name, sizeof(safe_name));
	write_state(writer, state);
	size = writer.offset;
	return writer.ready;
}

bool decode_save(
	const std::uint8_t* bytes,
	std::size_t size,
	CampaignState& state,
	char* display_name,
	std::size_t display_name_capacity)
{
	Reader reader{bytes, size};
	char name[kSaveNameBytes];
	if (!read_magic(reader, "SLCS"))
	{
		return false;
	}
	read_bytes(reader, name, sizeof(name));
	name[sizeof(name) - 1] = '\0';
	if (!read_state(reader, state) || reader.offset != size)
	{
		return false;
	}
	copy_text(display_name, display_name_capacity, name);
	return true;
}

void summary_from_state(CampaignSummary& summary, const CampaignState& state)
{
	copy_text(summary.id, sizeof(summary.id), state.id);
	copy_text(summary.callsign, sizeof(summary.callsign), state.callsign);
	summary.mission = state.mission;
	summary.rank = state.rank;
}

bool write_index(const CampaignStore& store)
{
	std::uint8_t bytes[kFileBytes]{};
	Writer writer{bytes, sizeof(bytes)};
	write_magic(writer, "SLCI");
	write_u16(writer, static_cast<std::uint16_t>(store.campaign_count));
	for (std::uint32_t index = 0; index < store.campaign_count; ++index)
	{
		const CampaignSummary& summary = store.campaigns[index];
		write_bytes(writer, summary.id, kCampaignIdCharacters);
		write_bytes(writer, summary.callsign, sizeof(summary.callsign));
		write_u16(writer, summary.mission);
		write_u8(writer, summary.rank);
	}

	char path[io::kMaxPath];
	char temporary[io::kMaxPath];
	return writer.ready
		&& build_path(
			store, "index", nullptr, 0, false, path, sizeof(path))
		&& build_path(
			store, "index", nullptr, 0, true, temporary, sizeof(temporary))
		&& atomic_write(
			store.filesystem, path, temporary, bytes, writer.offset);
}

std::uint64_t mix(std::uint64_t value)
{
	value ^= value >> 30;
	value *= UINT64_C(0xbf58476d1ce4e5b9);
	value ^= value >> 27;
	value *= UINT64_C(0x94d049bb133111eb);
	return value ^ (value >> 31);
}

void make_id(char (&id)[kCampaignIdCharacters + 1], const char* callsign)
{
	static std::uint64_t sequence = 0;
	std::uint64_t left = mix(
		SDL_GetTicksNS() ^ SDL_GetPerformanceCounter() ^ ++sequence);
	std::uint64_t right = UINT64_C(1469598103934665603);
	for (const unsigned char* it =
			reinterpret_cast<const unsigned char*>(callsign);
		it != nullptr && *it != 0;
		++it)
	{
		right = (right ^ *it) * UINT64_C(1099511628211);
	}
	right = mix(right ^ left ^ sequence);
	std::snprintf(
		id,
		sizeof(id),
		"%016llx%016llx",
		static_cast<unsigned long long>(left),
		static_cast<unsigned long long>(right));
}

}

void campaign_defaults(
	CampaignState& state,
	const char* callsign,
	Difficulty difficulty,
	Pilot pilot)
{
	state = {};
	copy_text(
		state.callsign,
		sizeof(state.callsign),
		callsign == nullptr || callsign[0] == '\0' ? "Alpha 2" : callsign);
	state.difficulty = difficulty;
	state.pilot = pilot;
	state.mission = 1;
	for (MissionGrade& result : state.mission_results)
	{
		result = MissionGrade::none;
	}
	for (std::int16_t& item : state.loadout)
	{
		item = -1;
	}
	set_default_branch_variables(state);
	state.leaderboard_seed = static_cast<std::uint32_t>(SDL_GetTicks());
}

bool campaign_callsign_set(CampaignState& state, const char* callsign)
{
	if (callsign == nullptr || callsign[0] == '\0'
		|| std::strlen(callsign) >= sizeof(state.callsign))
	{
		return false;
	}
	copy_text(state.callsign, sizeof(state.callsign), callsign);
	return true;
}

bool campaign_store_init(CampaignStore& store, SDL_EMFS_Context* filesystem)
{
	store = {};
	if (filesystem == nullptr)
	{
		return false;
	}
	store.filesystem = filesystem;
	copy_text(store.directory, sizeof(store.directory), "campaigns/");
	SDL_PathInfo info;
	if (!SDL_EMFS_CreateUserDirectory(filesystem, "campaigns")
		&& (!SDL_EMFS_GetPathInfo(
				filesystem, SDL_EMFS_ROOT_USER, "campaigns", &info)
			|| info.type != SDL_PATHTYPE_DIRECTORY))
	{
		return false;
	}
	return campaign_store_reload(store);
}

bool campaign_store_reload(CampaignStore& store)
{
	store.campaign_count = 0;
	char path[io::kMaxPath];
	if (!build_path(store, "index", nullptr, 0, false, path, sizeof(path)))
	{
		return false;
	}
	std::uint8_t bytes[kFileBytes];
	std::size_t size = 0;
	if (!read_file(store.filesystem, path, bytes, size))
	{
		return true;
	}

	Reader reader{bytes, size};
	if (!read_magic(reader, "SLCI"))
	{
		return false;
	}
	const std::uint16_t count = read_u16(reader);
	if (count > kMaxCampaigns)
	{
		return false;
	}
	for (std::uint16_t index = 0; index < count; ++index)
	{
		CampaignSummary& summary = store.campaigns[index];
		read_bytes(reader, summary.id, kCampaignIdCharacters);
		summary.id[kCampaignIdCharacters] = '\0';
		read_bytes(reader, summary.callsign, sizeof(summary.callsign));
		summary.callsign[sizeof(summary.callsign) - 1] = '\0';
		summary.mission = read_u16(reader);
		summary.rank = read_u8(reader);
		if (!reader.ready || !valid_id(summary.id)
			|| summary.callsign[0] == '\0'
			|| summary.mission < 1 || summary.mission > 29
			|| summary.rank > 8)
		{
			store.campaign_count = 0;
			return false;
		}
	}
	if (reader.offset != size)
	{
		return false;
	}
	store.campaign_count = count;
	return true;
}

bool campaign_create(CampaignStore& store, CampaignState& state)
{
	if (state.callsign[0] == '\0')
	{
		return false;
	}
	make_id(state.id, state.callsign);
	if (store.campaign_count == kMaxCampaigns)
	{
		for (std::uint32_t index = 1; index < kMaxCampaigns; ++index)
		{
			store.campaigns[index - 1] = store.campaigns[index];
		}
		--store.campaign_count;
	}
	summary_from_state(store.campaigns[store.campaign_count], state);
	++store.campaign_count;
	const bool profile_saved = campaign_profile_save(store, state);
	const bool autosave_saved = profile_saved
		&& campaign_save_slot(
			store, state, kCheckpointSaveSlot, "restart");
	if (!profile_saved || !autosave_saved)
	{
		SDL_Log(
			"Campaign %s started without a complete initial save in %s",
			state.id,
			store.directory[0] != '\0'
				? store.directory
				: "(unavailable user-data directory)");
	}
	return profile_saved && autosave_saved;
}

bool campaign_profile_save(CampaignStore& store, const CampaignState& state)
{
	if (!valid_id(state.id))
	{
		return false;
	}
	std::uint8_t bytes[kFileBytes]{};
	std::size_t size = 0;
	char path[io::kMaxPath];
	char temporary[io::kMaxPath];
	if (!encode_profile(state, bytes, size)
		|| !build_path(
			store, "profile", state.id, 0, false, path, sizeof(path))
		|| !build_path(
			store, "profile", state.id, 0, true, temporary, sizeof(temporary))
		|| !atomic_write(store.filesystem, path, temporary, bytes, size))
	{
		return false;
	}

	std::uint32_t index = 0;
	while (index < store.campaign_count
		&& std::strcmp(store.campaigns[index].id, state.id) != 0)
	{
		++index;
	}
	if (index == store.campaign_count)
	{
		if (store.campaign_count >= kMaxCampaigns)
		{
			return false;
		}
		++store.campaign_count;
	}
	summary_from_state(store.campaigns[index], state);
	return write_index(store);
}

bool campaign_profile_load(
	const CampaignStore& store,
	const char* campaign_id,
	CampaignState& state)
{
	char path[io::kMaxPath];
	std::uint8_t bytes[kFileBytes];
	std::size_t size = 0;
	return build_path(
			store, "profile", campaign_id, 0, false, path, sizeof(path))
		&& read_file(store.filesystem, path, bytes, size)
		&& decode_profile(bytes, size, state)
		&& std::strcmp(state.id, campaign_id) == 0;
}

bool campaign_save_slot(
	const CampaignStore& store,
	const CampaignState& state,
	std::uint16_t slot,
	const char* display_name)
{
	if (!valid_id(state.id) || !valid_save_slot(slot)
		|| display_name == nullptr || display_name[0] == '\0')
	{
		return false;
	}
	std::uint8_t bytes[kFileBytes]{};
	std::size_t size = 0;
	char path[io::kMaxPath];
	char temporary[io::kMaxPath];
	return encode_save(state, display_name, bytes, size)
		&& build_path(
			store, "save", state.id, slot, false, path, sizeof(path))
		&& build_path(
			store, "save", state.id, slot, true, temporary, sizeof(temporary))
		&& atomic_write(store.filesystem, path, temporary, bytes, size);
}

bool campaign_load_slot(
	const CampaignStore& store,
	const char* campaign_id,
	std::uint16_t slot,
	CampaignState& state,
	char* display_name,
	std::size_t display_name_capacity)
{
	char path[io::kMaxPath];
	std::uint8_t bytes[kFileBytes];
	std::size_t size = 0;
	return build_path(
			store, "save", campaign_id, slot, false, path, sizeof(path))
		&& read_file(store.filesystem, path, bytes, size)
		&& decode_save(
			bytes, size, state, display_name, display_name_capacity)
		&& std::strcmp(state.id, campaign_id) == 0;
}

bool campaign_list_saves(
	const CampaignStore& store,
	const char* campaign_id,
	SaveList& saves)
{
	saves = {};
	for (std::uint16_t slot = 0; slot < kMaxSaveSlots; ++slot)
	{
		CampaignState state;
		char name[kSaveNameBytes];
		if (!campaign_load_slot(
			store, campaign_id, slot, state, name, sizeof(name)))
		{
			continue;
		}
		SaveSlot& item = saves.slots[saves.count++];
		item.slot = slot;
		item.mission = state.mission;
		copy_text(item.name, sizeof(item.name), name);
		copy_text(item.callsign, sizeof(item.callsign), state.callsign);
	}
	return true;
}

}

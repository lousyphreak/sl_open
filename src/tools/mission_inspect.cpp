#include "assets/gameplay_model.hpp"
#include "assets/refpack.hpp"
#include "assets/texture_cache.hpp"
#include "assets/vfx.hpp"
#include "core/blob.hpp"
#include "io/endian.hpp"
#include "io/vfs.hpp"
#include "localization/language.hpp"
#include "mission/dte.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace
{
struct Count
{
	std::uint16_t value{};
	std::uint16_t count{};
};

void add_count(Count* counts, std::size_t capacity, std::uint16_t value)
{
	for (std::size_t index = 0; index < capacity; ++index)
	{
		if (counts[index].count == 0 || counts[index].value == value)
		{
			counts[index].value = value;
			++counts[index].count;
			return;
		}
	}
}

bool parse_u16(const char* text, std::uint16_t& value)
{
	if (text == nullptr || *text == '\0')
	{
		return false;
	}
	unsigned long parsed = 0;
	for (const char* cursor = text; *cursor != '\0'; ++cursor)
	{
		if (*cursor < '0' || *cursor > '9')
		{
			return false;
		}
		parsed = parsed * 10 + static_cast<unsigned>(*cursor - '0');
		if (parsed > UINT16_MAX)
		{
			return false;
		}
	}
	value = static_cast<std::uint16_t>(parsed);
	return true;
}

bool contains_path_separator(const char* text)
{
	for (; *text != '\0'; ++text)
	{
		if (*text == '/' || *text == '\\')
		{
			return true;
		}
	}
	return false;
}

const char* type_name(const sl_open::Blob& ship_stats, std::uint16_t type)
{
	constexpr std::size_t kRecordBytes = 0x160;
	constexpr std::size_t kRecordCount = 256;
	if (type >= kRecordCount
		|| ship_stats.size != kRecordBytes * kRecordCount)
	{
		return nullptr;
	}
	return reinterpret_cast<const char*>(
		ship_stats.data + static_cast<std::size_t>(type) * kRecordBytes);
}

void print_counts(
	const char* heading,
	const Count* counts,
	std::size_t capacity,
	const sl_open::Blob* ship_stats = nullptr)
{
	std::printf("%s\n", heading);
	for (std::size_t index = 0; index < capacity; ++index)
	{
		if (counts[index].count == 0)
		{
			break;
		}
		const char* name = ship_stats == nullptr
			? nullptr
			: type_name(*ship_stats, counts[index].value);
		if (name == nullptr)
		{
			std::printf(
				"  %5u  %u\n",
				static_cast<unsigned>(counts[index].value),
				static_cast<unsigned>(counts[index].count));
		}
		else
		{
			std::printf(
				"  %5u  %-64.64s  %u\n",
				static_cast<unsigned>(counts[index].value),
				name,
				static_cast<unsigned>(counts[index].count));
		}
	}
}

void shutdown_filesystem(sl_open::io::Vfs& vfs, SDL_EMFS_Context* filesystem)
{
	sl_open::io::vfs_shutdown(vfs);
	SDL_EMFS_Destroy(filesystem);
}
}

int main(int argc, char** argv)
{
	const char* requested_root = nullptr;
	const char* sprite_path = nullptr;
	const char* model_path = nullptr;
	bool print_objects = false;
	std::uint16_t mission_number = 29;
	std::uint16_t language_ids[32]{};
	std::size_t language_id_count = 0;
	std::uint16_t shape_ids[32]{};
	std::size_t shape_id_count = 0;
	for (int index = 1; index < argc; ++index)
	{
		if (std::strcmp(argv[index], "--data") == 0 && index + 1 < argc)
		{
			requested_root = argv[++index];
		}
		else if (std::strncmp(argv[index], "--data=", 7) == 0)
		{
			requested_root = argv[index] + 7;
		}
		else if (std::strncmp(argv[index], "--mission=", 10) == 0)
		{
			if (!parse_u16(argv[index] + 10, mission_number))
			{
				std::fprintf(
					stderr, "Invalid mission number: %s\n", argv[index]);
				return EXIT_FAILURE;
			}
		}
		else if (std::strncmp(argv[index], "--language=", 11) == 0)
		{
			if (language_id_count >= 32
				|| !parse_u16(
					argv[index] + 11,
					language_ids[language_id_count]))
			{
				std::fprintf(
					stderr, "Invalid language ID: %s\n", argv[index]);
				return EXIT_FAILURE;
			}
			++language_id_count;
		}
		else if (std::strncmp(argv[index], "--sprite=", 9) == 0)
		{
			sprite_path = argv[index] + 9;
		}
		else if (std::strncmp(argv[index], "--model=", 8) == 0)
		{
			model_path = argv[index] + 8;
		}
		else if (std::strcmp(argv[index], "--objects") == 0)
		{
			print_objects = true;
		}
		else if (std::strncmp(argv[index], "--shape=", 8) == 0)
		{
			if (shape_id_count >= 32
				|| !parse_u16(argv[index] + 8, shape_ids[shape_id_count]))
			{
				std::fprintf(
					stderr, "Invalid shape ID: %s\n", argv[index]);
				return EXIT_FAILURE;
			}
			++shape_id_count;
		}
		else
		{
			std::fprintf(
				stderr,
				"Usage: sl_open_inspect --data PATH [--mission=N] "
				"[--language=N ...] "
				"[--sprite=PATH --shape=N ...] [--model=PATH] [--objects]\n");
			return EXIT_FAILURE;
		}
	}

	SDL_EMFS_Config filesystem_config;
	SDL_EMFS_InitConfig(&filesystem_config);
	filesystem_config.asset_root = requested_root != nullptr
		? requested_root
		: ".";
	filesystem_config.organization = "sl_open";
	filesystem_config.application = "sl_open_inspect";
	SDL_EMFS_Context* filesystem = SDL_EMFS_Create(&filesystem_config);
	if (filesystem == nullptr || !SDL_EMFS_WaitReady(filesystem)
		|| !sl_open::io::game_root_looks_valid(filesystem))
	{
		std::fprintf(stderr, "Original StarLancer data was not found\n");
		if (filesystem != nullptr)
		{
			SDL_EMFS_Destroy(filesystem);
		}
		return EXIT_FAILURE;
	}

	sl_open::io::Vfs vfs;
	if (!sl_open::io::vfs_init(vfs, filesystem, false)
		|| !sl_open::io::vfs_mount_archive(vfs, "resource.hog"))
	{
		std::fprintf(stderr, "resource.hog could not be mounted\n");
		shutdown_filesystem(vfs, filesystem);
		return EXIT_FAILURE;
	}

	if (language_id_count != 0)
	{
		sl_open::LanguageTable language;
		if (!sl_open::load_language_dll(vfs, "LANGUAGE.DLL", language))
		{
			std::fprintf(stderr, "LANGUAGE.DLL could not be loaded\n");
			shutdown_filesystem(vfs, filesystem);
			return EXIT_FAILURE;
		}
		std::printf("Language strings\n");
		for (std::size_t index = 0; index < language_id_count; ++index)
		{
			const std::uint16_t id = language_ids[index];
			std::printf(
				"  0x%03x  %s\n",
				static_cast<unsigned>(id),
				sl_open::language_text(language, id));
		}
	}

	if (sprite_path != nullptr)
	{
		sl_open::assets::SpriteList sprites;
		if (!sl_open::assets::load_sprite_list(vfs, sprite_path, sprites))
		{
			std::fprintf(
				stderr, "%s could not be loaded as a sprite list\n",
				sprite_path);
			shutdown_filesystem(vfs, filesystem);
			return EXIT_FAILURE;
		}
		std::printf(
			"%s: %u records\n",
			sprite_path,
			static_cast<unsigned>(sprites.shape_count));
		for (std::size_t index = 0; index < shape_id_count; ++index)
		{
			sl_open::assets::IndexedImage shape;
			const std::uint16_t id = shape_ids[index];
			if (!sl_open::assets::decode_sprite_shape(sprites, id, shape))
			{
				std::printf("  shape %u: not an image\n", id);
				continue;
			}
			std::printf(
				"  shape %u: %ux%u offset=(%d,%d)\n",
				static_cast<unsigned>(id),
				static_cast<unsigned>(shape.width),
				static_cast<unsigned>(shape.height),
				shape.min_x,
				shape.min_y);
		}
	}

	if (model_path != nullptr)
	{
		sl_open::assets::TextureCache cache;
		sl_open::assets::GameplayModel model;
		if (!sl_open::assets::texture_cache_load(
				vfs, "tcachehw.dat", "palette.ccb", cache))
		{
			std::fprintf(stderr, "hardware texture cache could not be loaded\n");
			shutdown_filesystem(vfs, filesystem);
			return EXIT_FAILURE;
		}
		std::uint32_t indexed_textures = 0;
		std::uint32_t indexed_alpha_textures = 0;
		std::uint32_t rgb565_textures = 0;
		for (std::uint32_t texture_index = 0;
			texture_index < cache.record_count;
			++texture_index)
		{
			const sl_open::assets::TextureCacheRecord& record =
				cache.records[texture_index];
			sl_open::assets::TextureImage decoded;
			if (!sl_open::assets::texture_cache_decode(
					cache, record.name, decoded))
			{
				std::fprintf(
					stderr,
					"hardware texture %s could not be decoded\n",
					record.name);
				shutdown_filesystem(vfs, filesystem);
				return EXIT_FAILURE;
			}
			if (record.channel_masks[0] != 0)
			{
				if (record.channel_masks[4] != 0)
				{
					++indexed_alpha_textures;
				}
				else
				{
					++indexed_textures;
				}
			}
			else
			{
				++rgb565_textures;
			}
		}
		std::printf(
			"texture cache: records=%u indexed=%u indexed-alpha=%u "
			"rgb565=%u\n",
			cache.record_count,
			indexed_textures,
			indexed_alpha_textures,
			rgb565_textures);
		if (!sl_open::assets::load_gameplay_model(
				vfs, model_path, cache, model))
		{
			std::fprintf(
				stderr, "%s could not be loaded as a gameplay model\n",
				model_path);
			shutdown_filesystem(vfs, filesystem);
			return EXIT_FAILURE;
		}
		std::uint64_t vertices = 0;
		std::uint64_t indices = 0;
		std::uint32_t lods = 0;
		std::uint64_t material_mode_sections[16]{};
		float minimum_threshold = std::numeric_limits<float>::max();
		float maximum_threshold = 0.0f;
		for (const sl_open::assets::GameplayNode& node : model.nodes)
		{
			lods += static_cast<std::uint32_t>(node.lods.size());
			for (const sl_open::assets::GameplayLod& lod : node.lods)
			{
				vertices += lod.vertex_count;
				indices += lod.index_count;
				minimum_threshold =
					std::min(minimum_threshold, lod.threshold);
				maximum_threshold =
					std::max(maximum_threshold, lod.threshold);
				for (const sl_open::assets::GameplaySection& section
					: lod.sections)
				{
					++material_mode_sections[section.mode & 0x0fu];
				}
			}
		}
		std::printf(
			"%s: nodes=%zu lods=%u materials=%zu "
			"vertices=%llu indices=%llu radius=%.3f "
			"root-flags=0x%08x lod-thresholds=%.3f..%.3f "
			"center=(%.3f,%.3f,%.3f) camera=(%.3f,%.3f,%.3f)\n",
			model_path,
			model.nodes.size(),
			lods,
			model.materials.size(),
			static_cast<unsigned long long>(vertices),
			static_cast<unsigned long long>(indices),
			model.radius,
			model.root_flags,
			minimum_threshold,
			maximum_threshold,
			model.center_of_mass.x,
			model.center_of_mass.y,
			model.center_of_mass.z,
			model.camera_offset.x,
			model.camera_offset.y,
			model.camera_offset.z);
		std::printf("  material modes:");
		for (std::uint32_t mode = 0;
			mode < std::size(material_mode_sections);
			++mode)
		{
			if (material_mode_sections[mode] != 0)
			{
				std::printf(
					" %u=%llu",
					mode,
					static_cast<unsigned long long>(
						material_mode_sections[mode]));
			}
		}
		std::printf("\n");
		for (std::uint32_t node_index = 0;
			node_index < model.nodes.size();
			++node_index)
		{
			const sl_open::assets::GameplayNode& node =
				model.nodes[node_index];
			std::printf(
				"  node %2u parent=%2d type=%u hp=%d "
				"flags=0x%08x group=%u damage-selector=%u "
				"gun-kind=%u gun-slot=%u sequences=%zu "
				"point-groups=%zu position=(%.3f,%.3f,%.3f) "
				"basis-x=(%.6f,%.6f,%.6f) "
				"basis-y=(%.6f,%.6f,%.6f) "
				"basis-z=(%.6f,%.6f,%.6f) name=%s\n",
				static_cast<unsigned>(node_index),
				node.parent,
				node.model_type,
				node.maximum_hit_points,
				node.flags,
				node.part_group_id,
				node.damage_group_selector,
				static_cast<unsigned>(node.gun_mount_kind),
				node.gun_part_slot,
				node.sequences.size(),
				node.point_groups.size(),
				node.position.x,
				node.position.y,
				node.position.z,
				node.basis[0].x,
				node.basis[0].y,
				node.basis[0].z,
				node.basis[1].x,
				node.basis[1].y,
				node.basis[1].z,
				node.basis[2].x,
				node.basis[2].y,
				node.basis[2].z,
				node.name);
		}
		for (std::uint32_t locator_index = 0;
			locator_index < model.locators.size();
			++locator_index)
		{
			const sl_open::assets::GameplayLocator& locator =
				model.locators[locator_index];
			std::printf(
				"  locator %2u node=%u type=%d subtype=%d "
				"position=(%.3f,%.3f,%.3f) "
				"basis-x=(%.6f,%.6f,%.6f) "
				"basis-y=(%.6f,%.6f,%.6f) "
				"basis-z=(%.6f,%.6f,%.6f)\n",
				static_cast<unsigned>(locator_index),
				static_cast<unsigned>(locator.source_node),
				static_cast<int>(locator.type),
				static_cast<int>(locator.subtype),
				locator.position.x,
				locator.position.y,
				locator.position.z,
				locator.basis[0].x,
				locator.basis[0].y,
				locator.basis[0].z,
				locator.basis[1].x,
				locator.basis[1].y,
				locator.basis[1].z,
				locator.basis[2].x,
				locator.basis[2].y,
				locator.basis[2].z);
		}
	}

	char path[64];
	std::snprintf(
		path,
		sizeof(path),
		"missions/mission%u.dte",
		static_cast<unsigned>(mission_number));
	sl_open::mission::DteFile mission;
	if (!sl_open::mission::dte_load(vfs, path, mission))
	{
		std::fprintf(
			stderr,
			"%s: %s\n",
			path,
			sl_open::mission::dte_load_error_text(mission.error));
		shutdown_filesystem(vfs, filesystem);
		return EXIT_FAILURE;
	}

	std::printf(
		"%s: %zu bytes, %zu sections\n",
		path,
		mission.image.size,
		sl_open::mission::kDteSectionCount);
	for (std::size_t index = 0;
		index < sl_open::mission::kDteSectionCount;
		++index)
	{
		const sl_open::mission::DteSection& section = mission.sections[index];
		std::printf(
			"  section %2zu  count=%5u  offset=0x%06x  arena=0x%05x\n",
			index,
			static_cast<unsigned>(section.count),
			section.offset,
			section.bytes);
	}

	sl_open::Blob ship_stats_stored;
	sl_open::Blob ship_stats;
	if (sl_open::io::vfs_read_all(vfs, "shipstats.bin", ship_stats_stored))
	{
		sl_open::assets::unwrap_refpack(
			static_cast<sl_open::Blob&&>(ship_stats_stored), ship_stats);
	}

	const sl_open::mission::DteSection& object_section = mission.sections[3];
	const std::uint8_t* objects =
		sl_open::mission::dte_section_data(mission, 3);
	Count object_types[512]{};
	Count pilots[512]{};
	Count groups[512]{};
	for (std::uint16_t index = 0; index < object_section.count; ++index)
	{
		const std::uint8_t* object =
			objects + static_cast<std::size_t>(index) * 0x4c;
		add_count(
			object_types,
			512,
			sl_open::io::read_le16(object + 0x18));
		add_count(pilots, 512, object[0x15]);
		add_count(groups, 512, object[0x14]);
	}
	print_counts("Object types", object_types, 512, &ship_stats);
	print_counts("Pilot ordinals", pilots, 512);
	print_counts("Object group ordinals", groups, 512);
	if (print_objects)
	{
		std::printf("Mission objects\n");
		for (std::uint16_t index = 0;
			index < object_section.count;
			++index)
		{
			const std::uint8_t* record =
				objects + static_cast<std::size_t>(index) * 0x4c;
			float position[3];
			std::memcpy(position, record + 0x1c, sizeof(position));
			std::printf(
				"  %3u type=%5u group=%3u pilot=%3u "
				"launch=(type=%u point=%u) "
				"position=(%g,%g,%g) angles=(%d,%d,%d)\n",
				static_cast<unsigned>(index),
				static_cast<unsigned>(sl_open::io::read_le16(record + 0x18)),
				static_cast<unsigned>(record[0x14]),
				static_cast<unsigned>(record[0x15]),
				static_cast<unsigned>(sl_open::io::read_le16(record + 0x28)),
				static_cast<unsigned>(record[0x2b]),
				static_cast<double>(position[0]),
				static_cast<double>(position[1]),
				static_cast<double>(position[2]),
				static_cast<std::int16_t>(
					sl_open::io::read_le16(record + 0x2e)),
				static_cast<std::int16_t>(
					sl_open::io::read_le16(record + 0x3a)),
				static_cast<std::int16_t>(
					sl_open::io::read_le16(record + 0x4a)));
		}
	}

	const sl_open::mission::DteSection& trigger_section = mission.sections[5];
	const std::uint8_t* triggers =
		sl_open::mission::dte_section_data(mission, 5);
	Count trigger_types[512]{};
	for (std::uint16_t index = 0; index < trigger_section.count; ++index)
	{
		add_count(
			trigger_types,
			512,
			triggers[static_cast<std::size_t>(index) * 0x30]);
	}
	print_counts("Section-5 record types", trigger_types, 512);

	const sl_open::mission::DteSection& function_section = mission.sections[8];
	const std::uint8_t* functions =
		sl_open::mission::dte_section_data(mission, 8);
	std::printf("Section-8 script links\n");
	for (std::uint16_t index = 0; index < function_section.count; ++index)
	{
		const std::uint8_t* record =
			functions + static_cast<std::size_t>(index) * 0x1c;
		std::printf(
			"  %3u  word=%5u  mode=%3u  pool=%3u\n",
			static_cast<unsigned>(index),
			static_cast<unsigned>(sl_open::io::read_le16(record + 0x0a)),
			static_cast<unsigned>(record[0x0d]),
			static_cast<unsigned>(record[0x19]));
	}

	const sl_open::mission::DteSection& string_section = mission.sections[0];
	const char* strings = reinterpret_cast<const char*>(
		sl_open::mission::dte_section_data(mission, 0));
	std::printf("Path-bearing mission strings\n");
	std::size_t offset = 0;
	while (offset < string_section.count)
	{
		const char* text = strings + offset;
		const std::size_t remaining = string_section.count - offset;
		const void* terminator = std::memchr(text, '\0', remaining);
		if (terminator == nullptr)
		{
			std::printf("  [unterminated string at %zu]\n", offset);
			break;
		}
		const std::size_t length =
			static_cast<const char*>(terminator) - text;
		if (length != 0 && contains_path_separator(text))
		{
			std::printf("  %5zu  %s\n", offset, text);
		}
		offset += length + 1;
	}

	shutdown_filesystem(vfs, filesystem);
	return EXIT_SUCCESS;
}

#include "localization/language.hpp"

#include "io/endian.hpp"
#include "io/vfs.hpp"

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace sl_open
{
namespace
{
struct PeSection
{
	std::uint32_t virtual_address{};
	std::uint32_t virtual_size{};
	std::uint32_t raw_offset{};
	std::uint32_t raw_size{};
};

struct PeView
{
	const std::uint8_t* data{};
	std::size_t size{};
	PeSection sections[96]{};
	std::uint32_t section_count{};
	std::uint32_t resource_rva{};
	std::uint32_t resource_size{};
	std::size_t resource_offset{};
};

struct Utf16String
{
	const std::uint8_t* bytes{};
	std::uint16_t length{};
};

bool rva_to_offset(const PeView& pe, std::uint32_t rva, std::size_t& offset)
{
	for (std::uint32_t index = 0; index < pe.section_count; ++index)
	{
		const PeSection& section = pe.sections[index];
		const std::uint32_t mapped_size =
			section.virtual_size > section.raw_size
			? section.virtual_size
			: section.raw_size;
		if (rva >= section.virtual_address
			&& rva - section.virtual_address < mapped_size)
		{
			offset = static_cast<std::size_t>(section.raw_offset)
				+ (rva - section.virtual_address);
			return offset < pe.size;
		}
	}
	return false;
}

bool parse_pe(const sl_open::Blob& file, PeView& pe)
{
	if (file.size < 0x40 || std::memcmp(file.data, "MZ", 2) != 0)
	{
		return false;
	}
	const std::uint32_t pe_offset = io::read_le32(file.data + 0x3c);
	if (!io::range_fits(file.size, pe_offset, 24)
		|| std::memcmp(file.data + pe_offset, "PE\0\0", 4) != 0)
	{
		return false;
	}

	const std::uint16_t section_count = io::read_le16(file.data + pe_offset + 6);
	const std::uint16_t optional_size = io::read_le16(file.data + pe_offset + 20);
	const std::size_t optional = pe_offset + 24;
	if (section_count == 0 || section_count > 96
		|| !io::range_fits(file.size, optional, optional_size))
	{
		return false;
	}

	const std::uint16_t magic = io::read_le16(file.data + optional);
	const std::size_t directory_start = magic == 0x10b
		? optional + 96
		: magic == 0x20b ? optional + 112 : 0;
	if (directory_start == 0
		|| !io::range_fits(file.size, directory_start, 8 * 3))
	{
		return false;
	}

	pe = {};
	pe.data = file.data;
	pe.size = file.size;
	pe.section_count = section_count;
	pe.resource_rva = io::read_le32(file.data + directory_start + 16);
	pe.resource_size = io::read_le32(file.data + directory_start + 20);

	const std::size_t section_table = optional + optional_size;
	if (!io::range_fits(file.size, section_table, static_cast<std::size_t>(section_count) * 40))
	{
		return false;
	}
	for (std::uint32_t index = 0; index < section_count; ++index)
	{
		const std::uint8_t* entry = file.data + section_table + index * 40;
		pe.sections[index] = {
			.virtual_address = io::read_le32(entry + 12),
			.virtual_size = io::read_le32(entry + 8),
			.raw_offset = io::read_le32(entry + 20),
			.raw_size = io::read_le32(entry + 16),
		};
	}

	return pe.resource_rva != 0
		&& pe.resource_size >= 16
		&& rva_to_offset(pe, pe.resource_rva, pe.resource_offset)
		&& io::range_fits(pe.size, pe.resource_offset, pe.resource_size);
}

bool resource_range(
	const PeView& pe,
	std::uint32_t relative,
	std::size_t length,
	const std::uint8_t*& bytes)
{
	if (!io::range_fits(pe.resource_size, relative, length))
	{
		return false;
	}
	bytes = pe.data + pe.resource_offset + relative;
	return true;
}

bool find_numeric_resource(
	const PeView& pe,
	std::uint32_t directory,
	std::uint32_t id,
	std::uint32_t& child,
	bool& child_is_directory)
{
	const std::uint8_t* header = nullptr;
	if (!resource_range(pe, directory, 16, header))
	{
		return false;
	}
	const std::uint32_t named = io::read_le16(header + 12);
	const std::uint32_t ids = io::read_le16(header + 14);
	const std::uint32_t count = named + ids;
	const std::uint8_t* entries = nullptr;
	if (count > 4096
		|| !resource_range(pe, directory + 16, static_cast<std::size_t>(count) * 8, entries))
	{
		return false;
	}

	for (std::uint32_t index = named; index < count; ++index)
	{
		const std::uint32_t name = io::read_le32(entries + index * 8);
		const std::uint32_t target = io::read_le32(entries + index * 8 + 4);
		if ((name & 0x80000000u) == 0 && (name & 0xffffu) == id)
		{
			child = target & 0x7fffffffu;
			child_is_directory = (target & 0x80000000u) != 0;
			return true;
		}
	}
	return false;
}

bool first_resource_child(
	const PeView& pe,
	std::uint32_t directory,
	std::uint32_t& child,
	bool& child_is_directory)
{
	const std::uint8_t* header = nullptr;
	if (!resource_range(pe, directory, 16, header))
	{
		return false;
	}
	const std::uint32_t count =
		io::read_le16(header + 12) + io::read_le16(header + 14);
	const std::uint8_t* entry = nullptr;
	if (count == 0 || !resource_range(pe, directory + 16, 8, entry))
	{
		return false;
	}
	const std::uint32_t target = io::read_le32(entry + 4);
	child = target & 0x7fffffffu;
	child_is_directory = (target & 0x80000000u) != 0;
	return true;
}

bool string_block(
	const PeView& pe,
	std::uint32_t type_directory,
	std::uint32_t block_id,
	const std::uint8_t*& data,
	std::size_t& size)
{
	std::uint32_t name_directory = 0;
	bool is_directory = false;
	if (!find_numeric_resource(
		pe, type_directory, block_id, name_directory, is_directory)
		|| !is_directory)
	{
		return false;
	}

	std::uint32_t data_entry = 0;
	if (!first_resource_child(pe, name_directory, data_entry, is_directory)
		|| is_directory)
	{
		return false;
	}

	const std::uint8_t* entry = nullptr;
	if (!resource_range(pe, data_entry, 16, entry))
	{
		return false;
	}
	const std::uint32_t data_rva = io::read_le32(entry);
	const std::uint32_t data_size = io::read_le32(entry + 4);
	std::size_t file_offset = 0;
	if (!rva_to_offset(pe, data_rva, file_offset)
		|| !io::range_fits(pe.size, file_offset, data_size))
	{
		return false;
	}
	data = pe.data + file_offset;
	size = data_size;
	return true;
}

bool decode_string_block(
	const std::uint8_t* data,
	std::size_t size,
	std::uint32_t block_id,
	Utf16String* strings,
	std::uint32_t& highest_id)
{
	std::size_t cursor = 0;
	for (std::uint32_t slot = 0; slot < 16; ++slot)
	{
		if (!io::range_fits(size, cursor, 2))
		{
			return false;
		}
		const std::uint16_t length = io::read_le16(data + cursor);
		cursor += 2;
		const std::size_t bytes = static_cast<std::size_t>(length) * 2;
		if (!io::range_fits(size, cursor, bytes))
		{
			return false;
		}

		const std::uint32_t id = (block_id - 1) * 16 + slot;
		if (id < kMaxLanguageStrings && length != 0)
		{
			strings[id] = {.bytes = data + cursor, .length = length};
			if (id > highest_id)
			{
				highest_id = id;
			}
		}
		cursor += bytes;
	}
	return true;
}

bool read_codepoint(const Utf16String& source, std::size_t& index, std::uint32_t& codepoint)
{
	const std::uint16_t first = io::read_le16(source.bytes + index * 2);
	++index;
	if (first < 0xd800 || first > 0xdfff)
	{
		codepoint = first;
		return true;
	}
	if (first > 0xdbff || index >= source.length)
	{
		return false;
	}
	const std::uint16_t second = io::read_le16(source.bytes + index * 2);
	if (second < 0xdc00 || second > 0xdfff)
	{
		return false;
	}
	++index;
	codepoint = 0x10000
		+ ((static_cast<std::uint32_t>(first) - 0xd800) << 10)
		+ (static_cast<std::uint32_t>(second) - 0xdc00);
	return true;
}

std::size_t utf8_length(const Utf16String& source)
{
	std::size_t result = 0;
	for (std::size_t index = 0; index < source.length;)
	{
		std::uint32_t codepoint = 0;
		if (!read_codepoint(source, index, codepoint))
		{
			return SIZE_MAX;
		}
		result += codepoint < 0x80 ? 1 : codepoint < 0x800 ? 2
			: codepoint < 0x10000 ? 3 : 4;
	}
	return result;
}

bool utf8_write(const Utf16String& source, char* output)
{
	for (std::size_t index = 0; index < source.length;)
	{
		std::uint32_t codepoint = 0;
		if (!read_codepoint(source, index, codepoint))
		{
			return false;
		}
		if (codepoint < 0x80)
		{
			*output++ = static_cast<char>(codepoint);
		}
		else if (codepoint < 0x800)
		{
			*output++ = static_cast<char>(0xc0 | (codepoint >> 6));
			*output++ = static_cast<char>(0x80 | (codepoint & 0x3f));
		}
		else if (codepoint < 0x10000)
		{
			*output++ = static_cast<char>(0xe0 | (codepoint >> 12));
			*output++ = static_cast<char>(0x80 | ((codepoint >> 6) & 0x3f));
			*output++ = static_cast<char>(0x80 | (codepoint & 0x3f));
		}
		else
		{
			*output++ = static_cast<char>(0xf0 | (codepoint >> 18));
			*output++ = static_cast<char>(0x80 | ((codepoint >> 12) & 0x3f));
			*output++ = static_cast<char>(0x80 | ((codepoint >> 6) & 0x3f));
			*output++ = static_cast<char>(0x80 | (codepoint & 0x3f));
		}
	}
	*output = '\0';
	return true;
}
}

bool parse_language_dll(Blob file, LanguageTable& language)
{
	language = {};
	PeView pe;
	if (!parse_pe(file, pe))
	{
		return false;
	}

	std::uint32_t string_type = 0;
	bool is_directory = false;
	if (!find_numeric_resource(pe, 0, 6, string_type, is_directory) || !is_directory)
	{
		return false;
	}

	Utf16String strings[kMaxLanguageStrings]{};
	std::uint32_t highest_id = 0;
	for (std::uint32_t block = 1; block <= kMaxLanguageStrings / 16; ++block)
	{
		const std::uint8_t* data = nullptr;
		std::size_t size = 0;
		if (string_block(pe, string_type, block, data, size)
			&& !decode_string_block(data, size, block, strings, highest_id))
		{
			return false;
		}
	}

	std::size_t total_bytes = 1;
	for (std::uint32_t id = 0; id <= highest_id; ++id)
	{
		if (strings[id].bytes == nullptr)
		{
			continue;
		}
		const std::size_t length = utf8_length(strings[id]);
		if (length == SIZE_MAX || length > UINT16_MAX
			|| total_bytes > SIZE_MAX - length - 1)
		{
			return false;
		}
		total_bytes += length + 1;
	}
	if (!language.text.allocate(total_bytes))
	{
		return false;
	}
	language.text.data[0] = '\0';

	std::size_t cursor = 1;
	for (std::uint32_t id = 0; id <= highest_id; ++id)
	{
		if (strings[id].bytes == nullptr)
		{
			continue;
		}
		const std::size_t length = utf8_length(strings[id]);
		language.offsets[id] = static_cast<std::uint32_t>(cursor);
		language.lengths[id] = static_cast<std::uint16_t>(length);
		if (!utf8_write(strings[id], reinterpret_cast<char*>(language.text.data + cursor)))
		{
			language = {};
			return false;
		}
		cursor += length + 1;
	}
	language.highest_id = highest_id;
	return highest_id != 0;
}

bool load_language_dll(
	io::Vfs& vfs,
	const char* path,
	LanguageTable& language)
{
	Blob file;
	return io::vfs_read_all(vfs, path, file)
		&& parse_language_dll(static_cast<Blob&&>(file), language);
}

const char* language_text(const LanguageTable& language, std::uint32_t id)
{
	if (id >= kMaxLanguageStrings || language.offsets[id] == 0)
	{
		return "";
	}
	return reinterpret_cast<const char*>(language.text.data + language.offsets[id]);
}
}

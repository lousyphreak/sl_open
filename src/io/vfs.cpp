#include "io/vfs.hpp"

#include "io/endian.hpp"

#include <SDL3/SDL.h>

#include <cctype>
#include <cstring>

namespace sl_open::io
{
namespace
{
bool copy_string(char* output, std::size_t capacity, const char* input)
{
	if (output == nullptr || capacity == 0 || input == nullptr)
	{
		return false;
	}
	const std::size_t length = std::strlen(input);
	if (length >= capacity)
	{
		return false;
	}
	std::memcpy(output, input, length + 1);
	return true;
}

char ascii_lower(char value)
{
	if (value >= 'A' && value <= 'Z')
	{
		return static_cast<char>(value + ('a' - 'A'));
	}
	return value;
}

bool ascii_equal_case_insensitive(const char* left, const char* right)
{
	for (;; ++left, ++right)
	{
		if (ascii_lower(*left) != ascii_lower(*right))
		{
			return false;
		}
		if (*left == '\0')
		{
			return true;
		}
	}
}

bool read_now(
	SDL_EMFS_File* file,
	std::uint64_t offset,
	void* destination,
	std::size_t byte_count)
{
	SDL_IOStatus status = SDL_IO_STATUS_READY;
	return SDL_EMFS_ReadAt(
			file, offset, destination, byte_count, &status) == byte_count
		&& status != SDL_IO_STATUS_ERROR;
}

bool open_loose(Vfs& vfs, const char* normalized, VfsFile& file)
{
	SDL_EMFS_File* stream = SDL_EMFS_Open(
		vfs.filesystem, SDL_EMFS_ROOT_ASSET, normalized, "r");
	if (stream == nullptr)
	{
		return false;
	}
	const Sint64 size = SDL_EMFS_GetSize(stream);
	if (size < 0)
	{
		SDL_EMFS_Close(stream);
		return false;
	}

	file.loose_file = stream;
	file.size = static_cast<std::uint64_t>(size);
	file.source = FileSource::loose;
	return true;
}

const char* basename(const char* path)
{
	const char* result = path;
	for (const char* it = path; *it != '\0'; ++it)
	{
		if (*it == '/')
		{
			result = it + 1;
		}
	}
	return result;
}

bool open_archive_member(Vfs& vfs, const char* normalized, VfsFile& file)
{
	const char* wanted = basename(normalized);
	for (std::uint32_t archive_index = 0;
		archive_index < vfs.archive_count;
		++archive_index)
	{
		Archive& archive = vfs.archives[archive_index];
		for (std::uint32_t entry_index = 0;
			entry_index < archive.entry_count;
			++entry_index)
		{
			const ArchiveEntry& entry = archive.entries[entry_index];
			if (!ascii_equal_case_insensitive(
					wanted, archive.names + entry.name_offset))
			{
				continue;
			}

			file.archive = &archive;
			file.base_offset = entry.offset;
			file.size = entry.size;
			file.source = FileSource::archive;
			return true;
		}
	}
	return false;
}
}

bool normalize_asset_path(const char* input, char* output, std::size_t capacity)
{
	if (input == nullptr || output == nullptr || capacity == 0)
	{
		return false;
	}

	while (*input == '/' || *input == '\\')
	{
		++input;
	}
	if (*input == '\0')
	{
		return false;
	}

	std::size_t out = 0;
	std::size_t component_start = 0;
	for (;; ++input)
	{
		const char value = *input == '\\' ? '/' : *input;
		if (value == '/' || value == '\0')
		{
			const std::size_t component_length = out - component_start;
			if (component_length == 0)
			{
				if (value == '\0')
				{
					break;
				}
				continue;
			}
			if ((component_length == 1 && output[component_start] == '.')
				|| (component_length == 2
					&& output[component_start] == '.'
					&& output[component_start + 1] == '.'))
			{
				return false;
			}
			if (value == '\0')
			{
				break;
			}
			if (out + 1 >= capacity)
			{
				return false;
			}
			output[out++] = '/';
			component_start = out;
			continue;
		}
		if (value == ':' || out + 1 >= capacity)
		{
			return false;
		}
		output[out++] = value;
	}
	output[out] = '\0';
	return out != 0;
}

bool game_root_looks_valid(SDL_EMFS_Context* filesystem)
{
	const char* required[] = {"resource.hog", "CD1.HOG", "LANGUAGE.DLL"};
	for (const char* name : required)
	{
		SDL_PathInfo info;
		if (!SDL_EMFS_GetPathInfo(
				filesystem, SDL_EMFS_ROOT_ASSET, name, &info)
			|| info.type != SDL_PATHTYPE_FILE)
		{
			return false;
		}
	}
	return true;
}

bool vfs_init(Vfs& vfs, SDL_EMFS_Context* filesystem, bool loose_first)
{
	vfs = {};
	vfs.filesystem = filesystem;
	vfs.loose_first = loose_first;
	return filesystem != nullptr;
}

void vfs_shutdown(Vfs& vfs)
{
	for (std::uint32_t index = 0; index < vfs.archive_count; ++index)
	{
		SDL_EMFS_Close(vfs.archives[index].file);
	}
	vfs = {};
}

bool vfs_mount_archive(Vfs& vfs, const char* filename)
{
	if (vfs.archive_count >= kMaxArchives)
	{
		SDL_Log("VFS archive limit reached (%zu)", kMaxArchives);
		return false;
	}

	Archive& archive = vfs.archives[vfs.archive_count];
	archive.file = SDL_EMFS_Open(
		vfs.filesystem, SDL_EMFS_ROOT_ASSET, filename, "r");
	if (archive.file == nullptr)
	{
		return false;
	}
	const Sint64 archive_size = SDL_EMFS_GetSize(archive.file);
	if (archive_size < 0)
	{
		SDL_EMFS_Close(archive.file);
		archive = {};
		return false;
	}
	archive.size = static_cast<std::uint64_t>(archive_size);

	std::uint8_t header[16];
	if (!read_now(archive.file, 0, header, sizeof(header))
		|| (std::memcmp(header, "BIGF", 4) != 0
			&& std::memcmp(header, "BIG4", 4) != 0))
	{
		SDL_EMFS_Close(archive.file);
		archive = {};
		return false;
	}

	const std::uint32_t declared_size = read_be32(header + 4);
	const std::uint32_t declared_count = read_be32(header + 8);
	const std::uint32_t data_offset = read_be32(header + 12);
	if (declared_size != archive.size
		|| data_offset < sizeof(header)
		|| data_offset > archive.size
		|| data_offset > kArchiveNameBytes
		|| declared_count > kMaxArchiveEntries)
	{
		SDL_EMFS_Close(archive.file);
		archive = {};
		return false;
	}

	sl_open::Blob directory;
	if (!directory.allocate(data_offset)
		|| !read_now(archive.file, 0, directory.data, directory.size))
	{
		SDL_EMFS_Close(archive.file);
		archive = {};
		return false;
	}

	std::size_t cursor = 16;
	std::uint64_t previous_end = data_offset;
	while (archive.entry_count < declared_count
		&& cursor + 9 <= directory.size)
	{
		const std::uint32_t offset = read_be32(directory.data + cursor);
		const std::uint32_t size = read_be32(directory.data + cursor + 4);
		cursor += 8;

		const std::size_t name_start = cursor;
		while (cursor < directory.size && directory.data[cursor] != 0)
		{
			++cursor;
		}
		if (cursor == directory.size)
		{
			break;
		}

		const std::size_t name_length = cursor - name_start;
		++cursor;
		if (name_length == 0
			|| offset < data_offset
			|| offset < previous_end
			|| size > archive.size - offset
			|| name_length + 1 > kArchiveNameBytes - archive.name_bytes)
		{
			break;
		}

		ArchiveEntry& entry = archive.entries[archive.entry_count++];
		entry.offset = offset;
		entry.size = size;
		entry.name_offset = archive.name_bytes;
		std::memcpy(
			archive.names + archive.name_bytes,
			directory.data + name_start,
			name_length);
		archive.names[archive.name_bytes + name_length] = '\0';
		archive.name_bytes += static_cast<std::uint32_t>(name_length + 1);
		previous_end = static_cast<std::uint64_t>(offset) + size;
	}

	if (archive.entry_count == 0
		|| !copy_string(archive.path, sizeof(archive.path), filename))
	{
		SDL_EMFS_Close(archive.file);
		archive = {};
		return false;
	}

	++vfs.archive_count;
	return true;
}

bool vfs_open(Vfs& vfs, const char* logical_path, VfsFile& file)
{
	file = {};
	char normalized[kMaxPath];
	if (!normalize_asset_path(logical_path, normalized, sizeof(normalized)))
	{
		return false;
	}

	if (vfs.loose_first)
	{
		return open_loose(vfs, normalized, file)
			|| open_archive_member(vfs, normalized, file);
	}
	return open_archive_member(vfs, normalized, file)
		|| open_loose(vfs, normalized, file);
}

bool vfs_open_from_archive(
	Vfs& vfs,
	const char* archive_filename,
	const char* logical_path,
	VfsFile& file)
{
	file = {};
	char normalized[kMaxPath];
	if (!normalize_asset_path(logical_path, normalized, sizeof(normalized)))
	{
		return false;
	}
	const char* wanted = basename(normalized);
	for (std::uint32_t archive_index = 0;
		archive_index < vfs.archive_count;
		++archive_index)
	{
		Archive& archive = vfs.archives[archive_index];
		if (!ascii_equal_case_insensitive(
				archive_filename, basename(archive.path)))
		{
			continue;
		}
		for (std::uint32_t entry_index = 0;
			entry_index < archive.entry_count;
			++entry_index)
		{
			const ArchiveEntry& entry = archive.entries[entry_index];
			if (!ascii_equal_case_insensitive(
					wanted, archive.names + entry.name_offset))
			{
				continue;
			}
			file.archive = &archive;
			file.base_offset = entry.offset;
			file.size = entry.size;
			file.source = FileSource::archive;
			return true;
		}
		return false;
	}
	return false;
}

IoStatus vfs_read_at(
	Vfs&,
	VfsFile& file,
	std::uint64_t offset,
	void* destination,
	std::size_t byte_count,
	ReadRequest& request)
{
	request = {};
	if (file.source == FileSource::none
		|| offset > file.size
		|| byte_count > file.size - offset)
	{
		return IoStatus::failed;
	}

	SDL_EMFS_File* stream = file.source == FileSource::archive
		? file.archive->file
		: file.loose_file;
	SDL_IOStatus status = SDL_IO_STATUS_READY;
	request.transferred = SDL_EMFS_ReadAt(
		stream,
		file.base_offset + offset,
		destination,
		byte_count,
		&status);
	request.status = request.transferred == byte_count
		&& status != SDL_IO_STATUS_ERROR
		? IoStatus::complete
		: IoStatus::failed;
	return request.status;
}

void vfs_poll(Vfs&, ReadRequest&)
{
}

void vfs_close(Vfs&, VfsFile& file)
{
	if (file.source == FileSource::loose)
	{
		SDL_EMFS_Close(file.loose_file);
	}
	file = {};
}

bool vfs_read_all(Vfs& vfs, const char* logical_path, sl_open::Blob& output)
{
	VfsFile file;
	if (!vfs_open(vfs, logical_path, file)
		|| file.size > SIZE_MAX
		|| !output.allocate(static_cast<std::size_t>(file.size)))
	{
		return false;
	}

	ReadRequest request;
	const IoStatus status = vfs_read_at(
		vfs, file, 0, output.data, output.size, request);
	vfs_close(vfs, file);
	if (status != IoStatus::complete)
	{
		output.reset();
		return false;
	}
	return true;
}
}

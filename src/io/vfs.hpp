#pragma once

#include "core/blob.hpp"

#include <SDL_emfs/SDL_emfs_sync.h>

#include <cstddef>
#include <cstdint>

namespace sl_open::io
{
constexpr std::size_t kMaxPath = 1024;
constexpr std::size_t kMaxArchives = 6;
constexpr std::size_t kMaxArchiveEntries = 8192;
constexpr std::size_t kArchiveNameBytes = 128 * 1024;

enum class IoStatus : std::uint8_t
{
	complete,
	pending,
	failed,
};

struct ReadRequest
{
	IoStatus status{IoStatus::failed};
	std::size_t transferred{};
};

struct ArchiveEntry
{
	std::uint64_t offset{};
	std::uint64_t size{};
	std::uint32_t name_offset{};
};

struct Archive
{
	SDL_EMFS_File* file{};
	std::uint64_t size{};
	ArchiveEntry entries[kMaxArchiveEntries]{};
	char names[kArchiveNameBytes]{};
	std::uint32_t entry_count{};
	std::uint32_t name_bytes{};
	char path[kMaxPath]{};
};

enum class FileSource : std::uint8_t
{
	none,
	loose,
	archive,
};

struct VfsFile
{
	SDL_EMFS_File* loose_file{};
	Archive* archive{};
	std::uint64_t base_offset{};
	std::uint64_t size{};
	FileSource source{FileSource::none};
};

struct Vfs
{
	SDL_EMFS_Context* filesystem{};
	Archive archives[kMaxArchives]{};
	std::uint32_t archive_count{};
	bool loose_first{};
};

bool normalize_asset_path(const char* input, char* output, std::size_t capacity);
bool game_root_looks_valid(SDL_EMFS_Context* filesystem);

bool vfs_init(Vfs& vfs, SDL_EMFS_Context* filesystem, bool loose_first);
void vfs_shutdown(Vfs& vfs);
bool vfs_mount_archive(Vfs& vfs, const char* filename);
bool vfs_open(Vfs& vfs, const char* logical_path, VfsFile& file);
bool vfs_open_from_archive(
	Vfs& vfs,
	const char* archive_filename,
	const char* logical_path,
	VfsFile& file);
IoStatus vfs_read_at(
	Vfs& vfs,
	VfsFile& file,
	std::uint64_t offset,
	void* destination,
	std::size_t byte_count,
	ReadRequest& request);
void vfs_poll(Vfs& vfs, ReadRequest& request);
void vfs_close(Vfs& vfs, VfsFile& file);
bool vfs_read_all(Vfs& vfs, const char* logical_path, sl_open::Blob& output);
}

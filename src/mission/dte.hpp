#pragma once

#include "core/blob.hpp"

#include <cstddef>
#include <cstdint>

namespace sl_open::io
{
struct Vfs;
}

namespace sl_open::mission
{
constexpr std::size_t kDteHeaderBytes = 0x400;
constexpr std::size_t kDteHeaderSlots = 128;
constexpr std::size_t kDteSectionCount = 27;
constexpr std::size_t kMaxDteBytes = 0xfa000;

enum class DteLoadError : std::uint8_t
{
	none,
	not_found,
	decompression_failed,
	too_small,
	too_large,
	invalid_directory,
	section_capacity_exceeded,
};

struct DteSection
{
	std::uint32_t count_flags{};
	std::uint32_t offset{};
	std::uint32_t bytes{};
	std::uint16_t count{};
};

struct DteFile
{
	sl_open::Blob image;
	DteSection sections[kDteSectionCount]{};
	DteLoadError error{DteLoadError::none};
};

bool dte_load(
	io::Vfs& vfs,
	const char* path,
	DteFile& file);
const std::uint8_t* dte_section_data(
	const DteFile& file,
	std::size_t section);
const char* dte_load_error_text(DteLoadError error);
}

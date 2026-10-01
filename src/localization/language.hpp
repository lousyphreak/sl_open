#pragma once

#include "core/blob.hpp"

#include <cstdint>

namespace sl_open::io
{
struct Vfs;
}

namespace sl_open
{
constexpr std::uint32_t kMaxLanguageStrings = 2048;

struct LanguageTable
{
	Blob text;
	std::uint32_t offsets[kMaxLanguageStrings]{};
	std::uint16_t lengths[kMaxLanguageStrings]{};
	std::uint32_t highest_id{};
};

bool parse_language_dll(Blob file, LanguageTable& language);
bool load_language_dll(
	io::Vfs& vfs,
	const char* path,
	LanguageTable& language);
const char* language_text(const LanguageTable& language, std::uint32_t id);
}

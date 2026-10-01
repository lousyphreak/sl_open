#pragma once

#include <cstdint>
#include <cstddef>
#include <cstring>

namespace sl_open::frontend::gui
{
struct Rect
{
	float x;
	float y;
	float width;
	float height;
};

enum class TextAlign : std::uint8_t
{
	left,
	center,
	right,
};

constexpr bool hit_open(const Rect& rect, float x, float y)
{
	return x > rect.x && y > rect.y
		&& x < rect.x + rect.width
		&& y < rect.y + rect.height;
}

template<typename RectType>
constexpr bool hit_open(const RectType& rect, float x, float y)
{
	return x > rect.x && y > rect.y
		&& x < rect.x + rect.width
		&& y < rect.y + rect.height;
}

constexpr bool hit_half_open(const Rect& rect, float x, float y)
{
	return x >= rect.x && y >= rect.y
		&& x < rect.x + rect.width
		&& y < rect.y + rect.height;
}

template<typename RectType>
constexpr bool hit_half_open(const RectType& rect, float x, float y)
{
	return x >= rect.x && y >= rect.y
		&& x < rect.x + rect.width
		&& y < rect.y + rect.height;
}

template<typename Glyph>
float text_width(
	const Glyph* glyphs,
	std::uint32_t glyph_count,
	const char* text,
	float scale = 1.0f)
{
	float width = 0.0f;
	for (const auto* character =
		reinterpret_cast<const std::uint8_t*>(text);
		*character != 0;
		++character)
	{
		if (*character < glyph_count)
		{
			width += glyphs[*character].width * scale;
		}
	}
	return width;
}

template<typename Glyph>
float text_width(
	const Glyph* glyphs,
	std::uint32_t glyph_count,
	const char* text,
	std::size_t length,
	float scale)
{
	float width = 0.0f;
	for (std::size_t index = 0; index < length; ++index)
	{
		const auto character = static_cast<std::uint8_t>(text[index]);
		if (character < glyph_count)
		{
			width += glyphs[character].width * scale;
		}
	}
	return width;
}

constexpr float aligned_x(float anchor, float width, TextAlign alignment)
{
	switch (alignment)
	{
	case TextAlign::center: return anchor - width * 0.5f;
	case TextAlign::right: return anchor - width;
	default: return anchor;
	}
}

struct TextWrapCursor
{
	const char* text{};
};

template<typename Glyph, std::size_t Capacity>
bool next_wrapped_line(
	TextWrapCursor& cursor,
	const Glyph* glyphs,
	std::uint32_t glyph_count,
	float scale,
	float maximum_width,
	char (&line)[Capacity])
{
	static_assert(Capacity > 1);
	line[0] = '\0';
	if (cursor.text == nullptr || *cursor.text == '\0')
	{
		return false;
	}

	std::size_t line_length = 0;
	float line_width = 0.0f;
	const float space_width =
		text_width(glyphs, glyph_count, " ", scale);
	while (*cursor.text != '\0')
	{
		while (*cursor.text == ' ') ++cursor.text;
		if (*cursor.text == '\0') break;

		const char* end = cursor.text;
		while (*end != '\0' && *end != ' ' && *end != '\n') ++end;
		const std::size_t length =
			static_cast<std::size_t>(end - cursor.text);
		const std::size_t copied = length < 127 ? length : 127;
		const float word_width = text_width(
			glyphs, glyph_count, cursor.text, copied, scale);
		if (line_length != 0
			&& line_width + space_width + word_width > maximum_width)
		{
			line[line_length] = '\0';
			return true;
		}
		if (line_length != 0 && line_length + 1 < Capacity)
		{
			line[line_length++] = ' ';
			line_width += space_width;
		}
		const std::size_t available = Capacity - 1 - line_length;
		const std::size_t append = copied < available ? copied : available;
		std::memcpy(line + line_length, cursor.text, append);
		line_length += append;
		line_width += word_width;
		cursor.text = *end == '\0' ? end : end + 1;
		if (*end == '\n')
		{
			line[line_length] = '\0';
			return true;
		}
	}
	line[line_length] = '\0';
	return line_length != 0;
}
}

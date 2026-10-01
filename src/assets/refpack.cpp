#include "assets/refpack.hpp"

#include "io/endian.hpp"

#include <cstring>

namespace sl_open::assets
{
bool refpack_decompress(const sl_open::Blob& source, sl_open::Blob& output)
{
	if (source.size < 5 || sl_open::io::read_be16(source.data) != 0x10fb)
	{
		return false;
	}

	const std::size_t expected = sl_open::io::read_be24(source.data + 2);
	if (!output.allocate(expected))
	{
		return false;
	}

	std::size_t input = 5;
	std::size_t produced = 0;
	auto take = [&](std::uint8_t& value) {
		if (input >= source.size)
		{
			return false;
		}
		value = source.data[input++];
		return true;
	};
	auto copy_literals = [&](std::size_t count) {
		if (!sl_open::io::range_fits(source.size, input, count)
			|| !sl_open::io::range_fits(output.size, produced, count))
		{
			return false;
		}
		std::memcpy(output.data + produced, source.data + input, count);
		input += count;
		produced += count;
		return true;
	};
	auto copy_back_reference = [&](std::size_t offset, std::size_t count) {
		if (offset == 0 || offset > produced
			|| !sl_open::io::range_fits(output.size, produced, count))
		{
			return false;
		}
		for (std::size_t index = 0; index < count; ++index)
		{
			output.data[produced] = output.data[produced - offset];
			++produced;
		}
		return true;
	};

	for (;;)
	{
		std::uint8_t control = 0;
		if (!take(control))
		{
			output.reset();
			return false;
		}

		if (control >= 0xfc)
		{
			if (!copy_literals(control & 3))
			{
				output.reset();
				return false;
			}
			break;
		}
		if (control >= 0xe0)
		{
			if (!copy_literals(((control & 0x1f) << 2) + 4))
			{
				output.reset();
				return false;
			}
			continue;
		}

		std::uint8_t first = 0;
		std::uint8_t second = 0;
		std::uint8_t third = 0;
		std::size_t literal_count = 0;
		std::size_t offset = 0;
		std::size_t count = 0;
		if (control >= 0xc0)
		{
			if (!take(first) || !take(second) || !take(third))
			{
				output.reset();
				return false;
			}
			literal_count = control & 3;
			offset = ((control & 0x10) << 12)
				+ (static_cast<std::size_t>(first) << 8)
				+ second + 1;
			count = ((control & 0x0c) << 6) + third + 5;
		}
		else if (control >= 0x80)
		{
			if (!take(first) || !take(second))
			{
				output.reset();
				return false;
			}
			literal_count = first >> 6;
			offset = ((first & 0x3f) << 8) + second + 1;
			count = (control & 0x3f) + 4;
		}
		else
		{
			if (!take(first))
			{
				output.reset();
				return false;
			}
			literal_count = control & 3;
			offset = ((control & 0x60) << 3) + first + 1;
			count = ((control & 0x1c) >> 2) + 3;
		}

		if (!copy_literals(literal_count)
			|| !copy_back_reference(offset, count))
		{
			output.reset();
			return false;
		}
	}

	if (produced != expected)
	{
		output.reset();
		return false;
	}
	return true;
}

bool unwrap_refpack(sl_open::Blob&& source, sl_open::Blob& output)
{
	if (source.size >= 2 && sl_open::io::read_be16(source.data) == 0x10fb)
	{
		return refpack_decompress(source, output);
	}
	output = static_cast<sl_open::Blob&&>(source);
	return true;
}
}

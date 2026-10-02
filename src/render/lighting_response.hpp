#pragma once

#include <array>
#include <cmath>

namespace sl_open::render
{
inline const std::array<float, 4096>& lighting_response_table()
{
	// SR_math_tables_init (0x004c3048) passes a binary32 angle to sin,
	// rounds its result to binary32, then stores the half-sine response.
	// Share these samples between the retained GPU lookup and CPU fallback.
	static const std::array<float, 4096> table = [] {
		std::array<float, 4096> values;
		for (int index = 0; index < 4096; ++index)
		{
			const float angle = static_cast<float>(index - 2048)
				* (1.0f / 4096.0f) * 3.1415927410125732421875f;
			const float sine = static_cast<float>(
				std::sin(static_cast<double>(angle)));
			values[index] = static_cast<float>(
				(static_cast<double>(sine) + 1.0) * 0.5);
		}
		return values;
	}();
	return table;
}
}

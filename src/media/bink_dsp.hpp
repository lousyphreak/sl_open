/*
 * Bink DSP routines
 * Copyright (c) 2009 Konstantin Shishkov
 *
 * This file is part of FFmpeg and is licensed under the GNU Lesser General
 * Public License version 2.1 or later.
 */
#pragma once

#include <cstdint>

struct BinkDSPContext
{
	void (*idct_put)(std::uint8_t*, int, std::int32_t*);
	void (*idct_add)(std::uint8_t*, int, std::int32_t*);
	void (*scale_block)(const std::uint8_t[64], std::uint8_t*, int);
	void (*add_pixels8)(std::uint8_t*, std::int16_t*, int);
};

void ff_binkdsp_init(BinkDSPContext* context);

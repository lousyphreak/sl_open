#pragma once

#include "core/blob.hpp"

namespace sl_open::assets
{
bool refpack_decompress(const sl_open::Blob& source, sl_open::Blob& output);
bool unwrap_refpack(sl_open::Blob&& source, sl_open::Blob& output);
}

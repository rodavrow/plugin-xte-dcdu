// XTE-DCDU: libjpeg-turbo wrapper
// Copyright (C) 2026 David Rowlandson
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>
#include <vector>

namespace xtedcdu {

// Encode RGB (24-bit, top-down, no row padding) into a JPEG byte stream.
// Returns the number of bytes written into |out| (also resizes |out|).
// Returns 0 on failure.
size_t encode_jpeg_rgb(const uint8_t* rgb, int w, int h, int quality,
                       std::vector<uint8_t>& out);

} // namespace xtedcdu

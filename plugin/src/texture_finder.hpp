// XTE-DCDU: panel texture finder
//
// Copyright (C) 2026 David Rowlandson
//
// Texture-finding heuristic adapted from XTextureExtractor by Wayne Piekarski
// (https://github.com/waynepiekarski/XTextureExtractor), Copyright (C) Wayne
// Piekarski, licensed GPL-3.0-or-later. Modifications by David Rowlandson are
// also licensed GPL-3.0-or-later.
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <string>
#include <cstdint>

namespace xtedcdu {

struct TextureFinder {
    // Returns the absolute path to the loaded aircraft's
    // cockpit_3d/-PANELS-/Panel_Airliner.png, or empty string if it can't
    // resolve it. Uses XPLM datarefs for the .acf path.
    static std::string find_panel_png_path();

    // Parse PNG width and height from the file at |png_path|. Returns true on
    // success. Does not require libpng.
    static bool read_png_dimensions(const std::string& png_path,
                                    int& width, int& height);

    // Brute-force scan of texture ids in [start_id+1, max_id] for one whose
    // dimensions match (panel_w, panel_h) AND whose internal format matches
    // |required_internal_format| (e.g. 0x8058 = GL_RGBA8, which is what the
    // ToLiss panel atlas uses; this matches XTE's heuristic). Pass 0 to
    // disable the format filter.
    //
    // Returns the matching GL texture id, or 0 if nothing was found.
    static unsigned int scan_for_panel_texture(int panel_w, int panel_h,
                                               int start_id = 0,
                                               int max_id   = 10000,
                                               int required_internal_format = 0x8058);
};

} // namespace xtedcdu

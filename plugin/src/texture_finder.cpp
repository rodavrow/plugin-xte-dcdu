// XTE-DCDU: panel texture finder
//
// Copyright (C) 2026 David Rowlandson
//
// Texture-finding heuristic adapted from XTextureExtractor by Wayne Piekarski
// (https://github.com/waynepiekarski/XTextureExtractor), Copyright (C) Wayne
// Piekarski, licensed GPL-3.0-or-later. Modifications by David Rowlandson are
// also licensed GPL-3.0-or-later.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "texture_finder.hpp"
#include "gl_loader.hpp"
#include "log.hpp"

#include "XPLMPlanes.h"

#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>

#if defined(_WIN32)
  #define PATHSEP '\\'
#else
  #define PATHSEP '/'
#endif

namespace xtedcdu {

std::string TextureFinder::find_panel_png_path() {
    char filename[512] = {0};
    char acf_path[1024] = {0};
    XPLMGetNthAircraftModel(0, filename, acf_path);
    if (acf_path[0] == 0) {
        XTED_ERR("texture_finder: XPLMGetNthAircraftModel returned empty path");
        return {};
    }

    // acf_path is the full path to the .acf file; strip filename to get the
    // aircraft directory.
    std::string p(acf_path);
    auto slash = p.find_last_of("/\\");
    if (slash == std::string::npos) return {};
    std::string dir = p.substr(0, slash);

    // Try several candidate locations / filenames used by ToLiss & friends.
    const char* candidates[] = {
        "/cockpit_3d/-PANELS-/Panel_Airliner.png",
        "/cockpit_3d/-PANELS-/Panel_General.png",
        "/cockpit_3d/-PANELS-/Panel.png",
    };
    for (const char* c : candidates) {
        std::string candidate = dir + c;
#if defined(_WIN32)
        for (auto& ch : candidate) if (ch == '/') ch = '\\';
#endif
        std::ifstream f(candidate, std::ios::binary);
        if (f.is_open()) {
            XTED_LOG("texture_finder: panel png at '%s'", candidate.c_str());
            return candidate;
        }
    }
    XTED_ERR("texture_finder: no panel PNG found near '%s'", dir.c_str());
    return {};
}

bool TextureFinder::read_png_dimensions(const std::string& png_path,
                                        int& width, int& height) {
    std::ifstream f(png_path, std::ios::binary);
    if (!f.is_open()) return false;
    unsigned char buf[24];
    f.read(reinterpret_cast<char*>(buf), 24);
    if (f.gcount() != 24) return false;

    // PNG signature
    static const unsigned char sig[8] = {137, 80, 78, 71, 13, 10, 26, 10};
    if (std::memcmp(buf, sig, 8) != 0) return false;

    // Width @ bytes 16..19 (BE), Height @ bytes 20..23 (BE)
    width  = (buf[16] << 24) | (buf[17] << 16) | (buf[18] << 8) | buf[19];
    height = (buf[20] << 24) | (buf[21] << 16) | (buf[22] << 8) | buf[23];
    return width > 0 && height > 0;
}

unsigned int TextureFinder::scan_for_panel_texture(int panel_w, int panel_h,
                                                   int start_id, int max_id,
                                                   int required_internal_format) {
    // Iterate texture ids; for each one that exists (glIsTexture), check
    // GL_TEXTURE_WIDTH / GL_TEXTURE_HEIGHT against the expected panel size
    // AND GL_TEXTURE_INTERNAL_FORMAT against |required_internal_format|.
    // This mirrors XTE's heuristic of matching format(32856) = GL_RGBA8.
    GLint prev_tex = 0;
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &prev_tex);

    if (start_id >= max_id) {
        XTED_ERR("texture_finder: invalid scan range %d..%d", start_id + 1, max_id);
        return 0;
    }

    XTED_LOG("texture_finder: scanning id range %d..%d (%dx%d fmt=0x%x)",
             start_id + 1, max_id, panel_w, panel_h,
             static_cast<unsigned>(required_internal_format));

    unsigned int found = 0;
    int match_count = 0;
    int dim_only_count = 0;
    for (int id = start_id + 1; id <= max_id; ++id) {
        if (!glIsTexture(static_cast<GLuint>(id))) continue;
        glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(id));
        GLint w = 0, h = 0, fmt = 0;
        glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH,  &w);
        glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT, &h);
        glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_INTERNAL_FORMAT, &fmt);
        if (w != panel_w || h != panel_h) continue;
        ++dim_only_count;
        if (required_internal_format != 0 && fmt != required_internal_format) {
            XTED_VLOG("texture_finder: skip id=%d (%dx%d, fmt=0x%x) - fmt mismatch",
                      id, w, h, static_cast<unsigned>(fmt));
            continue;
        }
        ++match_count;
        XTED_LOG("texture_finder: candidate #%d id=%d (%dx%d, fmt=0x%x)",
                 match_count, id, w, h, static_cast<unsigned>(fmt));
        if (found == 0) found = static_cast<unsigned int>(id);
    }

    glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(prev_tex));
    if (!found) {
        XTED_ERR("texture_finder: no %dx%d texture with fmt=0x%x found in %d..%d "
                 "(%d matched dims only)",
                 panel_w, panel_h, static_cast<unsigned>(required_internal_format),
                 start_id + 1, max_id, dim_only_count);
    } else {
        XTED_LOG("texture_finder: selected id=%u (%d format-matched candidates; "
                 "use xtedcdu/next_texture cmd to cycle)",
                 found, match_count);
    }
    return found;
}

} // namespace xtedcdu

// XTE-DCDU: FBO + PBO async readback of a texture sub-rectangle
// Copyright (C) 2026 David Rowlandson
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>
#include <vector>

namespace xtedcdu {

class Readback {
public:
    Readback() = default;
    ~Readback();

    // Allocate FBO + |pbo_count| PBOs sized for w*h*4 bytes (RGBA).
    // Must be called from the render thread with a current GL context.
    bool init(int crop_w, int crop_h, int pbo_count);

    // Free GL resources. Render-thread only.
    void shutdown();

    // Issue glReadPixels for the current frame and, if available, copy the
    // previous frame's PBO out into |out_rgb| (size = crop_w*crop_h*3, with
    // rows flipped to top-down). Returns true if a new frame was produced
    // into |out_rgb|.
    //
    // |panel_tex_id| is the GL texture id of the panel atlas.
    // |panel_h| is the full panel atlas height (for Y-axis flip).
    // |x1, y1, x2, y2| are top-left-origin pixel coordinates inside the
    // panel atlas.
    bool readback(unsigned int panel_tex_id, int panel_h,
                  int x1, int y1, int x2, int y2,
                  std::vector<uint8_t>& out_rgb);

    bool initialised() const { return initialised_; }

private:
    bool         initialised_ = false;
    int          w_ = 0, h_ = 0;
    int          pbo_count_ = 2;
    unsigned int fbo_ = 0;
    unsigned int pbos_[4] = {0,0,0,0};
    int          frame_index_ = 0;
    bool         have_pending_[4] = {false,false,false,false};
    bool         logged_fbo_error_ = false;
};

} // namespace xtedcdu

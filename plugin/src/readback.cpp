// XTE-DCDU: FBO + PBO async readback of a texture sub-rectangle
// Copyright (C) 2026 David Rowlandson
// SPDX-License-Identifier: GPL-3.0-or-later
#include "readback.hpp"
#include "gl_loader.hpp"
#include "log.hpp"

#include <algorithm>
#include <cstring>

namespace xtedcdu {

Readback::~Readback() { shutdown(); }

bool Readback::init(int crop_w, int crop_h, int pbo_count) {
    if (!gl::load()) {
        XTED_ERR("readback: failed to load required GL entry points");
        return false;
    }
    pbo_count = std::clamp(pbo_count, 1, 4);
    shutdown();
    w_ = crop_w; h_ = crop_h; pbo_count_ = pbo_count;

    gl::p_glGenFramebuffers(1, &fbo_);
    gl::p_glGenBuffers(pbo_count_, pbos_);

    GLint prev_pbo = 0;
    glGetIntegerv(GL_PIXEL_PACK_BUFFER_BINDING, &prev_pbo);

    for (int i = 0; i < pbo_count_; ++i) {
        gl::p_glBindBuffer(GL_PIXEL_PACK_BUFFER, pbos_[i]);
        gl::p_glBufferData(GL_PIXEL_PACK_BUFFER,
                           static_cast<ptrdiff_t>(w_) * h_ * 4,
                           nullptr, GL_STREAM_READ);
        have_pending_[i] = false;
    }
    gl::p_glBindBuffer(GL_PIXEL_PACK_BUFFER, static_cast<GLuint>(prev_pbo));

    initialised_ = true;
    frame_index_ = 0;
    logged_fbo_error_ = false;
    XTED_LOG("readback: initialised fbo=%u, %d PBOs, crop %dx%d",
             fbo_, pbo_count_, w_, h_);
    return true;
}

void Readback::shutdown() {
    if (!initialised_) return;
    if (fbo_)        gl::p_glDeleteFramebuffers(1, &fbo_);
    if (pbo_count_)  gl::p_glDeleteBuffers(pbo_count_, pbos_);
    fbo_ = 0;
    for (auto& p : pbos_) p = 0;
    for (auto& f : have_pending_) f = false;
    initialised_ = false;
}

bool Readback::readback(unsigned int panel_tex_id, int panel_h,
                        int x1, int y1, int x2, int y2,
                        std::vector<uint8_t>& out_rgb) {
    if (!initialised_) return false;
    if (panel_tex_id == 0) return false;

    const int w = x2 - x1;
    const int h = y2 - y1;
    if (w != w_ || h != h_) {
        XTED_ERR("readback: crop size changed (%d,%d) vs init (%d,%d)",
                 w, h, w_, h_);
        return false;
    }
    // Convert top-left coords to GL bottom-left.
    const int gl_y = panel_h - y2;

    const int issue = frame_index_ % pbo_count_;
    const int read  = (frame_index_ + 1) % pbo_count_;

    // Save state we touch.
    GLint prev_draw_fbo = 0, prev_read_fbo = 0, prev_pbo = 0, prev_pack_align = 4, prev_tex = 0;
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &prev_draw_fbo);
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &prev_read_fbo);
    glGetIntegerv(GL_PIXEL_PACK_BUFFER_BINDING, &prev_pbo);
    glGetIntegerv(GL_PACK_ALIGNMENT, &prev_pack_align);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &prev_tex);

    glPixelStorei(GL_PACK_ALIGNMENT, 1);

    gl::p_glBindFramebuffer(GL_READ_FRAMEBUFFER, fbo_);
    gl::p_glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                                 GL_TEXTURE_2D, panel_tex_id, 0);

    // Verify the FBO is complete before issuing a read.
    GLenum fbo_status = gl::p_glCheckFramebufferStatus(GL_READ_FRAMEBUFFER);
    if (fbo_status != GL_FRAMEBUFFER_COMPLETE) {
        if (!logged_fbo_error_) {
            logged_fbo_error_ = true;
            XTED_ERR("readback: FBO not complete (status=0x%x) for tex=%u; "
                     "readback disabled until next reinit",
                     static_cast<unsigned>(fbo_status), panel_tex_id);
        }
        gl::p_glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                                     GL_TEXTURE_2D, 0, 0);
        gl::p_glBindFramebuffer(GL_READ_FRAMEBUFFER, static_cast<GLuint>(prev_read_fbo));
        gl::p_glBindFramebuffer(GL_DRAW_FRAMEBUFFER, static_cast<GLuint>(prev_draw_fbo));
        glPixelStorei(GL_PACK_ALIGNMENT, prev_pack_align);
        glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(prev_tex));
        return false;
    }

    // Issue async readback into the current "issue" PBO.
    gl::p_glBindBuffer(GL_PIXEL_PACK_BUFFER, pbos_[issue]);
    glReadPixels(x1, gl_y, w, h, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    have_pending_[issue] = true;

    bool produced = false;
    if (have_pending_[read] && pbo_count_ > 1) {
        gl::p_glBindBuffer(GL_PIXEL_PACK_BUFFER, pbos_[read]);
        const ptrdiff_t bytes = static_cast<ptrdiff_t>(w) * h * 4;
        void* mapped = gl::p_glMapBufferRange(GL_PIXEL_PACK_BUFFER, 0, bytes, GL_MAP_READ_BIT);
        if (mapped) {
            // Convert RGBA bottom-up to RGB top-down.
            out_rgb.resize(static_cast<size_t>(w) * h * 3);
            const uint8_t* src = static_cast<const uint8_t*>(mapped);
            for (int row = 0; row < h; ++row) {
                const uint8_t* s = src + static_cast<size_t>(h - 1 - row) * w * 4;
                uint8_t* d       = out_rgb.data() + static_cast<size_t>(row) * w * 3;
                for (int col = 0; col < w; ++col) {
                    d[0] = s[0]; d[1] = s[1]; d[2] = s[2];
                    s += 4; d += 3;
                }
            }
            gl::p_glUnmapBuffer(GL_PIXEL_PACK_BUFFER);
            produced = true;
        } else {
            XTED_VLOG("readback: glMapBufferRange returned null");
        }
        have_pending_[read] = false;
    } else if (pbo_count_ == 1) {
        // Synchronous map of the just-issued PBO (slow path).
        const ptrdiff_t bytes = static_cast<ptrdiff_t>(w) * h * 4;
        void* mapped = gl::p_glMapBufferRange(GL_PIXEL_PACK_BUFFER, 0, bytes, GL_MAP_READ_BIT);
        if (mapped) {
            out_rgb.resize(static_cast<size_t>(w) * h * 3);
            const uint8_t* src = static_cast<const uint8_t*>(mapped);
            for (int row = 0; row < h; ++row) {
                const uint8_t* s = src + static_cast<size_t>(h - 1 - row) * w * 4;
                uint8_t* d       = out_rgb.data() + static_cast<size_t>(row) * w * 3;
                for (int col = 0; col < w; ++col) {
                    d[0] = s[0]; d[1] = s[1]; d[2] = s[2];
                    s += 4; d += 3;
                }
            }
            gl::p_glUnmapBuffer(GL_PIXEL_PACK_BUFFER);
            produced = true;
        }
        have_pending_[issue] = false;
    }

    // Restore state.
    gl::p_glBindBuffer(GL_PIXEL_PACK_BUFFER, static_cast<GLuint>(prev_pbo));
    // Detach our texture from the FBO before restoring binding to be safe.
    gl::p_glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                                 GL_TEXTURE_2D, 0, 0);
    gl::p_glBindFramebuffer(GL_READ_FRAMEBUFFER, static_cast<GLuint>(prev_read_fbo));
    gl::p_glBindFramebuffer(GL_DRAW_FRAMEBUFFER, static_cast<GLuint>(prev_draw_fbo));
    glPixelStorei(GL_PACK_ALIGNMENT, prev_pack_align);
    glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(prev_tex));

    frame_index_++;
    return produced;
}

} // namespace xtedcdu

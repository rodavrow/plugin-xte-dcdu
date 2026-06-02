// XTE-DCDU: xxhash-backed change detector
// Copyright (C) 2026 David Rowlandson
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>
#include <cstddef>

namespace xtedcdu {

class ChangeDetector {
public:
    // Returns true if the buffer hash differs from the previous accepted
    // hash. The first call always returns true. NOTE: does NOT update the
    // stored hash -- call accept(h) once the frame has actually been sent.
    // This avoids "poisoning" the stored hash with a frame that was
    // detected as changed but then dropped (e.g. by the rate limiter),
    // which would otherwise cause the device display to lag behind real
    // content until the next *different-again* frame arrived.
    bool differs(const void* data, size_t size, uint64_t* out_hash) const;

    // Commit the given hash as the new baseline. Call this only after the
    // corresponding frame has actually been sent to the device.
    void accept(uint64_t h) { prev_ = h; has_prev_ = true; }

    void reset() { has_prev_ = false; prev_ = 0; }

private:
    uint64_t prev_ = 0;
    bool     has_prev_ = false;
};

} // namespace xtedcdu

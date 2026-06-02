// XTE-DCDU: xxhash-backed change detector
// Copyright (C) 2026 David Rowlandson
// SPDX-License-Identifier: GPL-3.0-or-later
#include "change_detect.hpp"

#define XXH_INLINE_ALL
#define XXH_STATIC_LINKING_ONLY
#include "xxhash.h"

namespace xtedcdu {

bool ChangeDetector::differs(const void* data, size_t size,
                             uint64_t* out_hash) const {
    uint64_t h = XXH3_64bits(data, size);
    if (out_hash) *out_hash = h;
    return !has_prev_ || h != prev_;
}

} // namespace xtedcdu

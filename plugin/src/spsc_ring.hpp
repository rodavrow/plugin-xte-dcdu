// XTE-DCDU: tiny single-producer / single-consumer ring buffer
// Copyright (C) 2026 David Rowlandson
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <utility>

namespace xtedcdu {

// Lock-free SPSC ring of fixed power-of-two capacity. Push from one thread,
// pop from another. push() returns false if full (caller may evict oldest by
// popping then pushing again).
template <typename T, size_t Capacity>
class SpscRing {
    static_assert((Capacity & (Capacity - 1)) == 0, "Capacity must be power of two");
public:
    bool push(T&& v) {
        const size_t h = head_.load(std::memory_order_relaxed);
        const size_t t = tail_.load(std::memory_order_acquire);
        if (h - t >= Capacity) return false;
        slots_[h & (Capacity - 1)] = std::move(v);
        head_.store(h + 1, std::memory_order_release);
        return true;
    }
    bool pop(T& out) {
        const size_t t = tail_.load(std::memory_order_relaxed);
        const size_t h = head_.load(std::memory_order_acquire);
        if (h == t) return false;
        out = std::move(slots_[t & (Capacity - 1)]);
        tail_.store(t + 1, std::memory_order_release);
        return true;
    }
    bool empty() const {
        return head_.load(std::memory_order_acquire) ==
               tail_.load(std::memory_order_acquire);
    }
private:
    std::array<T, Capacity> slots_{};
    std::atomic<size_t>     head_{0};
    std::atomic<size_t>     tail_{0};
};

} // namespace xtedcdu

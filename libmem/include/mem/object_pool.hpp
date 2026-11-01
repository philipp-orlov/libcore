// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// Typed pool for long-lived, frequently recycled objects (sessions, connections, call records).
// Slots come from a SlabPool (cache-line aligned, never returned to the OS, misuse detected); acquire() constructs
// in place and release() runs the destructor. Zero heap traffic in steady state.
#pragma once

#include <cstddef>
#include <new>
#include <utility>

#include "mem/slab_pool.hpp"

namespace mem {

template <class T>
class ObjectPool {
public:
    struct Options {
        std::size_t slabBytes = std::size_t{256} << 10;
        unsigned shards = 8;
        unsigned threadCacheBlocks = 8;
        std::size_t maxBytes = 0;
        Hooks hooks{};
    };

    ObjectPool() : ObjectPool(Options{}) {}
    explicit ObjectPool(const Options& o) : slabs_(make(o)) {}

    // nullptr when the pool is at its limit; throws only what T's constructor throws (the slot is returned first).
    template <class... A>
    T* acquire(A&&... args) {
        void* slot = slabs_.tryAcquire();
        if (!slot)
            return nullptr;
        try {
            return ::new (slot) T(std::forward<A>(args)...);
        } catch (...) {
            slabs_.release(slot);
            throw;
        }
    }
    void release(T* object) noexcept {
        if (!object)
            return;
        object->~T();
        slabs_.release(object);
    }
    void prewarm(std::size_t objects) noexcept { slabs_.prewarm(objects); }
    Stats stats() const noexcept { return slabs_.stats(); }

private:
    static SlabPoolOptions make(const Options& o) {
        SlabPoolOptions s;
        s.blockBytes = (sizeof(T) + 63) & ~std::size_t{63};
        s.slabBytes = o.slabBytes;
        while (s.slabBytes < s.blockBytes * 16)
            s.slabBytes <<= 1;
        s.shards = o.shards;
        s.threadCacheBlocks = o.threadCacheBlocks;
        s.maxBytes = o.maxBytes;
        s.hooks = o.hooks;
        s.name = "object";
        static_assert(alignof(T) <= 64, "ObjectPool slots are 64-byte aligned");
        return s;
    }
    SlabPool slabs_;
};

}  // namespace mem

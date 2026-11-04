// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// An Arena for objects that do have destructors: syntax trees and symbol tables, whose nodes own strings and vectors.
// make<T> registers the destructor in the arena itself, so there is no side table to grow; reset() runs them newest
// first and gives the memory back in one go. A compilation allocates freely and releases at once: no node is ever
// freed on its own, and compiling a million programs leaves nothing behind.
#pragma once

#include <cstddef>
#include <new>
#include <type_traits>
#include <utility>

#include "mem/arena.hpp"

namespace mem {

class ObjectArena {
public:
    ObjectArena() = default;
    explicit ObjectArena(Arena::Options options) : arena_(options) {}
    ~ObjectArena() { reset(); }
    ObjectArena(const ObjectArena&) = delete;
    ObjectArena& operator=(const ObjectArena&) = delete;

    // The object lives until reset(). If its constructor throws, nothing is registered.
    template <class T, class... A>
    T* make(A&&... args) {
        static_assert(alignof(T) <= 4096, "Arena alignment limit");
        void* memory = arena_.allocate(sizeof(T), alignof(T));
        T* object = ::new (memory) T(std::forward<A>(args)...);
        if constexpr (!std::is_trivially_destructible_v<T>) {
            auto* record = static_cast<Finalizer*>(arena_.allocate(sizeof(Finalizer), alignof(Finalizer)));
            record->destroy = [](void* p) noexcept { static_cast<T*>(p)->~T(); };
            record->object = object;
            record->older = newest_;
            newest_ = record;
        }
        return object;
    }

    void reset() noexcept {
        for (Finalizer* f = newest_; f; f = f->older)
            f->destroy(f->object);
        newest_ = nullptr;
        arena_.reset();
    }

    std::size_t usedBytes() const noexcept { return arena_.usedBytes(); }

private:
    struct Finalizer {
        void (*destroy)(void*) noexcept;
        void* object;
        Finalizer* older;
    };
    Arena arena_;
    Finalizer* newest_ = nullptr;
};

}  // namespace mem

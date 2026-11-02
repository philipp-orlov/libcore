// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// Bump allocator over chunks drawn from a SizeClassPool.
//   * Chunks never move, so raw pointers stay valid across further allocation, mark() and rewind() (libnosql's
//     b-tree code holds pointers across recursion).
//   * reset()/rewind() keep up to `retainChunks` standard-size chunks for the next round, so a statement that runs
//     again does not go back to the pool, and the pool still gets the rest back: no growth, no churn.
//   * Objects are never destroyed: create<T> accepts trivially destructible types only.
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <new>
#include <type_traits>

#include "mem/size_class_pool.hpp"

namespace mem {

class Arena {
public:
    struct Options {
        SizeClassPool* source = nullptr;      // null = SizeClassPool::global()
        std::size_t chunkBytes = 64 << 10;    // standard chunk size (rounded up to a pool class)
        std::size_t retainChunks = 4;
    };

    Arena() : Arena(Options{}) {}
    explicit Arena(Options options);
    ~Arena();
    Arena(const Arena&) = delete;
    Arena& operator=(const Arena&) = delete;

    // Throws std::bad_alloc when the source is exhausted. `align` must be a power of two <= 4096.
    void* allocate(std::size_t bytes, std::size_t align = alignof(std::max_align_t));
    // nullptr instead of throwing.
    void* tryAllocate(std::size_t bytes, std::size_t align = alignof(std::max_align_t)) noexcept;

    template <class T, class... A>
    T* create(A&&... args) {
        static_assert(std::is_trivially_destructible_v<T>, "Arena never runs destructors");
        return ::new (allocate(sizeof(T), alignof(T))) T(static_cast<A&&>(args)...);
    }
    template <class T>
    T* allocateArray(std::size_t n) {
        static_assert(std::is_trivially_destructible_v<T>, "Arena never runs destructors");
        if (n > (~std::size_t{0}) / sizeof(T))
            throw std::bad_alloc();
        return static_cast<T*>(allocate(n * sizeof(T), alignof(T)));
    }

    struct Mark {
        void* chunk = nullptr;
        std::size_t offset = 0;
    };
    Mark mark() const noexcept;
    void rewind(Mark m) noexcept;             // frees everything allocated after the mark
    class Scope {
    public:
        explicit Scope(Arena& a) noexcept : arena_(a), mark_(a.mark()) {}
        ~Scope() { arena_.rewind(mark_); }
        Scope(const Scope&) = delete;
        Scope& operator=(const Scope&) = delete;
    private:
        Arena& arena_;
        Mark mark_;
    };

    void reset() noexcept;                    // rewind to empty
    std::size_t usedBytes() const noexcept { return used_; }
    std::size_t retainedBytes() const noexcept;   // bytes held in spare chunks
    std::size_t chunkCount() const noexcept { return chunks_ + spareCount_; }
    std::size_t chunkBytes() const noexcept { return chunkBytes_; }   // standard chunk size, as the pool rounds it

    // For std::pmr containers: deallocate is a no-op, memory comes back on reset()/rewind().
    std::pmr::memory_resource* resource() noexcept { return &resource_; }

private:
    struct Chunk;
    class Resource final : public std::pmr::memory_resource {
    public:
        explicit Resource(Arena& a) : arena_(a) {}
    private:
        void* do_allocate(std::size_t bytes, std::size_t align) override { return arena_.allocate(bytes, align); }
        void do_deallocate(void*, std::size_t, std::size_t) override {}
        bool do_is_equal(const std::pmr::memory_resource& o) const noexcept override { return this == &o; }
        Arena& arena_;
    };

    Chunk* newChunk(std::size_t minPayload) noexcept;
    void dropChunk(Chunk* c) noexcept;    // to the spare list or back to the source

    SizeClassPool* source_;
    std::size_t chunkBytes_, retain_;
    Chunk* top_ = nullptr;                // current chunk; older chunks via ->prev
    Chunk* spare_ = nullptr;              // retained standard-size chunks
    std::size_t spareCount_ = 0, chunks_ = 0, used_ = 0;
    Resource resource_;
};

}  // namespace mem

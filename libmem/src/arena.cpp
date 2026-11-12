// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

#include "mem/arena.hpp"

#include <algorithm>

#include "base/status.hpp"

namespace mem {

struct Arena::Chunk {
    Chunk* prev;                       // older chunk (or next spare)
    SizeClassPool::Block block;
    std::size_t used;                  // bytes consumed including this header
    std::size_t usedBefore;            // Arena::used_ when this chunk became current (for usedBytes after rewind)
    bool standard;
};

namespace {
constexpr std::size_t kHeader = 64;   // the chunk header is padded to a cache line; payload follows
inline std::size_t alignUp(std::size_t v, std::size_t a) noexcept { return (v + a - 1) & ~(a - 1); }
}  // namespace

Arena::Arena(Options o)
    : source_(o.source ? o.source : &SizeClassPool::global()), chunkBytes_(o.chunkBytes), retain_(o.retainChunks), resource_(*this) {
    // Round the standard size to what the pool really hands out so nothing is wasted.
    if (chunkBytes_ < 4096)
        chunkBytes_ = 4096;
    const std::size_t cls = source_->classes().classOf(chunkBytes_);
    if (cls == ClassTable::npos)
        throw base::Error(base::Code::InvalidArgument, "Arena: chunkBytes larger than the pool's largest class");
    chunkBytes_ = source_->classes().bytesOf(cls);
}

Arena::~Arena() {
    reset();
    while (spare_) {
        Chunk* c = spare_;
        spare_ = c->prev;
        source_->release(c->block);
    }
}

Arena::Chunk* Arena::newChunk(std::size_t minPayload) noexcept {
    // A retained chunk serves any request that fits the standard size.
    if (spare_ && minPayload + kHeader <= chunkBytes_) {
        Chunk* c = spare_;
        spare_ = c->prev;
        --spareCount_;
        c->prev = nullptr;
        c->used = kHeader;
        return c;
    }
    const std::size_t want = std::max(chunkBytes_, minPayload + kHeader);
    SizeClassPool::Block b = source_->tryAcquire(want);
    if (!b.data)
        return nullptr;
    auto* c = static_cast<Chunk*>(b.data);
    c->prev = nullptr;
    c->block = b;
    c->used = kHeader;
    c->standard = (b.capacity == chunkBytes_);
    return c;
}

void Arena::dropChunk(Chunk* c) noexcept {
    if (c->standard && spareCount_ < retain_) {
        c->prev = spare_;
        spare_ = c;
        ++spareCount_;
    } else {
        source_->release(c->block);
    }
}

void* Arena::tryAllocate(std::size_t bytes, std::size_t align) noexcept {
    if (align == 0 || (align & (align - 1)) != 0 || align > 4096)
        return nullptr;
    if (bytes == 0)
        bytes = 1;
    if (top_) {
        const std::size_t at = alignUp(reinterpret_cast<std::uintptr_t>(top_->block.data) + top_->used, align) -
                               reinterpret_cast<std::uintptr_t>(top_->block.data);
        if (bytes <= top_->block.capacity && at <= top_->block.capacity - bytes) {
            top_->used = at + bytes;
            used_ += bytes;
            return static_cast<char*>(top_->block.data) + at;
        }
    }
    if (bytes > SIZE_MAX - align - 64)
        return nullptr;  // the request cannot be met; a sum below would wrap
    Chunk* c = newChunk(bytes + align);
    if (!c)
        return nullptr;
    c->prev = top_;
    top_ = c;
    ++chunks_;
    const std::size_t at = alignUp(reinterpret_cast<std::uintptr_t>(c->block.data) + c->used, align) -
                           reinterpret_cast<std::uintptr_t>(c->block.data);
    c->used = at + bytes;
    used_ += bytes;
    return static_cast<char*>(c->block.data) + at;
}

void* Arena::allocate(std::size_t bytes, std::size_t align) {
    void* p = tryAllocate(bytes, align);
    if (!p)
        throw std::bad_alloc();
    return p;
}

Arena::Mark Arena::mark() const noexcept { return {top_, top_ ? top_->used : 0}; }

void Arena::rewind(Mark m) noexcept {
    auto* target = static_cast<Chunk*>(m.chunk);
    while (top_ != target && top_) {
        Chunk* c = top_;
        top_ = c->prev;
        --chunks_;
        dropChunk(c);
    }
    if (top_)
        top_->used = m.offset;
    // used_ is recomputed lazily as the sum of live chunks: cheap because chunk counts are small.
    std::size_t sum = 0;
    for (Chunk* c = top_; c; c = c->prev)
        sum += c->used - kHeader;
    used_ = sum;
}

void Arena::reset() noexcept { rewind(Mark{}); }

std::size_t Arena::retainedBytes() const noexcept { return spareCount_ * chunkBytes_; }

}  // namespace mem

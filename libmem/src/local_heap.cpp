// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

#include "mem/local_heap.hpp"

#include <algorithm>

#include "sys/vm.hpp"

namespace mem {

// Lives at the start of the segment, in a block of its own; only its first page is ever touched.
struct LocalHeap::Segment {
    Segment* prevAll;
    Segment* nextAll;
    Segment* prevFree;
    Segment* nextFree;
    std::uint32_t freeMask;       // bit i: block i is free
    std::uint32_t residentMask;   // bit i: free block i has resident pages worth keeping
};

namespace {

constexpr std::size_t kBlocksPerSegment = LocalHeap::kSegmentBytes / LocalHeap::kBlockBytes;
static_assert(kBlocksPerSegment == 32, "the block masks are 32 bits wide");
constexpr std::uint32_t kAllBlocks = ~std::uint32_t{1};  // block 0 holds the segment header

template <class S>
S* segmentOf(void* block) noexcept {
    return reinterpret_cast<S*>(reinterpret_cast<std::uintptr_t>(block) & ~(std::uintptr_t{LocalHeap::kSegmentBytes} - 1));
}

#if defined(_MSC_VER)
unsigned lowestBit(std::uint32_t mask) noexcept {
    unsigned long index = 0;
    _BitScanForward(&index, mask);
    return static_cast<unsigned>(index);
}
unsigned bitCount(std::uint32_t mask) noexcept { return static_cast<unsigned>(__popcnt(mask)); }
#else
unsigned lowestBit(std::uint32_t mask) noexcept { return static_cast<unsigned>(__builtin_ctz(mask)); }
unsigned bitCount(std::uint32_t mask) noexcept { return static_cast<unsigned>(__builtin_popcount(mask)); }
#endif

unsigned blockIndex(const void* block) noexcept {
    return static_cast<unsigned>((reinterpret_cast<std::uintptr_t>(block) & (LocalHeap::kSegmentBytes - 1)) / LocalHeap::kBlockBytes);
}

}  // namespace

// The block of a class with no real block: it has no free slot, so the fast path falls through to allocateSlow, and it
// is never freed into because no pointer belongs to it.
namespace {
struct alignas(64) EmptyBlockStorage {
    unsigned char bytes[64] = {};
};
EmptyBlockStorage emptyBlockStorage;
}  // namespace

LocalHeap::LocalHeap(Options options)
    : options_(options), large_(options.large ? options.large : &SizeClassPool::global()) {
    auto* empty = reinterpret_cast<Block*>(&emptyBlockStorage);
    for (Class& cls : classes_)
        cls.current = empty;
    if (options_.idleFraction == 0)
        options_.idleFraction = 1;
}

LocalHeap::~LocalHeap() {
    while (segments_)
        unmapSegment(segments_);
}

LocalHeap::Segment* LocalHeap::mapSegment() {
    const sys::VmRegion region = sys::tryVmMapAligned(kSegmentBytes, kSegmentBytes);
    if (!region.base)
        throw std::bad_alloc();
    sys::vmAdviseNoHuge(region.base, region.bytes);  // this heap gives pages back by the block
    auto* segment = static_cast<Segment*>(region.base);
    segment->prevAll = nullptr;
    segment->nextAll = segments_;
    if (segments_)
        segments_->prevAll = segment;
    segments_ = segment;
    segment->prevFree = nullptr;
    segment->nextFree = withFree_;
    if (withFree_)
        withFree_->prevFree = segment;
    withFree_ = segment;
    segment->freeMask = kAllBlocks;
    segment->residentMask = 0;
    ++segmentCount_;
    return segment;
}

void LocalHeap::unmapSegment(Segment* segment) noexcept {
    if (segment->prevAll)
        segment->prevAll->nextAll = segment->nextAll;
    else
        segments_ = segment->nextAll;
    if (segment->nextAll)
        segment->nextAll->prevAll = segment->prevAll;
    if (segment->freeMask) {
        if (segment->prevFree)
            segment->prevFree->nextFree = segment->nextFree;
        else
            withFree_ = segment->nextFree;
        if (segment->nextFree)
            segment->nextFree->prevFree = segment->prevFree;
    }
    idleResident_ -= bitCount(segment->residentMask & segment->freeMask);
    --segmentCount_;
    unpoison(segment, kSegmentBytes);
    sys::vmUnmap(segment, kSegmentBytes);
}

LocalHeap::Block* LocalHeap::takeBlock(std::size_t cls) {
    Segment* segment = withFree_ ? withFree_ : mapSegment();
    // A block whose pages are resident first: it costs no page faults.
    const std::uint32_t warm = segment->freeMask & segment->residentMask;
    const unsigned index = lowestBit(warm ? warm : segment->freeMask);
    const std::uint32_t bit = std::uint32_t{1} << index;
    if (warm)
        --idleResident_;
    segment->freeMask &= ~bit;
    segment->residentMask &= ~bit;
    if (segment->freeMask == 0) {
        withFree_ = segment->nextFree;
        if (withFree_)
            withFree_->prevFree = nullptr;
        segment->nextFree = nullptr;
    }

    char* base = reinterpret_cast<char*>(segment) + index * kBlockBytes;
    const std::size_t slotBytes = slotBytesOf(cls);
    auto* block = ::new (base) Block();
    block->bump = base + kBlockHeader;
    block->end = block->bump + (kBlockBytes - kBlockHeader) / slotBytes * slotBytes;
    block->slotBytes = static_cast<std::uint32_t>(slotBytes);
    block->cls = static_cast<std::uint32_t>(cls);
    ++blocksInUse_;
    return block;
}

void LocalHeap::releaseBlock(Block* block) noexcept {
    auto* segment = segmentOf<Segment>(block);
    const std::uint32_t bit = std::uint32_t{1} << blockIndex(block);
    unpoison(block, kBlockBytes);
    if (segment->freeMask == 0) {
        segment->prevFree = nullptr;
        segment->nextFree = withFree_;
        if (withFree_)
            withFree_->prevFree = segment;
        withFree_ = segment;
    }
    segment->freeMask |= bit;
    segment->residentMask |= bit;
    ++idleResident_;
    --blocksInUse_;
}

void LocalHeap::unlinkPartial(Class& cls, Block* block) noexcept {
    if (block->prev)
        block->prev->next = block->next;
    else
        cls.partial = block->next;
    if (block->next)
        block->next->prev = block->prev;
    block->prev = block->next = nullptr;
}

void LocalHeap::pushPartial(Class& cls, Block* block) noexcept {
    block->prev = nullptr;
    block->next = cls.partial;
    if (cls.partial)
        cls.partial->prev = block;
    cls.partial = block;
}

// The current block has no slot left. It simply stops being current; it joins the partial list again when something in
// it is freed.
void* LocalHeap::allocateSlow(Class& cls) {
    Block* next = cls.partial;
    if (next)
        unlinkPartial(cls, next);
    else
        next = takeBlock(static_cast<std::size_t>(&cls - classes_));
    cls.current = next;
    return allocate(next->slotBytes);
}

void LocalHeap::blockChanged(Block* block, bool wasFull) noexcept {
    Class& cls = classes_[block->cls];
    if (block == cls.current)
        return;  // stays, even when empty: a class that empties and refills must not bounce
    if (block->live == 0) {
        if (!wasFull)
            unlinkPartial(cls, block);
        releaseBlock(block);
    } else if (wasFull) {
        pushPartial(cls, block);
    }
}

void* LocalHeap::allocateLarge(std::size_t bytes) {
    largeBytes_ += bytes;
    if (bytes == 0)
        return large_->acquire(1).data;
    if (bytes > large_->classes().maxBytes()) {
        const sys::VmRegion region = sys::tryVmMap(bytes);
        if (!region.base) {
            largeBytes_ -= bytes;
            throw std::bad_alloc();
        }
        return region.base;
    }
    try {
        return large_->acquire(bytes).data;
    } catch (...) {
        largeBytes_ -= bytes;
        throw;
    }
}

void LocalHeap::deallocateLarge(void* p, std::size_t bytes) noexcept {
    largeBytes_ -= bytes;
    const ClassTable& classes = large_->classes();
    if (bytes > classes.maxBytes()) {
        sys::vmUnmap(p, sys::roundUpToPage(bytes));
        return;
    }
    large_->release({p, classes.bytesOf(classes.classOf(bytes ? bytes : 1))});
}

void LocalHeap::trim() noexcept {
    // A class that went quiet gives up its last block as well.
    auto* empty = reinterpret_cast<Block*>(&emptyBlockStorage);
    for (Class& cls : classes_) {
        Block* block = cls.current;
        if (block == empty || block->live != 0)
            continue;
        Block* next = cls.partial;
        if (next)
            unlinkPartial(cls, next);
        cls.current = next ? next : empty;
        releaseBlock(block);
    }

    const std::uint64_t keep = std::max<std::uint64_t>(options_.minIdleBlocks, blocksInUse_ / options_.idleFraction);

    // Segments with nothing live in them go first, as long as what stays still covers the reserve.
    for (Segment* segment = withFree_; segment && idleResident_ > keep;) {
        Segment* next = segment->nextFree;
        if (segment->freeMask == kAllBlocks) {
            const auto resident = static_cast<std::uint64_t>(bitCount(segment->residentMask));
            if (idleResident_ >= keep + resident)
                unmapSegment(segment);
        }
        segment = next;
    }

    // What is still over goes back page-wise; the address range stays.
    for (Segment* segment = withFree_; segment && idleResident_ > keep; segment = segment->nextFree) {
        std::uint32_t resident = segment->freeMask & segment->residentMask;
        while (resident && idleResident_ > keep) {
            const unsigned index = lowestBit(resident);
            const std::uint32_t bit = std::uint32_t{1} << index;
            resident &= ~bit;
            sys::vmDiscard(reinterpret_cast<char*>(segment) + index * kBlockBytes, kBlockBytes);
            segment->residentMask &= ~bit;
            --idleResident_;
        }
    }
}

void LocalHeap::forEachSlot(void (*visit)(void* context, void* slot), void* context) const {
    constexpr std::size_t kMaxSlots = kBlockBytes / kGranule;
    for (const Segment* segment = segments_; segment; segment = segment->nextAll) {
        std::uint32_t used = ~segment->freeMask & kAllBlocks;
        while (used) {
            const unsigned index = lowestBit(used);
            used &= used - 1;
            auto* block = reinterpret_cast<Block*>(reinterpret_cast<std::uintptr_t>(segment) + index * kBlockBytes);
            char* first = reinterpret_cast<char*>(block) + kBlockHeader;
            const std::size_t carved = static_cast<std::size_t>(block->bump - first) / block->slotBytes;
            std::uint64_t isFree[kMaxSlots / 64 + 1] = {};
            for (const Slot* slot = block->free; slot; slot = slot->next) {
                const std::size_t at = static_cast<std::size_t>(reinterpret_cast<const char*>(slot) - first) / block->slotBytes;
                isFree[at / 64] |= std::uint64_t{1} << (at % 64);
            }
            for (std::size_t at = 0; at < carved; ++at) {
                if (!(isFree[at / 64] >> (at % 64) & 1))
                    visit(context, first + at * block->slotBytes);
            }
        }
    }
}

LocalHeap::Stats LocalHeap::stats() const noexcept {
    Stats s;
    s.mappedBytes = segmentCount_ * kSegmentBytes;
    s.committedBytes = (blocksInUse_ + idleResident_) * kBlockBytes;
    s.largeBytes = largeBytes_;
    s.blocksInUse = blocksInUse_;
    s.idleBlocks = idleResident_;
    s.segments = segmentCount_;
    return s;
}

}  // namespace mem

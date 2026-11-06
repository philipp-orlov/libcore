// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <random>
#include <vector>

#include "bench/alloc_budget.hpp"
#include "mem/local_heap.hpp"
#include "mini_test.hpp"
#include "sys/proc.hpp"
#include "sys/vm.hpp"

namespace {
struct Live {
    unsigned char* p;
    std::size_t bytes;
    unsigned char fill;
};

constexpr std::size_t kBlock = mem::LocalHeap::kBlockBytes;

bool underSanitizer() {
#if defined(MEM_LOCAL_HEAP_ASAN)
    return true;
#else
    return false;
#endif
}
}  // namespace

TEST(everySizeIsAlignedUsableAndClassedUpward) {
    mem::LocalHeap heap;
    for (std::size_t bytes = 1; bytes <= mem::LocalHeap::kMaxSmall + 64; ++bytes) {
        void* p = heap.allocate(bytes);
        CHECK_EQ(reinterpret_cast<std::uintptr_t>(p) % 16, 0u);
        CHECK(mem::LocalHeap::usableSize(bytes) >= bytes);
        std::memset(p, 0xAB, bytes);
        heap.deallocate(p, bytes);
    }
    CHECK_EQ(heap.stats().largeBytes, 0u);
}

TEST(classSizesMatchTheTable) {
    CHECK_EQ(mem::LocalHeap::usableSize(1), 16u);
    CHECK_EQ(mem::LocalHeap::usableSize(16), 16u);
    CHECK_EQ(mem::LocalHeap::usableSize(17), 32u);
    CHECK_EQ(mem::LocalHeap::usableSize(512), 512u);
    CHECK_EQ(mem::LocalHeap::usableSize(513), 640u);
    CHECK_EQ(mem::LocalHeap::usableSize(1024), 1024u);
    CHECK_EQ(mem::LocalHeap::usableSize(1025), 1280u);
    CHECK_EQ(mem::LocalHeap::usableSize(8192), 8192u);
}

TEST(randomChurnKeepsEveryObjectIntact) {
    mem::LocalHeap heap;
    std::mt19937_64 random(99);
    std::vector<Live> live;
    for (int step = 0; step < 400000; ++step) {
        if (live.size() < 2000 && (live.empty() || random() % 100 < 55)) {
            std::size_t bytes = random() % 100 < 98 ? 1 + random() % 600 : 1 + random() % 40000;
            auto fill = static_cast<unsigned char>(random());
            auto* p = static_cast<unsigned char*>(heap.allocate(bytes));
            std::memset(p, fill, bytes);
            live.push_back({p, bytes, fill});
        } else {
            const std::size_t i = random() % live.size();
            const Live object = live[i];
            for (std::size_t k = 0; k < object.bytes; k += 64)
                CHECK_EQ(object.p[k], object.fill);
            CHECK_EQ(object.p[object.bytes - 1], object.fill);
            heap.deallocate(object.p, object.bytes);
            live[i] = live.back();
            live.pop_back();
        }
        if (step % 50000 == 0)
            heap.trim();
    }
    for (const Live& object : live)
        heap.deallocate(object.p, object.bytes);
    heap.trim();
    const auto s = heap.stats();
    CHECK_EQ(s.blocksInUse, 0u);
    CHECK_EQ(s.largeBytes, 0u);
}

TEST(aDrainedBlockIsTakenByAnotherClass) {
    mem::LocalHeap heap;
    constexpr std::size_t kCount = 200000;
    std::vector<void*> small;
    for (std::size_t i = 0; i < kCount; ++i)
        small.push_back(heap.allocate(32));
    const auto full = heap.stats();
    for (void* p : small)
        heap.deallocate(p, 32);
    CHECK(heap.stats().blocksInUse <= 1);

    // Another size: the memory is reused, no new segments.
    std::vector<void*> other;
    const std::size_t count = kCount / 4;
    for (std::size_t i = 0; i < count; ++i)
        other.push_back(heap.allocate(128));
    CHECK(heap.stats().segments <= full.segments);
    for (void* p : other)
        heap.deallocate(p, 128);
}

TEST(survivorsScatteredThroughBlocksDoNotKeepOtherClassesOut) {
    // The classic way a heap shreds: a burst, of which a little survives. The survivors pin their blocks, but those
    // blocks refill first, so the next burst of the same size does not grow the heap.
    mem::LocalHeap heap;
    std::vector<void*> survivors;
    std::uint64_t committedAfterFirst = 0;
    for (int round = 0; round < 50; ++round) {
        std::vector<void*> burst;
        for (int i = 0; i < 50000; ++i)
            burst.push_back(heap.allocate(64));
        for (std::size_t i = 0; i < burst.size(); ++i) {
            if (i % 500 == 0 && survivors.size() < 100)
                survivors.push_back(burst[i]);
            else
                heap.deallocate(burst[i], 64);
        }
        heap.trim();
        if (round == 0)
            committedAfterFirst = heap.stats().committedBytes;
    }
    CHECK(heap.stats().committedBytes <= committedAfterFirst + 8 * kBlock);
    for (void* p : survivors)
        heap.deallocate(p, 64);
}

TEST(trimGivesASpikeBack) {
    if (underSanitizer())
        return;  // shadow memory and quarantine, not the heap, decide the resident size there
    mem::LocalHeap heap;
    const std::size_t idle = sys::residentBytes();
    constexpr std::size_t kBytes = std::size_t{512} << 20;
    std::vector<void*> objects;
    objects.reserve(kBytes / 256);
    const auto heapBaseline = heap.stats();
    for (std::size_t i = 0; i < kBytes / 256; ++i)
        std::memset(objects.emplace_back(heap.allocate(256)), 1, 256);
    CHECK(sys::residentBytes() > idle + kBytes / 2);
    const std::uint64_t peakMapped = heap.stats().mappedBytes;
    for (void* p : objects)
        heap.deallocate(p, 256);
    objects.clear();
    objects.shrink_to_fit();
    heap.trim();
    const auto s = heap.stats();
    CHECK(s.mappedBytes < peakMapped / 8);
    CHECK(s.committedBytes <= heapBaseline.committedBytes + 32 * kBlock);
    CHECK(sys::residentBytes() < idle + (std::size_t{32} << 20));
}

TEST(aPartlyUsedSegmentKeepsItsLiveBlocksThroughTrim) {
    mem::LocalHeap heap;
    std::vector<unsigned char*> objects;
    for (std::size_t i = 0; i < 10 * mem::LocalHeap::kSegmentBytes / 512; ++i) {
        auto* p = static_cast<unsigned char*>(heap.allocate(512));
        *p = static_cast<unsigned char>(i);
        objects.push_back(p);
    }
    for (std::size_t i = 0; i < objects.size(); ++i)
        if (i % 1024)
            heap.deallocate(objects[i], 512);
    heap.trim();
    for (std::size_t i = 0; i < objects.size(); i += 1024) {
        CHECK_EQ(*objects[i], static_cast<unsigned char>(i));
        heap.deallocate(objects[i], 512);
    }
}

TEST(aSmallLoopTouchesNeitherTheHeapNorTheOs) {
    mem::LocalHeap heap;
    void* warm = heap.allocate(48);
    heap.deallocate(warm, 48);
    const auto before = heap.stats();
    bench::AllocScope scope;
    for (int i = 0; i < 1000000; ++i) {
        void* p = heap.allocate(48);
        heap.deallocate(p, 48);
    }
    CHECK_EQ(scope.calls(), 0u);
    CHECK_EQ(heap.stats().segments, before.segments);
    CHECK_EQ(heap.stats().blocksInUse, before.blocksInUse);
}

TEST(largeRequestsGoToThePoolOrTheOs) {
    mem::LocalHeap heap;
    void* p = heap.allocate(mem::LocalHeap::kMaxSmall + 1);
    std::memset(p, 1, mem::LocalHeap::kMaxSmall + 1);
    CHECK_EQ(heap.stats().largeBytes, mem::LocalHeap::kMaxSmall + 1);
    heap.deallocate(p, mem::LocalHeap::kMaxSmall + 1);
    const std::size_t huge = std::size_t{128} << 20;   // past the default pool's largest class
    void* q = heap.allocate(huge);
    static_cast<char*>(q)[huge - 1] = 1;
    heap.deallocate(q, huge);
    void* z = heap.allocate(0);
    heap.deallocate(z, 0);
    CHECK_EQ(heap.stats().largeBytes, 0u);
}

TEST(forEachSlotSeesWhatIsOutAndNothingElse) {
    mem::LocalHeap heap;
    std::vector<void*> out;
    for (std::size_t i = 0; i < 20000; ++i)
        out.push_back(heap.allocate(16 + i % 300));
    std::vector<void*> kept;
    for (std::size_t i = 0; i < out.size(); ++i) {
        if (i % 3 == 0)
            kept.push_back(out[i]);
        else
            heap.deallocate(out[i], 16 + i % 300);
    }
    void* big = heap.allocate(mem::LocalHeap::kMaxSmall + 1);
    std::vector<void*> seen;
    heap.forEachSlot([](void* list, void* slot) { static_cast<std::vector<void*>*>(list)->push_back(slot); }, &seen);
    std::sort(seen.begin(), seen.end());
    std::sort(kept.begin(), kept.end());
    CHECK(seen == kept);
    heap.deallocate(big, mem::LocalHeap::kMaxSmall + 1);
    for (std::size_t i = 0; i < out.size(); i += 3)
        heap.deallocate(out[i], 16 + i % 300);
}

int main() { return tst::runAll("local_heap"); }

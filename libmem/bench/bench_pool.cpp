// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// acquire/release throughput of SizeClassPool against malloc/free, N threads, a working set of 32 blocks each.
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <vector>

#include "mem/size_class_pool.hpp"

template <class F>
static double run(unsigned threads, F&& body) {
    const auto t0 = std::chrono::steady_clock::now();
    std::vector<std::thread> ts;
    for (unsigned t = 0; t < threads; ++t)
        ts.emplace_back([&, t] { body(t); });
    for (auto& t : ts) t.join();
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
}

int main(int argc, char** argv) {
    const std::size_t size = argc > 1 ? std::strtoul(argv[1], nullptr, 10) : 4096;
    constexpr int kOps = 2'000'000;
    std::printf("size %zu, %d ops/thread\n%8s %14s %14s %14s\n", size, kOps, "threads", "checked Mops/s", "unchecked", "malloc");
    for (unsigned threads : {1u, 2u, 4u, 8u}) {
        mem::PoolOptions fast;
        fast.verifyBlocks = false;
        mem::SizeClassPool unchecked(fast);
        unchecked.prewarm(unchecked.classes().classOf(size), 256);
        const double c = run(threads, [&](unsigned) {
            mem::SizeClassPool::Block held[32];
            for (int i = 0; i < kOps; ++i) {
                auto& slot = held[i & 31];
                if (slot.data) unchecked.release(slot);
                slot = unchecked.tryAcquire(size);
                static_cast<volatile char*>(slot.data)[0] = 1;
            }
            for (auto& s : held) unchecked.release(s);
        });
        mem::SizeClassPool pool;
        pool.prewarm(pool.classes().classOf(size), 256);
        const double a = run(threads, [&](unsigned) {
            mem::SizeClassPool::Block held[32];
            for (int i = 0; i < kOps; ++i) {
                auto& slot = held[i & 31];
                if (slot.data) pool.release(slot);
                slot = pool.tryAcquire(size);
                static_cast<volatile char*>(slot.data)[0] = 1;
            }
            for (auto& s : held) pool.release(s);
        });
        const double b = run(threads, [&](unsigned) {
            void* held[32] = {};
            for (int i = 0; i < kOps; ++i) {
                void*& slot = held[i & 31];
                std::free(slot);
                slot = std::malloc(size);
                static_cast<volatile char*>(slot)[0] = 1;
            }
            for (void* s : held) std::free(s);
        });
        std::printf("%8u %14.1f %14.1f %14.1f\n", threads, threads * kOps / a / 1e6, threads * kOps / c / 1e6, threads * kOps / b / 1e6);
    }
}

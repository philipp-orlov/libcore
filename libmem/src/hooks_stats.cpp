// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

#include <cstdio>
#include <cstdlib>

#include "mem/hooks.hpp"
#include "mem/stats.hpp"

namespace mem {

void reportMisuse(const Hooks& hooks, const char* what) noexcept {
    if (hooks.onMisuse)
        hooks.onMisuse(hooks.ctx, what);
    else
        std::fprintf(stderr, "mem: misuse: %s\n", what);
    if (hooks.abortOnMisuse)
        std::abort();
}

void reportExhausted(const Hooks& hooks, const char* pool, std::size_t bytes) noexcept {
    if (hooks.onExhausted)
        hooks.onExhausted(hooks.ctx, pool, bytes);
}

std::size_t Stats::toJson(char* out, std::size_t cap) const noexcept {
    return static_cast<std::size_t>(std::snprintf(
        out, cap,
        "{\"mappedBytes\":%llu,\"retainedBytes\":%llu,\"blocksInUse\":%llu,\"peakBlocksInUse\":%llu,\"acquires\":%llu,"
        "\"releases\":%llu,\"mapCalls\":%llu,\"unmapCalls\":%llu,\"borrowed\":%llu,\"refused\":%llu,\"misuse\":%llu}",
        (unsigned long long)mappedBytes, (unsigned long long)retainedBytes, (unsigned long long)blocksInUse,
        (unsigned long long)peakBlocksInUse, (unsigned long long)acquires, (unsigned long long)releases,
        (unsigned long long)mapCalls, (unsigned long long)unmapCalls, (unsigned long long)borrowed,
        (unsigned long long)refused, (unsigned long long)misuse));
}

std::size_t Stats::toPrometheus(char* out, std::size_t cap, const char* prefix) const noexcept {
    struct Row { const char* name; const char* type; unsigned long long value; };
    const Row rows[] = {{"mapped_bytes", "gauge", mappedBytes},     {"retained_bytes", "gauge", retainedBytes},
                        {"blocks_in_use", "gauge", blocksInUse},     {"peak_blocks_in_use", "gauge", peakBlocksInUse},
                        {"acquires_total", "counter", acquires},     {"releases_total", "counter", releases},
                        {"map_calls_total", "counter", mapCalls},    {"unmap_calls_total", "counter", unmapCalls},
                        {"borrowed_total", "counter", borrowed},     {"refused_total", "counter", refused},
                        {"misuse_total", "counter", misuse}};
    std::size_t total = 0;
    for (const Row& r : rows) {
        char* dst = (out && total < cap) ? out + total : nullptr;
        const std::size_t room = dst ? cap - total : 0;
        total += static_cast<std::size_t>(std::snprintf(dst, room, "# TYPE %s%s %s\n%s%s %llu\n", prefix, r.name, r.type,
                                                         prefix, r.name, r.value));
    }
    return total;
}

}  // namespace mem

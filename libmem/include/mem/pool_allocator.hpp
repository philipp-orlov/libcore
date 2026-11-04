// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// A standard allocator over a SizeClassPool, for containers whose buffers must stay out of the process heap. It holds
// no state: `Pool` names a function that returns the pool, so the container keeps its size and move semantics. A
// request beyond the pool's largest class is mapped directly, which is what such a request would cost anyway.
//
//   mem::SizeClassPool& payload();
//   template <class T> using Vec = std::vector<T, mem::PoolAllocator<T, &payload>>;
#pragma once

#include <cstddef>
#include <new>
#include <type_traits>

#include "mem/size_class_pool.hpp"
#include "sys/vm.hpp"

namespace mem {

using PoolFn = SizeClassPool& (*)();

template <class T, PoolFn Pool>
class PoolAllocator {
public:
    using value_type = T;
    using is_always_equal = std::true_type;
    using propagate_on_container_move_assignment = std::true_type;

    template <class U>
    struct rebind {
        using other = PoolAllocator<U, Pool>;
    };

    PoolAllocator() noexcept = default;
    template <class U>
    PoolAllocator(const PoolAllocator<U, Pool>&) noexcept {}

    T* allocate(std::size_t n) {
        static_assert(alignof(T) <= 64, "the pool's blocks are 64-byte aligned");
        if (n > ~std::size_t{0} / sizeof(T))
            throw std::bad_alloc();
        const std::size_t bytes = n * sizeof(T);
        SizeClassPool& pool = Pool();
        if (bytes > pool.classes().maxBytes()) {
            const sys::VmRegion region = sys::tryVmMap(bytes);
            if (!region.base)
                throw std::bad_alloc();
            return static_cast<T*>(region.base);
        }
        return static_cast<T*>(pool.acquire(bytes).data);
    }

    void deallocate(T* p, std::size_t n) noexcept {
        const std::size_t bytes = n * sizeof(T);
        SizeClassPool& pool = Pool();
        const ClassTable& classes = pool.classes();
        if (bytes > classes.maxBytes()) {
            sys::vmUnmap(p, sys::roundUpToPage(bytes));
            return;
        }
        pool.release({p, classes.bytesOf(classes.classOf(bytes))});
    }

    friend bool operator==(const PoolAllocator&, const PoolAllocator&) noexcept { return true; }
    friend bool operator!=(const PoolAllocator&, const PoolAllocator&) noexcept { return false; }
};

}  // namespace mem

// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// A move-only callable wrapper that never touches the heap. std::function allocates whenever the callable is
// larger than two pointers (libstdc++) or not trivially copyable; on a queue that runs for months, that is a
// steady stream of small allocations. A callable that does not fit is a compile error, so the cost cannot
// appear unnoticed: capture a pointer to a pooled object instead.
#pragma once

#include <cstddef>
#include <new>
#include <type_traits>
#include <utility>
#include <memory>

namespace base {

template <class Signature, std::size_t Capacity = 48>
class InlineFunction;

template <class R, class... Args, std::size_t Capacity>
class InlineFunction<R(Args...), Capacity> {
public:
    InlineFunction() noexcept = default;
    InlineFunction(std::nullptr_t) noexcept {}

    template <class F, class D = std::decay_t<F>,
              class = std::enable_if_t<!std::is_same_v<D, InlineFunction> && std::is_invocable_r_v<R, D&, Args...>>>
    InlineFunction(F&& f) {  // NOLINT: implicit on purpose, like std::function
        static_assert(sizeof(D) <= Capacity, "callable too large for InlineFunction: capture a pointer to a pooled object");
        static_assert(alignof(D) <= alignof(std::max_align_t), "callable over-aligned for InlineFunction");
        static_assert(std::is_nothrow_move_constructible_v<D>, "InlineFunction needs a nothrow-movable callable");
        ::new (static_cast<void*>(&storage_)) D(std::forward<F>(f));
        ops_ = &opsFor<D>;
    }

    InlineFunction(InlineFunction&& other) noexcept { moveFrom(other); }
    InlineFunction& operator=(InlineFunction&& other) noexcept {
        if (this != &other) {
            reset();
            moveFrom(other);
        }
        return *this;
    }
    InlineFunction& operator=(std::nullptr_t) noexcept {
        reset();
        return *this;
    }
    InlineFunction(const InlineFunction&) = delete;
    InlineFunction& operator=(const InlineFunction&) = delete;
    ~InlineFunction() { reset(); }

    explicit operator bool() const noexcept { return ops_ != nullptr; }

    R operator()(Args... args) { return ops_->call(&storage_, std::forward<Args>(args)...); }

    void reset() noexcept {
        if (ops_) {
            ops_->destroy(&storage_);
            ops_ = nullptr;
        }
    }

private:
    struct Ops {
        R (*call)(void*, Args&&...);
        void (*move)(void* from, void* to) noexcept;
        void (*destroy)(void*) noexcept;
    };

    template <class D>
    static inline const Ops opsFor{
        [](void* p, Args&&... args) -> R { return (*static_cast<D*>(p))(std::forward<Args>(args)...); },
        [](void* from, void* to) noexcept {
            ::new (to) D(std::move(*static_cast<D*>(from)));
            std::destroy_at(static_cast<D*>(from));
        },
        [](void* p) noexcept { std::destroy_at(static_cast<D*>(p)); }};

    void moveFrom(InlineFunction& other) noexcept {
        if (other.ops_) {
            other.ops_->move(&other.storage_, &storage_);
            ops_ = other.ops_;
            other.ops_ = nullptr;
        }
    }

    alignas(std::max_align_t) unsigned char storage_[Capacity];
    const Ops* ops_ = nullptr;
};

}  // namespace base

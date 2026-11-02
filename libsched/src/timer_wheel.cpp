// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

#include "sched/timer_wheel.hpp"

#include <algorithm>

namespace sched {

TimerWheel::TimerWheel(std::size_t capacity, Clock::time_point origin) : origin_(origin) {
    if (!capacity || capacity > std::numeric_limits<std::uint32_t>::max() - 1)
        throw std::invalid_argument("invalid timer capacity");
    slots_.resize(capacity);
    free_.reserve(capacity);
    for (std::size_t r = capacity; r; --r)
        free_.push_back(static_cast<std::uint32_t>(r - 1));
    heads_.fill(kNil);
    tails_.fill(kNil);
}

namespace {
// The tick `now` has reached (floor), as opposed to the tick a timer is due at (ceil): a timer never fires early.
std::uint64_t floorTick(std::chrono::steady_clock::time_point origin, std::chrono::steady_clock::time_point t) noexcept {
    if (t <= origin)
        return 0;
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(t - origin).count());
}
}  // namespace

std::uint64_t TimerWheel::tickOf(Clock::time_point t) const noexcept {
    if (t <= origin_)
        return 0;
    const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(t - origin_).count();
    return static_cast<std::uint64_t>((ns + 999999) / 1000000);   // ceil to the next millisecond tick
}

void TimerWheel::link(std::uint32_t index, std::uint16_t bucket) {
    Slot& s = slots_[index];
    s.bucket = bucket;
    s.next = kNil;
    s.prev = tails_[bucket];
    if (tails_[bucket] != kNil)
        slots_[tails_[bucket]].next = index;
    else
        heads_[bucket] = index;
    tails_[bucket] = index;
    if (bucket == kDueBucket)
        ++dueCount_;
}

void TimerWheel::unlink(std::uint32_t index) {
    Slot& s = slots_[index];
    if (s.prev != kNil) slots_[s.prev].next = s.next; else heads_[s.bucket] = s.next;
    if (s.next != kNil) slots_[s.next].prev = s.prev; else tails_[s.bucket] = s.prev;
    if (s.bucket == kDueBucket)
        --dueCount_;
    s.prev = s.next = kNil;
}

void TimerWheel::place(std::uint32_t index) {
    Slot& s = slots_[index];
    if (s.tick <= now_) {
        link(index, kDueBucket);
        return;
    }
    const std::uint64_t delta = s.tick - now_;
    for (unsigned level = 0; level < kLevels; ++level) {
        const std::uint64_t span = std::uint64_t{1} << (kBits * (level + 1));
        if (delta < span || level == kLevels - 1) {
            // Beyond the top level's reach the entry waits in the slot it will share with a later lap and is
            // cascaded (re-placed) when that slot comes round.
            link(index, static_cast<std::uint16_t>(level * kSlots + ((s.tick >> (kBits * level)) & (kSlots - 1))));
            return;
        }
    }
}

void TimerWheel::advanceTo(std::uint64_t tick) {
    if (tick <= now_)
        return;
    if (tick - now_ > (1u << 20)) {
        // A very long sleep: re-place everything instead of walking millions of ticks.
        std::vector<std::uint32_t> all;
        for (std::uint32_t i = 0; i < slots_.size(); ++i)
            if (slots_[i].active && slots_[i].bucket != kDueBucket) {
                unlink(i);
                all.push_back(i);
            }
        now_ = tick;
        for (std::uint32_t i : all)
            place(i);
        return;
    }
    while (now_ < tick) {
        ++now_;
        // Cascade: when a lower level wraps, the next level's current slot spills down.
        for (unsigned level = 1; level < kLevels; ++level) {
            if ((now_ & ((std::uint64_t{1} << (kBits * level)) - 1)) != 0)
                break;
            const std::uint16_t bucket = static_cast<std::uint16_t>(level * kSlots + ((now_ >> (kBits * level)) & (kSlots - 1)));
            std::uint32_t i = heads_[bucket];
            heads_[bucket] = tails_[bucket] = kNil;
            while (i != kNil) {
                const std::uint32_t next = slots_[i].next;
                slots_[i].prev = slots_[i].next = kNil;
                place(i);
                i = next;
            }
        }
        const std::uint16_t b0 = static_cast<std::uint16_t>(now_ & (kSlots - 1));
        std::uint32_t i = heads_[b0];
        heads_[b0] = tails_[b0] = kNil;
        while (i != kNil) {
            const std::uint32_t next = slots_[i].next;
            slots_[i].prev = slots_[i].next = kNil;
            if (slots_[i].tick <= now_)
                link(i, kDueBucket);
            else
                place(i);
            i = next;
        }
    }
}

std::size_t TimerWheel::find(std::uint64_t id) const noexcept {
    const std::size_t index = static_cast<std::uint32_t>(id) - std::size_t{1};
    if (index >= slots_.size() || !slots_[index].active || slots_[index].generation != (id >> 32))
        return slots_.size();
    return index;
}

std::uint64_t TimerWheel::tryAdd(Clock::time_point when, Callback callback) {
    if (!callback)
        return 0;
    std::lock_guard<std::mutex> lock(mutex_);
    if (free_.empty())
        return 0;
    const std::uint32_t index = free_.back();
    free_.pop_back();
    Slot& s = slots_[index];
    if (++s.generation == 0)
        ++s.generation;
    s.active = true;
    s.tick = tickOf(when);
    s.callback = std::move(callback);
    place(index);
    armed_.store(slots_.size() - free_.size());
    return (static_cast<std::uint64_t>(s.generation) << 32) | (index + 1);
}

bool TimerWheel::refresh(std::uint64_t id, Clock::time_point when) {
    std::lock_guard<std::mutex> lock(mutex_);
    const std::size_t index = find(id);
    if (index == slots_.size())
        return false;
    unlink(static_cast<std::uint32_t>(index));
    slots_[index].tick = tickOf(when);
    place(static_cast<std::uint32_t>(index));
    return true;
}

void TimerWheel::cancel(std::uint64_t id) {
    Callback retired;
    std::lock_guard<std::mutex> lock(mutex_);
    const std::size_t index = find(id);
    if (index == slots_.size())
        return;
    unlink(static_cast<std::uint32_t>(index));
    Slot& s = slots_[index];
    s.active = false;
    retired = std::move(s.callback);
    free_.push_back(static_cast<std::uint32_t>(index));
    armed_.store(slots_.size() - free_.size());
}

bool TimerWheel::popDue(Clock::time_point now, Callback& callback) {
    Callback retired;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        advanceTo(floorTick(origin_, now));
        const std::uint32_t i = heads_[kDueBucket];
        if (i == kNil)
            return false;
        unlink(i);
        Slot& s = slots_[i];
        s.active = false;
        retired = std::move(s.callback);
        free_.push_back(i);
        armed_.store(slots_.size() - free_.size());
    }
    callback = std::move(retired);
    return true;
}

int TimerWheel::waitMilliseconds(Clock::time_point now) const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (slots_.size() == free_.size())
        return -1;
    if (dueCount_)
        return 0;
    // Milliseconds from `now` until the earliest tick that has anything in it; a conservative (early) answer is fine,
    // the loop just wakes and asks again.
    const std::uint64_t cur = std::max(now_, floorTick(origin_, now));
    // Entries in the upper levels move down at the next multiple of 256 ticks, so never sleep past that.
    const std::uint64_t boundary = (now_ | (kSlots - 1)) + 1;
    std::uint64_t target = boundary;
    for (unsigned k = 1; k <= kSlots; ++k) {
        const std::uint64_t t = now_ + k;
        if (t >= target)
            break;
        if (heads_[t & (kSlots - 1)] != kNil) {
            target = t;
            break;
        }
    }
    return target <= cur ? 0 : static_cast<int>(std::min<std::uint64_t>(target - cur, std::numeric_limits<int>::max()));
}

}  // namespace sched

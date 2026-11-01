// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

#include "lg/logger.hpp"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <new>

#include "base/platform.hpp"
#include "mem/size_class_pool.hpp"
#include "sys/proc.hpp"
#include "sys/wait.hpp"

namespace lg {

namespace {
std::atomic<std::uint64_t> g_nextGen{1};
std::mutex& registryMutex() {
    static std::mutex* m = new std::mutex;   // leaked: thread-exit hooks may run during static destruction
    return *m;
}
std::vector<std::uint64_t>& liveGens() {
    static auto* v = new std::vector<std::uint64_t>;
    return *v;
}
bool isLive(std::uint64_t gen) {   // registryMutex held
    const auto& v = liveGens();
    return std::find(v.begin(), v.end(), gen) != v.end();
}
}  // namespace

const char* levelName(Level l) noexcept {
    switch (l) {
        case Level::Trace: return "TRACE";
        case Level::Debug: return "DEBUG";
        case Level::Info: return "INFO";
        case Level::Warn: return "WARN";
        case Level::Error: return "ERROR";
        default: return "OFF";
    }
}

Level parseLevel(const char* t, Level fallback) noexcept {
    if (!t)
        return fallback;
    auto eq = [&](const char* name) {
        const char* a = t;
        const char* b = name;
        for (; *a && *b; ++a, ++b)
            if ((*a | 0x20) != *b)
                return false;
        return !*a && !*b;
    };
    if (eq("trace")) return Level::Trace;
    if (eq("debug")) return Level::Debug;
    if (eq("info")) return Level::Info;
    if (eq("warn") || eq("warning")) return Level::Warn;
    if (eq("error")) return Level::Error;
    if (eq("off") || eq("none")) return Level::Off;
    return fallback;
}

struct Logger::Ring {
    struct Slot {
        std::uint64_t wallNs;
        std::uint16_t length;
        std::uint8_t level;
        std::uint8_t pad[5];
        char text[1];
    };
    alignas(base::kCacheLine) std::atomic<std::uint64_t> head{0};
    alignas(base::kCacheLine) std::atomic<std::uint64_t> tail{0};
    std::uint64_t cachedTail = 0;               // producer's last view of tail
    std::atomic<bool> retired{false};
    mem::SizeClassPool::Block block;
    std::size_t slotBytes = 0, mask = 0;
    char* slots = nullptr;
    Slot* slot(std::uint64_t i) const noexcept { return reinterpret_cast<Slot*>(slots + (i & mask) * slotBytes); }
};

namespace {
struct ThreadEntry {
    Logger* logger = nullptr;
    std::uint64_t gen = 0;
    void* ring = nullptr;
};
struct ThreadRings {
    ThreadEntry entries[4];
    unsigned next = 0;
    ~ThreadRings();
};
thread_local ThreadRings t_rings;
}  // namespace

// Gives the thread-exit hook access to the private retireRing().
struct RingHandle {
    static void retire(Logger* l, void* ring) { l->retireRing(static_cast<Logger::Ring*>(ring)); }
};

namespace {
ThreadRings::~ThreadRings() {
    std::lock_guard<std::mutex> g(registryMutex());
    for (ThreadEntry& e : entries)
        if (e.logger && isLive(e.gen))
            RingHandle::retire(e.logger, e.ring);
}
}  // namespace

Logger::Logger(LoggerOptions options)
    : options_([&] {
          LoggerOptions o = options;
          o.slotBytes = std::min<std::size_t>(1024, std::max<std::size_t>(128, (o.slotBytes + 15) & ~std::size_t{15}));
          std::size_t s = 4;
          while (s < o.ringSlots) s <<= 1;
          o.ringSlots = s;
          return o;
      }()),
      gen_(g_nextGen.fetch_add(1, std::memory_order_relaxed)),
      level_(static_cast<std::uint8_t>(options.level)) {
    {
        std::lock_guard<std::mutex> g(registryMutex());
        liveGens().push_back(gen_);
    }
    consumer_ = std::thread([this] { consumerLoop(); });
}

Logger::~Logger() {
    shutdown();
    {
        std::lock_guard<std::mutex> g(registryMutex());
        auto& v = liveGens();
        v.erase(std::remove(v.begin(), v.end(), gen_), v.end());
    }
    std::lock_guard<std::mutex> g(ringsMu_);
    for (Ring* r : rings_) {
        mem::SizeClassPool::global().release(r->block);
    }
    rings_.clear();
}

void Logger::addSink(std::unique_ptr<Sink> sink) {
    std::lock_guard<std::mutex> g(sinksMu_);
    sinks_.push_back(std::move(sink));
}
void Logger::clearSinks() {
    std::lock_guard<std::mutex> g(sinksMu_);
    sinks_.clear();
}

Logger::Ring* Logger::ringForThisThread() noexcept {
    for (ThreadEntry& e : t_rings.entries)
        if (e.gen == gen_ && e.logger == this)
            return static_cast<Ring*>(e.ring);
    // First line from this thread (or this logger): create a ring.
    const std::size_t bytes = 256 + options_.ringSlots * options_.slotBytes;
    mem::SizeClassPool::Block block = mem::SizeClassPool::global().tryAcquire(bytes);
    if (!block.data)
        return nullptr;
    auto* ring = new (block.data) Ring();
    ring->block = block;
    ring->slotBytes = options_.slotBytes;
    ring->mask = options_.ringSlots - 1;
    ring->slots = static_cast<char*>(block.data) + 256;
    {
        std::lock_guard<std::mutex> g(ringsMu_);
        try {
            rings_.push_back(ring);
        } catch (...) {
            mem::SizeClassPool::global().release(block);
            return nullptr;
        }
    }
    ThreadEntry& slot = t_rings.entries[t_rings.next++ % 4];
    if (slot.logger) {
        std::lock_guard<std::mutex> g(registryMutex());
        if (isLive(slot.gen))
            RingHandle::retire(slot.logger, slot.ring);   // evicted entry: its ring is finished
    }
    slot.logger = this;
    slot.gen = gen_;
    slot.ring = ring;
    return ring;
}

void Logger::retireRing(Ring* ring) noexcept {
    ring->retired.store(true, std::memory_order_release);
    wakeConsumer();
}

void Logger::wakeConsumer() noexcept {
    wakeWord_.fetch_add(1, std::memory_order_release);
    sys::wakeOne(wakeWord_);
}

// Called by a producer after it publishes: when its ring is at least half full the consumer is woken (at most once
// per consumer cycle, so a burst costs a few futex calls, not one per line).
#define LG_NOTIFY_IF_FILLING(ring, head)                                                                  \
    do {                                                                                                  \
        if ((head) + 1 - (ring)->cachedTail >= (ring)->mask / 2 && !wakePending_.exchange(true, std::memory_order_acq_rel)) \
            wakeConsumer();                                                                               \
    } while (0)

void Logger::writeRaw(Level level, std::string_view message) noexcept {
    if (stopping_.load(std::memory_order_relaxed) || !enabled(level)) {
        if (stopping_.load(std::memory_order_relaxed))
            dropped_.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    Ring* r = ringForThisThread();
    if (!r) {
        dropped_.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    const std::uint64_t head = r->head.load(std::memory_order_relaxed);
    if (head - r->cachedTail >= r->mask + 1) {
        r->cachedTail = r->tail.load(std::memory_order_acquire);
        if (head - r->cachedTail >= r->mask + 1) {
            dropped_.fetch_add(1, std::memory_order_relaxed);
            return;
        }
    }
    Ring::Slot* s = r->slot(head);
    const std::size_t cap = r->slotBytes - offsetof(Ring::Slot, text);
    std::size_t n = message.size();
    if (n > cap) {
        n = cap;
        truncated_.fetch_add(1, std::memory_order_relaxed);
        std::memcpy(s->text, message.data(), n - 3);
        std::memcpy(s->text + n - 3, "...", 3);
    } else {
        std::memcpy(s->text, message.data(), n);
    }
    s->length = static_cast<std::uint16_t>(n);
    s->level = static_cast<std::uint8_t>(level);
    s->wallNs = sys::wallNs();
    r->head.store(head + 1, std::memory_order_release);
    LG_NOTIFY_IF_FILLING(r, head);
}

void Logger::writeV(Level level, const char* fmt, va_list args) noexcept {
    if (stopping_.load(std::memory_order_relaxed) || !enabled(level)) {
        if (stopping_.load(std::memory_order_relaxed))
            dropped_.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    Ring* r = ringForThisThread();
    if (!r) {
        dropped_.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    const std::uint64_t head = r->head.load(std::memory_order_relaxed);
    if (head - r->cachedTail >= r->mask + 1) {
        r->cachedTail = r->tail.load(std::memory_order_acquire);
        if (head - r->cachedTail >= r->mask + 1) {
            dropped_.fetch_add(1, std::memory_order_relaxed);
            return;
        }
    }
    Ring::Slot* s = r->slot(head);
    const std::size_t cap = r->slotBytes - offsetof(Ring::Slot, text);
    const int n = std::vsnprintf(s->text, cap, fmt, args);   // formats straight into the slot
    std::size_t len;
    if (n < 0) {
        len = 0;
    } else if (static_cast<std::size_t>(n) >= cap) {
        len = cap - 1;
        std::memcpy(s->text + len - 3, "...", 3);
        truncated_.fetch_add(1, std::memory_order_relaxed);
    } else {
        len = static_cast<std::size_t>(n);
    }
    s->length = static_cast<std::uint16_t>(len);
    s->level = static_cast<std::uint8_t>(level);
    s->wallNs = sys::wallNs();
    r->head.store(head + 1, std::memory_order_release);
    LG_NOTIFY_IF_FILLING(r, head);
}

void Logger::write(Level level, const char* fmt, ...) noexcept {
    va_list args;
    va_start(args, fmt);
    writeV(level, fmt, args);
    va_end(args);
}

std::uint64_t Logger::producedLines() const noexcept {
    std::lock_guard<std::mutex> g(ringsMu_);
    std::uint64_t total = retiredProduced_;
    for (const Ring* r : rings_)
        total += r->head.load(std::memory_order_acquire);
    return total;
}

// ---- consumer ----

namespace {
struct TimePrefix {
    std::time_t second = -1;
    char text[24] = {};   // "YYYY-MM-DD HH:MM:SS"
    void update(std::time_t sec, bool utc) {
        if (sec == second)
            return;
        second = sec;
        std::tm tm{};
        if (utc) gmtime_r(&sec, &tm); else localtime_r(&sec, &tm);
        std::snprintf(text, sizeof text, "%04d-%02d-%02d %02d:%02d:%02d", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                      tm.tm_hour, tm.tm_min, tm.tm_sec);
    }
};

// Appends one escaped line to out; returns bytes written. `room` must be >= 4 * length + 64.
std::size_t composeLine(char* out, TimePrefix& prefix, const Logger&, bool timestamps, bool utc, std::uint64_t wallNs,
                        Level level, const char* text, std::size_t length) {
    char* o = out;
    if (timestamps) {
        prefix.update(static_cast<std::time_t>(wallNs / 1000000000ull), utc);
        std::memcpy(o, prefix.text, 19);
        o += 19;
        const unsigned ms = static_cast<unsigned>((wallNs / 1000000ull) % 1000);
        *o++ = '.';
        *o++ = static_cast<char>('0' + ms / 100);
        *o++ = static_cast<char>('0' + ms / 10 % 10);
        *o++ = static_cast<char>('0' + ms % 10);
        *o++ = ' ';
    }
    static const char* const kNames[] = {"[TRACE] ", "[DEBUG] ", "[INFO ] ", "[WARN ] ", "[ERROR] "};
    const char* name = kNames[std::min<unsigned>(static_cast<unsigned>(level), 4)];
    std::memcpy(o, name, 8);
    o += 8;
    static const char kHex[] = "0123456789abcdef";
    for (std::size_t i = 0; i < length; ++i) {
        const unsigned char c = static_cast<unsigned char>(text[i]);
        if (c >= 0x20 && c != 0x7f) {
            *o++ = static_cast<char>(c);
        } else if (c == '\n') { *o++ = '\\'; *o++ = 'n'; }
        else if (c == '\r') { *o++ = '\\'; *o++ = 'r'; }
        else if (c == '\t') { *o++ = '\\'; *o++ = 't'; }
        else { *o++ = '\\'; *o++ = 'x'; *o++ = kHex[c >> 4]; *o++ = kHex[c & 15]; }
    }
    *o++ = '\n';
    return static_cast<std::size_t>(o - out);
}
}  // namespace

bool Logger::drainOnce(bool all) {
    static thread_local std::vector<char> batch;       // consumer thread only; reserved once, never regrown
    static thread_local TimePrefix prefix;
    if (batch.capacity() < 64 * 1024)
        batch.reserve(64 * 1024);
    batch.resize(0);
    static thread_local std::vector<Ring*> snapshot;   // consumer thread only; capacity is reused
    {
        std::lock_guard<std::mutex> g(ringsMu_);
        snapshot.assign(rings_.begin(), rings_.end());   // rings are only freed by this thread
    }
    const std::size_t maxLine = 4 * options_.slotBytes + 64;
    std::uint64_t consumedNow = 0;
    Level highest = Level::Trace;
    bool any = false;

    auto flushBatch = [&] {
        if (batch.empty())
            return;
        std::lock_guard<std::mutex> g(sinksMu_);
        for (auto& sink : sinks_) {
            if (!sink->perLine()) {
                sink->write(highest, batch.data(), batch.size());
            } else {
                const char* p = batch.data();
                const char* end = p + batch.size();
                while (p < end) {
                    const char* nl = static_cast<const char*>(std::memchr(p, '\n', static_cast<std::size_t>(end - p)));
                    const std::size_t len = nl ? static_cast<std::size_t>(nl - p) + 1 : static_cast<std::size_t>(end - p);
                    sink->write(highest, p, len);
                    p += len;
                }
            }
        }
        batch.resize(0);
        highest = Level::Trace;
    };

    for (std::size_t round = 0; round < (all ? ~std::size_t{0} : 4096); ++round) {
        Ring* best = nullptr;
        std::uint64_t bestTs = ~std::uint64_t{0};
        for (Ring* r : snapshot) {
            const std::uint64_t t = r->tail.load(std::memory_order_relaxed);
            if (t == r->head.load(std::memory_order_acquire))
                continue;
            const std::uint64_t ts = r->slot(t)->wallNs;
            if (ts < bestTs) { bestTs = ts; best = r; }
        }
        if (!best)
            break;
        any = true;
        if (batch.capacity() - batch.size() < maxLine)
            flushBatch();
        const std::uint64_t t = best->tail.load(std::memory_order_relaxed);
        const Ring::Slot* s = best->slot(t);
        const std::size_t old = batch.size();
        batch.resize(old + maxLine);   // within capacity: no allocation
        const std::size_t n = composeLine(batch.data() + old, prefix, *this, options_.timestamps, options_.utc, s->wallNs,
                                          static_cast<Level>(s->level), s->text, s->length);
        batch.resize(old + n);
        highest = std::max(highest, static_cast<Level>(s->level));
        best->tail.store(t + 1, std::memory_order_release);
        ++consumedNow;
    }
    flushBatch();

    // Free rings whose thread is gone and which are empty.
    {
        std::lock_guard<std::mutex> g(ringsMu_);
        for (std::size_t i = 0; i < rings_.size();) {
            Ring* r = rings_[i];
            if (r->retired.load(std::memory_order_acquire) && r->tail.load() == r->head.load(std::memory_order_acquire)) {
                retiredProduced_ += r->head.load();
                mem::SizeClassPool::global().release(r->block);
                rings_[i] = rings_.back();
                rings_.pop_back();
            } else {
                ++i;
            }
        }
    }
    if (consumedNow) {
        consumed_.fetch_add(consumedNow, std::memory_order_release);
        progressWord_.fetch_add(1, std::memory_order_release);
        sys::wakeAll(progressWord_);
    }
    return any;
}

void Logger::consumerLoop() {
    auto lastDropReport = std::chrono::steady_clock::now();
    std::uint64_t reportedDrops = 0;
    for (;;) {
        const std::uint32_t seen = wakeWord_.load(std::memory_order_acquire);
        wakePending_.store(false, std::memory_order_release);
        const bool any = drainOnce(false);
        const auto now = std::chrono::steady_clock::now();
        if (now - lastDropReport >= std::chrono::seconds(1)) {
            lastDropReport = now;
            const std::uint64_t d = dropped_.load(std::memory_order_relaxed);
            if (d != reportedDrops) {
                char line[96];
                const int n = std::snprintf(line, sizeof line, "lg: %llu log lines dropped (ring full or logger stopping)\n",
                                            static_cast<unsigned long long>(d - reportedDrops));
                reportedDrops = d;
                std::lock_guard<std::mutex> g(sinksMu_);
                for (auto& sink : sinks_)
                    sink->write(Level::Warn, line, static_cast<std::size_t>(n));
            }
        }
        if (stopping_.load(std::memory_order_acquire)) {
            drainOnce(true);   // drain every ring to empty, then leave
            std::lock_guard<std::mutex> g(sinksMu_);
            for (auto& sink : sinks_)
                sink->flush();
            progressWord_.fetch_add(1, std::memory_order_release);
            sys::wakeAll(progressWord_);
            return;
        }
        if (!any)
            sys::waitOnAddress(wakeWord_, seen, options_.idlePoll);
    }
}

void Logger::flush() {
    if (!consumer_.joinable()) {
        return;
    }
    const std::uint64_t target = producedLines();
    while (consumed_.load(std::memory_order_acquire) < target) {
        const std::uint32_t p = progressWord_.load(std::memory_order_acquire);
        if (consumed_.load(std::memory_order_acquire) >= target || !consumer_.joinable())
            break;
        wakeConsumer();
        sys::waitOnAddress(progressWord_, p, std::chrono::milliseconds(10));
        if (stopping_.load(std::memory_order_acquire) && consumed_.load() >= target)
            break;
    }
    std::lock_guard<std::mutex> g(sinksMu_);
    for (auto& sink : sinks_)
        sink->flush();
}

void Logger::reopenSinks() {
    std::lock_guard<std::mutex> g(sinksMu_);
    for (auto& sink : sinks_)
        sink->reopen();
}

void Logger::shutdown() noexcept {
    std::lock_guard<std::mutex> g(shutdownMu_);
    if (!consumer_.joinable())
        return;
    stopping_.store(true, std::memory_order_release);
    wakeConsumer();
    consumer_.join();
}

Logger& Logger::instance() {
    static Logger* logger = [] {
        LoggerOptions o;
        o.level = parseLevel(std::getenv("LG_LEVEL"), Level::Info);
        auto* l = new Logger(o);
        if (const char* file = std::getenv("LG_FILE")) {
            FileSinkOptions fo;
            fo.path = file;
            std::string error;
            if (auto sink = makeFileSink(fo, &error))
                l->addSink(std::move(sink));
            else
                l->addSink(makeConsoleSink());
        } else {
            l->addSink(makeConsoleSink());
        }
        return l;   // intentionally never destroyed: threads may log during static destruction
    }();
    return *logger;
}

}  // namespace lg

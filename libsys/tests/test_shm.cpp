// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cstring>

#include "base/status.hpp"
#include "mini_test.hpp"
#include "sys/shm.hpp"
#include "sys/wait.hpp"

TEST(createdMemoryIsZeroSealedAndSharedWithAChild) {
    sys::SharedMemory m = sys::SharedMemory::create("test", 8192);
    CHECK_EQ(m.size(), 8192u);
    for (std::size_t i = 0; i < m.size(); i += 512) CHECK_EQ(m.data()[i], 0);
    // Sealed: the file cannot be shrunk or grown.
    CHECK(::ftruncate(m.fd(), 4096) != 0);
    CHECK(::ftruncate(m.fd(), 1 << 20) != 0);
    // Another view of the same file sees our writes (what adopt() hands to a peer process).
    sys::Fd dup(::dup(m.fd()));
    sys::SharedMemory peer = sys::SharedMemory::adopt(std::move(dup));
    CHECK_EQ(peer.size(), 8192u);
    std::memcpy(m.data(), "hello", 6);
    CHECK_EQ(std::memcmp(peer.data(), "hello", 6), 0);

    // And across a real process boundary, with a futex word in the memory.
    auto* word = reinterpret_cast<std::atomic<std::uint32_t>*>(m.data() + 64);
    const pid_t pid = ::fork();
    if (pid == 0) {
        while (word->load() == 0) sys::waitOnAddress(*word, 0);
        m.data()[100] = 42;
        ::_exit(0);
    }
    word->store(1);
    sys::wakeAll(*word);
    int status = 0;
    ::waitpid(pid, &status, 0);
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    CHECK_EQ(m.data()[100], 42);
}

TEST(adoptRefusesWhatIsNotSafeToMap) {
    auto code = [](auto&& body) {
        try { body(); } catch (const base::Error& e) { return e.code(); }
        return base::Code::Ok;
    };
    // An ordinary unsealed memfd.
    sys::Fd plain(::memfd_create("plain", MFD_CLOEXEC | MFD_ALLOW_SEALING));
    CHECK_EQ(::ftruncate(plain.get(), 4096), 0);
    CHECK(code([&] { sys::SharedMemory::adopt(std::move(plain)); }) == base::Code::FailedPrecondition);
    // No descriptor.
    CHECK(code([] { sys::SharedMemory::adopt(sys::Fd()); }) == base::Code::InvalidArgument);
    // Too large for the caller's limit.
    sys::SharedMemory big = sys::SharedMemory::create("big", 1 << 20);
    sys::Fd d(::dup(big.fd()));
    CHECK(code([&] { sys::SharedMemory::adopt(std::move(d), 4096); }) == base::Code::OutOfRange);
    CHECK(code([] { sys::SharedMemory::create("zero", 0); }) == base::Code::InvalidArgument);
}

int main() { return tst::runAll("shm"); }

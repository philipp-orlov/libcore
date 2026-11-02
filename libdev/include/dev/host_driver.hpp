// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "dev/driver.hpp"

namespace dev {

// Simulated devices on host memory. Work is "asynchronous" in one respect only: an event recorded on a stream
// completes when that stream is synchronized or when the test calls complete(stream), which is what makes deferred
// frees and event polling testable.
class HostDriver : public Driver {
public:
    virtual void complete(StreamHandle stream) = 0;        // everything recorded so far on `stream` has finished
    virtual std::size_t usedBytes(int device) = 0;
    virtual std::size_t liveAllocations() = 0;
    // Calls that broke a runtime rule: freeing memory on the wrong device, a stream or event of another device,
    // destroying something twice. A correct client leaves this at 0.
    virtual std::uint64_t misuse() = 0;
};

std::unique_ptr<HostDriver> makeHostDriverTyped(int devices, std::size_t bytesPerDevice);

}  // namespace dev

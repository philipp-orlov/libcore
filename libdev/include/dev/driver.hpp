// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// The thin device-runtime interface everything else in libdev is written against. Two implementations:
//   * HostDriver: simulated devices on host memory (any number of them), used by the tests and on machines without a
//     GPU. It enforces the rules a real runtime enforces -- memory belongs to a device, a stream belongs to a device,
//     the current device is per thread -- so a bug that only shows on a second GPU shows here too.
//   * CudaDriver: the CUDA runtime loaded with dlopen at run time (libcudart.so), so libdev builds without the CUDA
//     toolkit and a binary built on a build machine runs on whatever machine has (or lacks) a driver.
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

namespace dev {

using StreamHandle = void*;
using EventHandle = void*;

class Driver {
public:
    virtual ~Driver() = default;
    virtual const char* name() const noexcept = 0;
    virtual int deviceCount() = 0;
    virtual bool setDevice(int device) = 0;      // per-thread current device
    virtual int currentDevice() = 0;
    virtual void* malloc(std::size_t bytes) = 0;   // on the current device; nullptr on failure
    virtual void free(void* p) = 0;
    virtual bool memInfo(std::size_t* freeBytes, std::size_t* totalBytes) = 0;   // of the current device
    virtual bool hostRegister(void* p, std::size_t bytes) = 0;
    virtual bool hostUnregister(void* p) = 0;
    virtual StreamHandle streamCreate() = 0;      // on the current device
    virtual void streamDestroy(StreamHandle) = 0;
    virtual bool streamSynchronize(StreamHandle) = 0;
    virtual EventHandle eventCreate() = 0;
    virtual bool eventRecord(EventHandle, StreamHandle) = 0;
    virtual bool eventQuery(EventHandle) = 0;     // true = completed
    virtual void eventDestroy(EventHandle) = 0;
    virtual bool canAccessPeer(int device, int peer) = 0;
    virtual bool enablePeerAccess(int peer) = 0;  // from the current device
};

// `devices` simulated devices of `bytesPerDevice` each (0 = unlimited).
std::unique_ptr<Driver> makeHostDriver(int devices = 1, std::size_t bytesPerDevice = 0);

// dlopen("libcudart.so[.N]"); nullptr (with the reason in *error) if the runtime or a symbol is missing or reports no
// devices.
std::unique_ptr<Driver> makeCudaDriver(std::string* error = nullptr);

}  // namespace dev

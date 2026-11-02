// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

#include <dlfcn.h>

#include <type_traits>
#include <string>

#include "dev/driver.hpp"

namespace dev {

namespace {

// cudaError_t is an int; 0 is success. The signatures below are the stable C ABI of libcudart.
using Err = int;
struct Table {
    Err (*getDeviceCount)(int*);
    Err (*setDevice)(int);
    Err (*getDevice)(int*);
    Err (*malloc_)(void**, std::size_t);
    Err (*free_)(void*);
    Err (*memGetInfo)(std::size_t*, std::size_t*);
    Err (*hostRegister)(void*, std::size_t, unsigned);
    Err (*hostUnregister)(void*);
    Err (*streamCreate)(void**);
    Err (*streamDestroy)(void*);
    Err (*streamSynchronize)(void*);
    Err (*eventCreateWithFlags)(void**, unsigned);
    Err (*eventRecord)(void*, void*);
    Err (*eventQuery)(void*);
    Err (*eventDestroy)(void*);
    Err (*deviceCanAccessPeer)(int*, int, int);
    Err (*deviceEnablePeerAccess)(int, unsigned);
};

class Cuda final : public Driver {
public:
    explicit Cuda(void* lib) : lib_(lib) {}
    ~Cuda() override { if (lib_) ::dlclose(lib_); }
    bool load(std::string* error) {
        auto sym = [&](const char* name, auto& slot) {
            slot = reinterpret_cast<std::remove_reference_t<decltype(slot)>>(::dlsym(lib_, name));
            if (!slot && error) *error = std::string("libcudart is missing ") + name;
            return slot != nullptr;
        };
        return sym("cudaGetDeviceCount", t.getDeviceCount) && sym("cudaSetDevice", t.setDevice) && sym("cudaGetDevice", t.getDevice) &&
               sym("cudaMalloc", t.malloc_) && sym("cudaFree", t.free_) && sym("cudaMemGetInfo", t.memGetInfo) &&
               sym("cudaHostRegister", t.hostRegister) && sym("cudaHostUnregister", t.hostUnregister) &&
               sym("cudaStreamCreate", t.streamCreate) && sym("cudaStreamDestroy", t.streamDestroy) &&
               sym("cudaStreamSynchronize", t.streamSynchronize) && sym("cudaEventCreateWithFlags", t.eventCreateWithFlags) &&
               sym("cudaEventRecord", t.eventRecord) && sym("cudaEventQuery", t.eventQuery) && sym("cudaEventDestroy", t.eventDestroy) &&
               sym("cudaDeviceCanAccessPeer", t.deviceCanAccessPeer) && sym("cudaDeviceEnablePeerAccess", t.deviceEnablePeerAccess);
    }
    const char* name() const noexcept override { return "cuda"; }
    int deviceCount() override { int n = 0; return t.getDeviceCount(&n) == 0 ? n : 0; }
    bool setDevice(int d) override { return t.setDevice(d) == 0; }
    int currentDevice() override { int d = -1; t.getDevice(&d); return d; }
    void* malloc(std::size_t bytes) override { void* p = nullptr; return t.malloc_(&p, bytes) == 0 ? p : nullptr; }
    void free(void* p) override { t.free_(p); }
    bool memInfo(std::size_t* f, std::size_t* total) override { std::size_t a = 0, b = 0; const bool ok = t.memGetInfo(&a, &b) == 0; if (f) *f = a; if (total) *total = b; return ok; }
    bool hostRegister(void* p, std::size_t n) override { return t.hostRegister(p, n, 0 /*cudaHostRegisterDefault*/) == 0; }
    bool hostUnregister(void* p) override { return t.hostUnregister(p) == 0; }
    StreamHandle streamCreate() override { void* s = nullptr; return t.streamCreate(&s) == 0 ? s : nullptr; }
    void streamDestroy(StreamHandle s) override { t.streamDestroy(s); }
    bool streamSynchronize(StreamHandle s) override { return t.streamSynchronize(s) == 0; }
    EventHandle eventCreate() override { void* e = nullptr; return t.eventCreateWithFlags(&e, 2 /*cudaEventDisableTiming*/) == 0 ? e : nullptr; }
    bool eventRecord(EventHandle e, StreamHandle s) override { return t.eventRecord(e, s) == 0; }
    bool eventQuery(EventHandle e) override { return t.eventQuery(e) == 0; }   // cudaErrorNotReady (600) = still running
    void eventDestroy(EventHandle e) override { t.eventDestroy(e); }
    bool canAccessPeer(int d, int p) override { int ok = 0; return t.deviceCanAccessPeer(&ok, d, p) == 0 && ok; }
    bool enablePeerAccess(int p) override { const Err e = t.deviceEnablePeerAccess(p, 0); return e == 0 || e == 704 /*already enabled*/; }

private:
    void* lib_;
    Table t{};
};

}  // namespace

std::unique_ptr<Driver> makeCudaDriver(std::string* error) {
    void* lib = nullptr;
    for (const char* name : {"libcudart.so", "libcudart.so.12", "libcudart.so.11.0"})
        if ((lib = ::dlopen(name, RTLD_NOW | RTLD_LOCAL)))
            break;
    if (!lib) {
        if (error) *error = "libcudart is not installed";
        return nullptr;
    }
    auto driver = std::make_unique<Cuda>(lib);
    if (!driver->load(error))
        return nullptr;
    if (driver->deviceCount() == 0) {
        if (error) *error = "the CUDA runtime reports no devices";
        return nullptr;
    }
    return driver;
}

}  // namespace dev

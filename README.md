# libcore

The shared components of the seven libraries (libsql, librpc, libnosql, libhttp, libbufs, cortex, gscope), kept in one
repository until they are stable enough to split. Each directory is self-contained, depends only on the ones before it
in this list, and builds as C++17 (cortex is C++20 and uses these as they are).

| Component | What it is |
|---|---|
| `buildkit` | CMake modules (warnings, one `DEVELOP_SANITIZE` switch), `scripts/check_no_dups.sh` |
| `libbase` | status/`Result`/`Error`, `Span`/`Slice`, xxh3, FNV-1a, CRC32C (SSE4.2/ARMv8 dispatch), SHA-1, base64/hex, UTF-8, `InlineFunction` |
| `libsys` | OS layer: virtual memory, futex wait, CPU topology, `Process` (no shell), files + mappings + durability, sockets, sealed shared memory |
| `libmem` | `SlabPool`, `SizeClassPool`, `Arena`, `ObjectPool`, `RegisteredPool`, `PageClassAllocator`, `CachingAllocator` |
| `libsched` | `ThreadPool`, `parallelFor`, `TimerHeap`, `TimerWheel`, `TaskQueue` |
| `liblog` | asynchronous logger with per-thread rings and console/file/syslog/callback sinks |
| `libjson` | `Json` value + parser, allocation-free `Writer` |
| `libtar` | hardened ustar/pax/GNU reader and writer, shell-free process source |
| `libschema` | the `.bufs` front end (parser, checker, model) |
| `libcfg` | typed parameters from argv, environment, URL query |
| `libhw` | hardware probe, metrics providers (tegrastats, NVML, nvidia-smi, `/proc`), allocation-light `/proc` samplers |
| `libdev` | device/stream/event/allocator layer over a `Driver` (host simulator, CUDA runtime via `dlopen`) |
| `libnet` | reactor loops (epoll, io_uring, IOCP), `TcpListener`, `TcpConnection` |
| `libbench` | `AllocScope` (counts `operator new`), `runSoak` |

## Building

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build --output-on-failure
```

Sanitizers: `-DDEVELOP_SANITIZE=address,undefined` or `-DDEVELOP_SANITIZE=thread`. A longer soak:
`DEVELOP_SOAK_SECONDS=86400 build/libbench/bench_soak_memory`.

## Using it

As a subdirectory (what the seven repositories do, with a sibling checkout):

```cmake
set(DEVELOP_BUILD_TESTS OFF CACHE BOOL "")
add_subdirectory(../libcore libcore)
target_link_libraries(mylib PRIVATE develop::mem develop::sys)
```

Or installed: `cmake --install build --prefix /opt/develop`, then `find_package(libcore CONFIG REQUIRED)` and
link `develop::<component>`.

## Rules

* Steady-state paths allocate nothing from the process heap. A component that claims this has an `AllocScope` test.
* Every component has tests that run under ASan+UBSan and TSan.
* Windows and macOS branches that were not built and run are marked as such in the migration plan; do not trust them.
* No component depends on a logger: they report through a hook (`hw::setWarnHook`, `net::setLogHook`).

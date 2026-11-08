# libcore

libcore is the shared foundation under a family of related projects: libsql, librpc, libnosql, libhttp, libbufs, libscript, cortex, and gscope.

Each component is self-contained and depends only on the components listed before it. Everything builds as C++17, except cortex, which is C++20 and uses these components unchanged.

## What's inside

| Component | What it does |
|---|---|
| `libbase` | Status and `Result` types, span/slice views, hashes and checksums (xxh3, FNV-1a, CRC32C, SHA-1), base64/hex, UTF-8 and UTF-16, locale-independent number conversion |
| `libsys` | OS layer: virtual memory and executable pages, futex waits, CPU topology, processes without a shell, files and durability, sockets, shared memory, running as a service (`sys/service.hpp`: one way to stop, and the Windows service manager) |
| `libmem` | Memory allocators: slab, size-class, arena (plain and with destructors), object pools, a single-owner small-object heap, a standard allocator over a pool, and a caching allocator |
| `libsched` | Thread pool, `parallelFor`, timer heap and wheel, task queue |
| `liblog` | Asynchronous logger with per-thread rings and console, file, syslog, and callback sinks |
| `libjson` | JSON value type, parser, and an allocation-free writer |
| `libtar` | Hardened tar reader and writer (ustar, pax, GNU) |
| `libschema` | Front end for the `.bufs` schema language: parser, checker, model |
| `libcfg` | Typed parameters from command line, environment, and URL query |
| `libhw` | Hardware probing and metrics (tegrastats, NVML, nvidia-smi, `/proc`) |
| `libdev` | Device, stream, event, and allocator layer over a driver: a host simulator, or CUDA loaded at runtime |
| `libnet` | Reactor loops (epoll, io_uring, IOCP), TCP listener and connection |
| `libbench` | Benchmark helpers: allocation counting and soak runs |
| `buildkit` | CMake modules, warning flags, and a sanitizer switch |

## Guarantees

- Steady-state paths don't allocate from the process heap. Each component that claims this has a test that checks it.
- Every component is tested under AddressSanitizer, UndefinedBehaviorSanitizer, and ThreadSanitizer.
- Windows and macOS code paths that haven't been built and run are marked as such in the migration plan. Don't rely on them yet. What tunnel uses on Windows has been built and run there: base, sys (not `Process` and `SharedMemory`, which are POSIX only), mem, sched, log, json, cfg, schema and net's IOCP loop. libhw, libdev and libtar have not been ported.
- No component depends on a logger. They report through hooks such as `hw::setWarnHook` and `net::setLogHook`.

## Building

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build --output-on-failure
```

Sanitizers: `-DCORE_SANITIZE=address,undefined` or `-DCORE_SANITIZE=thread`. For a longer memory soak test: `CORE_SOAK_SECONDS=86400 build/libbench/bench_soak_memory`.

## Using it

As a subdirectory, with a sibling checkout:

```cmake
set(CORE_BUILD_TESTS OFF CACHE BOOL "")
add_subdirectory(../libcore libcore)
target_link_libraries(mylib PRIVATE core::mem core::sys)
```

Or installed system-wide:

```bash
cmake --install build --prefix /opt/develop
```

Then in your project: `find_package(libcore CONFIG REQUIRED)` and link `core::<component>`.

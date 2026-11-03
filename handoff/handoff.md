# Handoff — continuing on a machine with an NVIDIA GPU

**Date:** 2026-11-02. **Companion files:** `handoff.sh` (next to this file) and `components-plan-2026-11-01.md` (the plan; §0.1 is the progress log, and its "Third session" table lists everything done since the last handoff).

Everything that could be built and tested without a GPU is done and pushed. This document says exactly what to run on the GPU machine, what each result means, and what to implement next, in order. Each task has *where / what / how / done when*, so you or an agent can execute it without re-deriving context.

---

## 0. Where everything is

| Repository | Branch | Role |
|---|---|---|
| `philipp-orlov/libcore` | `main` | the shared components (13 libraries) |
| `philipp-orlov/cortex` | `main` | training/inference; CUDA backend |
| `philipp-orlov/libhttp` | `main` | HTTP/WebSocket server |
| `philipp-orlov/libnosql` | `main` | embedded KV store |
| `philipp-orlov/libsql` | `main` | SQL engine |
| `philipp-orlov/librpc` | `main` | RPC |
| `philipp-orlov/libbufs` | `main` | serialization |
| `philipp-orlov/gscope` | `main` | GPU/system monitor (uses libhw) |

All repositories expect to be **siblings in one directory** (each has a `<REPO>_LIBCORE_DIR` CMake cache variable defaulting to `../libcore`, and libsql/librpc/gscope find libhttp, libbufs, libnosql the same way). `handoff.sh` sets that up.

Commit identity everywhere: **Philipp Orlov `<philipp@gotcf.net>`** (the script sets it per repository). Local work branches use the `develop/` prefix; the script creates `develop/gpu`.

**The script itself was tested here** (x86-64, no GPU) against fresh clones of all eight repositories: check ok, fetch ok, core 39/39 (`dev_cuda` skipped), libbufs 22/23 (`bufs_languages`), libhttp 32/32, libnosql 30/30, librpc 9/10 (an `rpc_allocations` RSS flake, since fixed: the transport rings are prefaulted), libsql 29/29, gscope 12/12. That run also found that libcore could not configure from a clean clone; fixed in `e3ec626`.

## 1. Before you start (machine prerequisites)

| Need | Why | Check |
|---|---|---|
| NVIDIA driver + CUDA toolkit; **CUDA 13 (or 12.9+) for sm_120/121** (GB10 / RTX 50xx class) | cortex's default `CMAKE_CUDA_ARCHITECTURES` is `121-real;121-virtual` (GB10); the script detects the real value with `nvidia-smi` | `nvcc --version`, `nvidia-smi` |
| `nvcc` on `PATH` | the cortex CUDA build | `export PATH=/usr/local/cuda/bin:$PATH` |
| a host GCC that your nvcc supports | nvcc refuses newer host compilers; cortex `.cu` files are C++20, so GCC ≥ 11 | `nvcc --version` release notes; set `-DCMAKE_CUDA_HOST_COMPILER=/usr/bin/g++-12` if needed |
| CMake ≥ 3.24, Ninja (optional, faster) | `-real/-virtual` architecture syntax | `cmake --version` |
| OpenSSL dev headers (optional) | libhttp TLS / libsql `wss://`; the script turns TLS off if absent | `pkg-config --modversion openssl` |
| GitHub SSH key with access to the eight repositories | cloning private repositories | `ssh -T git@github.com` (or `REMOTE=https://github.com/philipp-orlov`) |
| Python 3 + numpy + OpenCV + Pillow (optional) | regenerating golden images (`cortex/tools/py/*.py`) | `python3 -c "import cv2, numpy, PIL"` |
| `cortex/scripts/upstream.env` (optional) | `scripts/fetch_models.sh` downloads `configs/models/*.yaml`; without them `unit_optim` and `unit_engine` fail | copy `scripts/upstream.env.example`, set `UPSTREAM_MODELS_URL` |

**If the GPU machine is aarch64 (GB10 / DGX Spark, Jetson, Grace):** this is the **first build of libcore on ARM**. Code paths that were written but never compiled: `base::crc32c` ARMv8 CRC path (`libbase/src/crc32c.cpp`, `getauxval(AT_HWCAP)` + `target("arch=armv8-a+crc")`), `sys::cpuFeatures()` ARM branch (`libsys/src/cpu.cpp`), libhw's Jetson/tegrastats detection. Watch the `core-release` results for `base_hash` and `sys_cpu` first.

## 2. Step 1 — run the script

```bash
git clone git@github.com:philipp-orlov/libcore.git ~/develop/libcore
```

```bash
~/develop/libcore/handoff/handoff.sh
```

That runs, in order: `check` (tools, GPUs, `nvidia-smi topo -m`), `fetch` (clone/fast-forward all eight, create `develop/gpu`; never resets local work), `core` (libcore Release + the GPU-only `dev_cuda` test), `consumers` (libbufs, libhttp, libnosql, librpc, libsql, gscope), `cortex` (CPU build with `-L fast|golden`, then the CUDA build with the detected architecture and the same labels, plus verbose output of `cortex_cuda_features` and the backend parity binary), and `report`.

Useful variants:

```bash
SANITIZERS=1 ~/develop/libcore/handoff/handoff.sh core
```

```bash
~/develop/libcore/handoff/handoff.sh cortex report
```

```bash
CORE_SOAK_SECONDS=86400 ~/develop/libcore/handoff/handoff.sh soak
```

**Output:** `~/develop/handoff-results/summary.md` (one table + the failing tests' output) and one log per stage. Paste `summary.md` back into the next session; it is written to be enough to continue from.

### What "good" looks like

| Stage | Expected on a healthy machine | Known, acceptable deviations |
|---|---|---|
| core-release | 39/39 (38 + `dev_cuda`) | none |
| core-gpu | `dev_test_cuda passed on N device(s)`: guard, segments on their device, cross-stream reuse, steady state, pinned pool, peer access matrix printed | `peerAccessWhereAvailable` only prints with one GPU |
| libbufs | 22/23 | `bufs_languages` needs `esbuild` and `dotnet` |
| libhttp, libnosql, librpc, libsql, gscope | 32/32, 30/30, 10/10, 29/29, 12/12 | none known (the `rpc_allocations` RSS flake was fixed by prefaulting the transport rings, librpc `916d2c6`) |
| cortex-cpu | all `fast` + `golden` pass | `unit_optim`, `unit_engine` without the model YAMLs; `install_and_examples` expects a tree at `cortex/build` |
| cortex-cuda | same as cpu **plus** `cortex_cuda_features`, `test_cuda_backend`, `test_cuda_allocator`, `test_cuda_conv` | this is the first run of my cortex CUDA changes (see §3, T1) |

## 3. Step 2 — the GPU work, in order

### T1. Verify what was written blind (do first; small)

*Where:* cortex `src/backend/cuda/cuda_allocator.cpp` (BUG-H7, H8 from commit `cc914ff`), `src/backend/cuda/cuda_backend.cpp:244–251` (L8 `CX_CUDA_DEVICE` parsing, `3f086e4`), libcore `libdev/src/cuda_driver.cpp`.
*What:* these compile for the first time on your machine.
*How:* the `cortex-cuda` stage builds and runs `tests/backend_parity/test_cuda_allocator.cpp` (rounding classes, cache accounting: `cachedBytes` never underflows after `emptyCache`, retention cap from `CX_CACHE_LIMIT_MB`). Then by hand: `CX_CUDA_DEVICE=abc build/cortex-cuda/apps/cortex …` must fail with "is not a device index"; `CX_CUDA_DEVICE=0` must work.
*Done when:* cortex-cuda passes, and `dev_test_cuda` passes.

### T2. cortex `CudaAllocator` onto `mem::CachingAllocator` (BUG-H8 for device memory)

*Where:* cortex `src/backend/cuda/cuda_allocator.{hpp,cpp}` (246 lines; exact-size `std::map<size, vector<Block>>` free lists, `pool_`/`pins_`/`references_` for CUDA-graph capture).
*What:* the same move the CPU allocator got in `b81b7b1`: best fit, split, coalesce, bounded retention, everything off-heap. Keep the public `Allocator` interface and **keep the capture semantics exactly** (blocks referenced by a live recording go to a separate pool and are never handed to non-capture allocations).
*How:*
1. Look at `CpuAllocator` in `src/backend/cpu/cpu_backend.cpp` after `b81b7b1`: `mem::CachingAllocator` over a `mem::BackingOps` table; `Block.handle` packs `node << 32 | generation`.
2. For CUDA, the backing is `cudaMalloc/cudaFree` (Device), `cudaMallocManaged` (Managed) or `cudaHostAlloc(mapped)` (Mapped). Either use `dev::DeviceAllocator` (libdev; it already wraps one `CachingAllocator` per device with a `DeviceGuard` around segment allocation) through a small `dev::Driver` adapter, or write a `BackingOps` with those three calls directly. The adapter is preferable because T3 needs the per-device guard anyway. Note that libdev's `Driver::malloc` is `cudaMalloc` only: add `mallocManaged`/`hostAllocMapped` to `dev::Driver` (both implementations; `HostDriver` can just use host memory) or keep Managed/Mapped on the old path.
3. Segment size: 2 MiB minimum, `cudaMalloc` of larger requests rounded to 2 MiB (that is what the CPU side does; CUDA's own granularity is 2 MiB).
4. Capture: keep a **second** `CachingAllocator` instance for the capture pool; `beginCapture` switches allocations to it, `endCapture`/`releaseCapture` keep their current bookkeeping (`pins_`, `references_`) but the freed blocks go back to the capture instance. Do not mix the two.
5. `stats()`: map `CacheStats` → `MemoryStats` as the CPU allocator does. `cachedBytes` now counts idle bytes inside held segments, so `test_cuda_allocator`'s exact-equality checks may need the same `>=` relaxation `test_dispatch` got. Say so in the commit.
*Done when:* `test_cuda_allocator` and the whole `cortex-cuda` suite pass; a fragmentation run (train 2,000 steps of the smallest model with varying batch sizes, or `bench/` if it has one) shows `segmentBytes / peakAllocatedBytes ≤ 1.3` and `backingAllocs` flat after warm-up; `nvidia-smi --query-gpu=memory.used --format=csv -l 5` stays flat over a 30-minute run.

### T3. Multi-GPU CUDA backend (BUG-H9, Phase 6)

*Where:* cortex `src/backend/cuda/cuda_backend.cpp` (`createCudaBackend`, `CudaBackend` ctor), `cuda_stream.cpp`, `launch/matmul.cpp:100–105` (cuBLASLt handle + 32 MiB `cudaMalloc` workspace), `src/core/backend.cpp:48` (registration), `cuda_entry.hpp`.
*What already exists (CPU-tested):* the registry is keyed by `(DeviceType, index)` with `backend(Device)` / `backendAvailable(Device)`; `Backend::deviceIndex()`; `Storage` frees on the stream that last used it (`noteUse`); `tests/unit/core/test_multi_device.cpp` proves allocation/stream/free routing with two fake devices.
*How:*
1. `createCudaBackend()` → `createCudaBackends()` returning one backend per visible device (respect `CUDA_VISIBLE_DEVICES`; keep `CX_CUDA_DEVICE` as "default device" for code that asks for `Device{Cuda}` without an index). Delete the `device == 0` check at `cuda_backend.cpp:250`.
2. Every runtime call that depends on the current device runs under `dev::DeviceGuard` (or a cortex equivalent: `cudaGetDevice`/`cudaSetDevice` save-restore). That means stream creation, `cudaMalloc`, kernel launches, cuBLASLt calls, event record/query, `cudaHostAlloc` for staging. The current device is **per host thread**, so worker threads must set it too: the bug class is "works on GPU 0".
3. Per-device state: cuBLASLt handle, matmul workspace, kernel streams, the allocator from T2 (one per device: `dev::System` gives you exactly that), capture pools. Kernel registration (`registerCudaKernels()`) is process-wide function pointers and stays global.
4. Cross-device copies: `cudaMemcpyPeerAsync` when `cudaDeviceCanAccessPeer`, otherwise staged through pinned host memory (`dev::makePinnedPool`). Enable peer access once per pair at start-up (`dev_test_cuda` prints the matrix).
5. Tests: port `test_multi_device.cpp` to CUDA as `tests/backend_parity/test_cuda_multi_device.cpp`, skipped (`SKIP_RETURN_CODE 77`) with fewer than 2 devices. Cases: a tensor created on `cuda:1` while `cuda:0` is current lands on device 1 (`cudaPointerGetAttributes`), an op on `cuda:1` runs on a `cuda:1` stream, freeing from a thread whose current device is 0 frees on device 1, a peer copy round-trips bit-exactly.
*Done when:* all CUDA tests pass on 1 GPU (multi-device cases skipped) and on ≥ 2 GPUs (multi-device cases run). With a single-GPU machine, the routing logic stays covered by `test_multi_device.cpp` (CPU fakes) and libdev's `HostDriver` tests.

### T4. Pinned staging buffers from a pool

*Where:* cortex `src/backend/cuda/cuda_backend.cpp:118` and `:165/:221` (`cudaHostAlloc`/`cudaFreeHost` per transfer).
*What/how:* take staging memory from `dev::makePinnedPool(driver)` (`mem::RegisteredPool`: registered once, reused, bounded, largest-first idle eviction). Free on the stream's completion, as now.
*Done when:* a training run shows no `cudaHostAlloc` after warm-up (`nsys profile --trace=cuda` or `ltrace -e cudaHostAlloc`).

### T5. Measure (needed before calling Phase 6 done)

1. cortex benchmarks before/after T2–T4 (`-DCORTEX_BUILD_BENCH=ON`; the plan's rule: > 2 % regression blocks).
2. `CORE_SOAK_SECONDS=86400 handoff.sh soak` on the target machine: window maxima of RSS and pool gauges must stay within +2 %.
3. A 24-hour training soak with `nvidia-smi` memory logging; flat after warm-up.
4. gscope on the GPU machine: `gscope --port 8080`, open `/health` and the metrics endpoints; the NVML provider (dlopen `libnvidia-ml.so.1`) and `nvidia-smi` fallback are running live for the first time. On Jetson: the tegrastats provider.

### T6. Optional on the GPU machine

* `-DCORE_NET_IO_URING=ON` (libnet's io_uring backend was moved but never built; needs liburing ≥ 2.0 headers): `handoff.sh core` with that flag, then libhttp's tests.
* GCC 9 / C++17 check of libcore (`libcore/.github/workflows/ci.yml` has the job; it has never run).

## 4. Things that are not done and do not need a GPU

These are in the plan's "Not yet touched" list; any machine works:

* libhttp `BufferPool`/`SlabPool` onto libmem (needs allocator hooks and pool-outliving leases in libmem first; see the plan, §0.1 Phase 7–8 table).
* `numtext`, `flat_hash`, `fmt` in libbase; libjson arena tree; libhttp timers on `sched::TimerWheel` (its callbacks exceed the 48-byte inline slot).
* Windows/macOS gaps (`sys::Process` CreateProcess, `SharedMemory`, `cpuTopology`, a kqueue loop).
* A per-thread magazine for `SizeClassPool` large blocks (removes the last two shared atomics; see the plan's contention row).

## 5. Gotchas found along the way

* **CI** runs on every push in all eight repositories. Each checks out its sibling repositories at `main`, side by side; libcore is public, so the default token reads it. cortex's CI has no model YAMLs (`scripts/fetch_models.sh` needs `UPSTREAM_MODELS_URL`), so the tests that build a model skip there.
* **libcore's `.gitignore` used to hide `buildkit/`** (`build*/` matched it): a clean clone could not configure until commit "buildkit: track the CMake modules…". If you see `include could not find requested file: DevelopCompiler`, your checkout predates that commit.
* `libcudart` is loaded with `dlopen`. If `dev_test_cuda` says "libcudart is not installed", set `CORE_CUDART=/usr/local/cuda/lib64/libcudart.so` (or add that directory to `LD_LIBRARY_PATH`).
* Unified-memory GPUs (GB10, Jetson): cortex defaults to `cudaMalloc` memory (`CX_CUDA_MEMORY=device`); try `CX_CUDA_MEMORY=managed` for host-visible tensors without copies and compare throughput.
* librpc commit `0798f1a` carries tool-attribution trailers the environment added; rewrite it if you want (`git commit --amend` + `git push --force-with-lease`), the environment refused force-pushes.
* Pushing from the GPU machine: `git push origin develop/gpu:<branch-you-want>`; the script never pushes.

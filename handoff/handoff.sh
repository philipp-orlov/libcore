#!/usr/bin/env bash
# Copyright (c) 2026 Philipp Orlov
# SPDX-License-Identifier: Apache-2.0
#
# Continue the libcore unification on a machine with an NVIDIA GPU.
# Clones (or updates) the eight repositories side by side, builds and tests everything, then builds cortex with the
# CUDA backend and runs its GPU tests, and writes one summary you can paste back.
#
#   ./handoff.sh                 # check, fetch, core, consumers, cortex, report
#   ./handoff.sh check           # prerequisites only
#   ./handoff.sh fetch core      # any subset of stages, in the order given
#   ./handoff.sh soak            # long allocation soak (CORE_SOAK_SECONDS, default 600)
#
# Environment (all optional):
#   WS            workspace directory holding the checkouts       (default: $HOME/develop)
#   BRANCH        branch to check out in every repository         (default: main)
#   REMOTE        clone base; ssh or https                        (default: git@github.com:philipp-orlov)
#   SUFFIX        appended to REMOTE/<repo>                       (default: .git; empty for local directories)
#   LOCAL_BRANCH  local work branch created from BRANCH           (default: develop/gpu)
#   JOBS          parallel build jobs                             (default: nproc)
#   SANITIZERS=1  also build/test libcore under ASan+UBSan and TSan
#   CUDA_ARCH     CMAKE_CUDA_ARCHITECTURES for cortex             (default: detected with nvidia-smi)
#   GOLDEN=1      also run cortex's golden tests                  (default: 1)
#   CORE_SOAK_SECONDS  length of the soak stage                (default: 600)
#
# Nothing here pushes. Every stage logs to $WS/handoff-results/<stage>.log; a failing stage does not stop the next.

set -uo pipefail

WS="${WS:-$HOME/develop}"
BRANCH="${BRANCH:-main}"
REMOTE="${REMOTE:-git@github.com:philipp-orlov}"
SUFFIX="${SUFFIX-.git}"
LOCAL_BRANCH="${LOCAL_BRANCH:-develop/gpu}"
JOBS="${JOBS:-$(nproc 2>/dev/null || echo 4)}"
SANITIZERS="${SANITIZERS:-0}"
GOLDEN="${GOLDEN:-1}"
RESULTS="$WS/handoff-results"
REPOS=(libcore libbufs libhttp libnosql librpc libsql gscope cortex)
CONSUMERS=(libbufs libhttp libnosql librpc libsql gscope)

mkdir -p "$RESULTS"
SUMMARY="$RESULTS/summary.md"
declare -A STATUS=()

info() { printf '\033[1;34m==>\033[0m %s\n' "$*"; }
warn() { printf '\033[1;33m!!\033[0m %s\n' "$*"; }
record() {   # record <stage> <ok|FAIL|skipped> <detail>
    STATUS["$1"]="$2 — $3"
    printf '%-22s %-8s %s\n' "$1" "$2" "$3"
}

generator_args() { command -v ninja >/dev/null 2>&1 && echo "-G Ninja"; }

# ---------------------------------------------------------------------------------------------------------------- check
stage_check() {
    info "Prerequisites"
    local log="$RESULTS/check.log" missing=0
    {
        echo "date: $(date -u +%FT%TZ)"
        echo "host: $(uname -a)"
        for tool in git cmake ninja g++ clang++ nvcc nvidia-smi python3; do
            if command -v "$tool" >/dev/null 2>&1; then
                printf '%-11s %s\n' "$tool" "$("$tool" --version 2>&1 | head -1)"
            else
                printf '%-11s MISSING\n' "$tool"
            fi
        done
        if command -v nvidia-smi >/dev/null 2>&1; then
            nvidia-smi --query-gpu=index,name,compute_cap,memory.total,driver_version --format=csv
            nvidia-smi topo -m 2>/dev/null || true
        fi
        echo "openssl: $(pkg-config --modversion openssl 2>/dev/null || echo 'not found (TLS options will be turned off)')"
    } >"$log" 2>&1
    cat "$log"
    for tool in git cmake g++; do command -v "$tool" >/dev/null 2>&1 || missing=1; done
    local cmake_minor
    cmake_minor=$(cmake --version 2>/dev/null | head -1 | sed -E 's/.* ([0-9]+)\.([0-9]+).*/\1 \2/')
    if [[ -n "$cmake_minor" ]] && (( ${cmake_minor% *} < 3 || (${cmake_minor% *} == 3 && ${cmake_minor#* } < 24) )); then
        warn "cmake 3.24+ is needed (cortex uses CMAKE_CUDA_ARCHITECTURES with -real/-virtual)"; missing=1
    fi
    command -v nvcc >/dev/null 2>&1 || warn "nvcc not on PATH: export PATH=/usr/local/cuda/bin:\$PATH (cortex CUDA stage will be skipped)"
    if (( missing )); then record check FAIL "see $log"; else record check ok "$(grep -c MISSING "$log") optional tools missing"; fi
}

# ---------------------------------------------------------------------------------------------------------------- fetch
stage_fetch() {
    info "Fetch $BRANCH into $WS"
    local log="$RESULTS/fetch.log" failed=0
    : >"$log"
    for repo in "${REPOS[@]}"; do
        local dir="$WS/$repo"
        if [[ -d "$dir/.git" ]]; then
            # Never reset local work: fast-forward the local branch when possible, otherwise say so and leave it.
            git -C "$dir" fetch origin "$BRANCH" >>"$log" 2>&1 || failed=1
            if git -C "$dir" rev-parse --verify -q "$LOCAL_BRANCH" >/dev/null; then
                git -C "$dir" checkout "$LOCAL_BRANCH" >>"$log" 2>&1 &&
                    git -C "$dir" merge --ff-only "origin/$BRANCH" >>"$log" 2>&1 ||
                    warn "$repo: $LOCAL_BRANCH has diverged from origin/$BRANCH; left as is"
            else
                git -C "$dir" checkout -b "$LOCAL_BRANCH" "origin/$BRANCH" >>"$log" 2>&1 || failed=1
            fi
        else
            git clone --branch "$BRANCH" "$REMOTE/$repo$SUFFIX" "$dir" >>"$log" 2>&1 &&
                git -C "$dir" checkout -b "$LOCAL_BRANCH" >>"$log" 2>&1 || failed=1
        fi
        if [[ -d "$dir/.git" ]]; then
            git -C "$dir" config user.name "Philipp Orlov"
            git -C "$dir" config user.email "philipp@gotcf.net"
            echo "$repo $(git -C "$dir" rev-parse --short HEAD)" | tee -a "$log"
        else
            warn "$repo: clone failed (see $log)"
        fi
    done
    if (( failed )); then record fetch FAIL "see $log"; else record fetch ok "${#REPOS[@]} repositories at $BRANCH"; fi
}

# Runs cmake configure + build + ctest; echoes "passed/total" into the stage status.
build_and_test() {   # build_and_test <stage> <src> <build> <ctest-args> -- <cmake args...>
    local stage="$1" src="$2" build="$3" ctest_args="$4"
    shift 4
    [[ "${1:-}" == "--" ]] && shift
    local log="$RESULTS/$stage.log"
    {
        echo "### configure"
        # shellcheck disable=SC2046
        cmake -S "$src" -B "$build" $( [[ -f "$build/CMakeCache.txt" ]] || generator_args ) "$@" &&
        echo "### build" &&
        cmake --build "$build" -j "$JOBS"
    } >"$log" 2>&1
    if [[ $? -ne 0 ]]; then
        record "$stage" FAIL "build failed: tail of $log below"
        tail -30 "$log" >>"$RESULTS/failures.txt"
        return 1
    fi
    echo "### test" >>"$log"
    # shellcheck disable=SC2086
    ctest --test-dir "$build" --output-on-failure -j "$JOBS" --timeout 900 $ctest_args >>"$log" 2>&1
    local rc=$?
    local line
    line=$(grep -E "tests passed|No tests were found" "$log" | tail -1)
    local failed
    failed=$(sed -n '/The following tests FAILED/,$p' "$log" | grep -E '^\s+[0-9]+ - ' | tr -s ' ' | tr '\n' ';')
    if (( rc == 0 )); then
        record "$stage" ok "${line:-no ctest summary}"
    else
        record "$stage" FAIL "${line:-?} ${failed}"
        { echo "== $stage"; sed -n '/The following tests FAILED/,$p' "$log"; } >>"$RESULTS/failures.txt"
    fi
    return $rc
}

ssl_args() {
    if pkg-config --exists openssl 2>/dev/null; then return; fi
    echo "-DHTTP_WITH_SSL=OFF -DSQL_WITH_TLS=OFF -DRPC_WITH_TLS=OFF"
}

# ----------------------------------------------------------------------------------------------------------------- core
stage_core() {
    info "libcore (Release$( ((SANITIZERS)) && echo ', ASan+UBSan, TSan'))"
    build_and_test core-release "$WS/libcore" "$WS/build/core-release" "" -- -DCMAKE_BUILD_TYPE=Release
    # The GPU-only test is in the run above (label gpu); show its output on its own so it is easy to find.
    if [[ -x "$WS/build/core-release/libdev/dev_test_cuda" ]]; then
        "$WS/build/core-release/libdev/dev_test_cuda" >"$RESULTS/core-gpu.log" 2>&1
        case $? in
            0) record core-gpu ok "dev_test_cuda passed on $(grep -o '[0-9]* device(s)' "$RESULTS/core-gpu.log")" ;;
            77) record core-gpu skipped "$(head -1 "$RESULTS/core-gpu.log")" ;;
            *) record core-gpu FAIL "see $RESULTS/core-gpu.log"; cat "$RESULTS/core-gpu.log" >>"$RESULTS/failures.txt" ;;
        esac
    fi
    if ((SANITIZERS)); then
        build_and_test core-asan "$WS/libcore" "$WS/build/core-asan" "" -- -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCORE_SANITIZE=address,undefined
        build_and_test core-tsan "$WS/libcore" "$WS/build/core-tsan" "" -- -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCORE_SANITIZE=thread
    fi
}

# ------------------------------------------------------------------------------------------------------------ consumers
stage_consumers() {
    info "Consumers: ${CONSUMERS[*]}"
    local extra
    extra=$(ssl_args)
    for repo in "${CONSUMERS[@]}"; do
        # shellcheck disable=SC2086
        build_and_test "$repo" "$WS/$repo" "$WS/build/$repo" "" -- -DCMAKE_BUILD_TYPE=Release $extra
    done
}

# --------------------------------------------------------------------------------------------------------------- cortex
detect_cuda_arch() {
    if [[ -n "${CUDA_ARCH:-}" ]]; then echo "$CUDA_ARCH"; return; fi
    command -v nvidia-smi >/dev/null 2>&1 || return
    local caps list="" last=""
    caps=$(nvidia-smi --query-gpu=compute_cap --format=csv,noheader 2>/dev/null | tr -d ' .' | sort -u)
    for c in $caps; do list+="${c}-real;"; last="$c"; done
    [[ -n "$last" ]] && echo "${list}${last}-virtual"
}

stage_cortex() {
    info "cortex (CPU build, then CUDA build)"
    if [[ ! -f "$WS/cortex/configs/models/yolo26.yaml" && -f "$WS/cortex/scripts/upstream.env" ]]; then
        (cd "$WS/cortex" && scripts/fetch_models.sh) >"$RESULTS/cortex-models.log" 2>&1 ||
            warn "fetch_models.sh failed (see $RESULTS/cortex-models.log)"
    fi
    [[ -f "$WS/cortex/configs/models/yolo26.yaml" ]] ||
        warn "configs/models/*.yaml missing: unit_optim and unit_engine will fail (cp scripts/upstream.env.example scripts/upstream.env, fill UPSTREAM_MODELS_URL, rerun)"
    local labels="-L fast"
    ((GOLDEN)) && labels="-L fast|golden"
    build_and_test cortex-cpu "$WS/cortex" "$WS/build/cortex-cpu" "$labels" -- \
        -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCORTEX_BACKEND_CUDA=OFF -DCORTEX_BUILD_BENCH=OFF
    if ! command -v nvcc >/dev/null 2>&1; then
        record cortex-cuda skipped "nvcc not found (export PATH=/usr/local/cuda/bin:\$PATH)"
        return
    fi
    local arch
    arch=$(detect_cuda_arch)
    if [[ -z "$arch" ]]; then
        record cortex-cuda skipped "no GPU visible to nvidia-smi and CUDA_ARCH not set"
        return
    fi
    info "cortex CUDA, CMAKE_CUDA_ARCHITECTURES=$arch"
    build_and_test cortex-cuda "$WS/cortex" "$WS/build/cortex-cuda" "$labels" -- \
        -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCORTEX_BACKEND_CUDA=ON "-DCMAKE_CUDA_ARCHITECTURES=$arch" -DCORTEX_BUILD_BENCH=ON
    # The CUDA feature probe and the backend parity binary, verbose, for the record.
    for t in cortex_cuda_features cortex_test_backend_parity; do
        local bin
        bin=$(find "$WS/build/cortex-cuda" -type f -name "$t" -perm -u+x 2>/dev/null | head -1)
        [[ -n "$bin" ]] && { echo "### $t"; "$bin" 2>&1 | tail -40; } >>"$RESULTS/cortex-cuda-detail.log"
    done
}

# ----------------------------------------------------------------------------------------------------------------- soak
stage_soak() {
    local seconds="${CORE_SOAK_SECONDS:-600}"
    info "Allocation soak, ${seconds}s"
    local bin="$WS/build/core-release/libbench/bench_soak_memory"
    [[ -x "$bin" ]] || build_and_test core-release "$WS/libcore" "$WS/build/core-release" "-R none" -- -DCMAKE_BUILD_TYPE=Release
    if CORE_SOAK_SECONDS="$seconds" "$bin" >"$RESULTS/soak.log" 2>&1; then
        record soak ok "$(tail -1 "$RESULTS/soak.log")"
    else
        record soak FAIL "see $RESULTS/soak.log"
    fi
}

# --------------------------------------------------------------------------------------------------------------- report
stage_report() {
    {
        echo "# handoff results — $(date -u +%FT%TZ)"
        echo
        echo "Host: \`$(uname -srm)\`"
        command -v nvidia-smi >/dev/null 2>&1 &&
            echo "GPUs: $(nvidia-smi --query-gpu=name,compute_cap --format=csv,noheader | paste -sd ';' -)"
        echo
        echo "| stage | result |"
        echo "|---|---|"
        for k in "${!STATUS[@]}"; do echo "| $k | ${STATUS[$k]} |"; done | sort
        echo
        echo "Commits:"
        for repo in "${REPOS[@]}"; do
            [[ -d "$WS/$repo/.git" ]] && echo "- $repo \`$(git -C "$WS/$repo" rev-parse --short HEAD)\`"
        done
        if [[ -s "$RESULTS/failures.txt" ]]; then
            echo
            echo "## Failures"
            echo '```'
            tail -200 "$RESULTS/failures.txt"
            echo '```'
        fi
    } >"$SUMMARY"
    info "Summary: $SUMMARY"
    cat "$SUMMARY"
}

stages=("$@")
((${#stages[@]})) || stages=(check fetch core consumers cortex report)
: >"$RESULTS/failures.txt"
for s in "${stages[@]}"; do
    case "$s" in
        check|fetch|core|consumers|cortex|soak|report) "stage_$s" ;;
        *) echo "unknown stage: $s (check fetch core consumers cortex soak report)"; exit 2 ;;
    esac
done
[[ " ${stages[*]} " == *" report "* ]] || stage_report

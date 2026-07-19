# Pinned build/bench environment (DESIGN.md §9). Building this image IS the
# verification: it compiles the release preset, runs the full test suite, and
# pushes a 10k-command replay file through the end-to-end pipeline bench —
# PLAN.md's bench-smoke, realized here.
#
#   Build:  docker build -t lob .
#   Bench:  docker run --rm lob            # 100k-command e2e bench smoke
#   Shell:  docker run --rm -it lob bash   # all presets available inside
#
# Pinning policy, honestly stated: the base tag and the explicit gcc-13 /
# clang-17 packages pin the toolchain majors every published number cites
# (docs/results.md); apt still floats within noble point releases, and
# GoogleTest / Google Benchmark are exact-tag-pinned by FetchContent. Numbers
# measured inside a container on a VM remain indicative (DESIGN.md §11).
#
# The tsan preset additionally needs the host kernel's ASLR entropy lowered
# (sysctl vm.mmap_rnd_bits=28) — a kernel-wide knob an unprivileged container
# cannot set; run that preset on the host, or pass the sysctl to a privileged
# container (PROGRESS.md "Known issues").
FROM ubuntu:24.04

ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update \
    && apt-get install -y --no-install-recommends \
        ca-certificates git cmake ninja-build gcc-13 g++-13 clang-17 \
    && rm -rf /var/lib/apt/lists/*

# gcc-13 is the default toolchain — the one the published numbers cite;
# clang-17 is present for the asan / tsan presets (export CC/CXX to switch).
ENV CC=gcc-13 CXX=g++-13

WORKDIR /lob
COPY . .

RUN cmake --preset release && cmake --build --preset release
RUN ctest --preset release --output-on-failure

# Bench smoke: generate a small deterministic workload and run it through the
# full four-thread pipeline (producer -> engine -> logger). Asserts the
# release binaries actually execute, not just link.
RUN ./build/release/tools/lob_replay generate --out /tmp/smoke.lobr --ops 10000 --seed 42 \
    && ./build/release/bench/lob_e2e_bench /tmp/smoke.lobr \
    && rm /tmp/smoke.lobr

# Default command: a larger rerun so a bare `docker run` shows the engine
# doing real work, with per-op-type latency percentiles on stdout.
CMD ["bash", "-c", "./build/release/tools/lob_replay generate --out /tmp/bench.lobr --ops 100000 --seed 42 && ./build/release/bench/lob_e2e_bench /tmp/bench.lobr"]

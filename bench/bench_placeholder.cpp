#include <benchmark/benchmark.h>

#include "tickforge/version.hpp"

static void BM_VersionPatchAccess(benchmark::State& state) {
    volatile int patch = tickforge::kVersionPatch;
    for (auto _ : state) {
        benchmark::DoNotOptimize(patch);
    }
}
BENCHMARK(BM_VersionPatchAccess);

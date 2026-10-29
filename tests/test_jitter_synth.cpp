// SPDX-License-Identifier: GPL-3.0-or-later
// Synthetic fields with a border and vertical bars, damaged by per-line
// jitter, a crawling sine wobble, a top hook and a field offset. Edge Lock
// must take back exactly those offsets.
#include "core/Synth.h"
#include "testing.h"

#include "core/Simd.h"

#include <cstdio>

using namespace ud;

TEST_CASE("jitter, wobble, hook and field offset come back clean") {
    SynthScene s;
    DamageModel d;  // +-3 jitter, +-4 wobble over 60 lines, +15 hook, 1.5 field offset
    const int64_t frames = 40;
    TrackerConfig c = borderConfig(s, d, frames);
    CHECK(!c.refs.empty());
    const SynthRun r = runTracker(s, d, c, frames);
    CHECK_EQ(int64_t(r.fields.size()), frames * 2);
    const Residual res = residualOf(r, d);
    std::printf("  clean: rms %.4f px over %d measured lines of %d, all lines %.4f, worst %.3f\n", res.rmsMeasured, res.measured,
                res.lines, res.rmsAll, res.maxAbs);
    CHECK_MSG(res.rmsMeasured < 0.15, "residual %.4f px RMS", res.rmsMeasured);
    CHECK(res.measured > res.lines * 9 / 10);
}

TEST_CASE("with VHS-like noise the residual stays under half a pixel") {
    SynthScene s;
    s.noise = 4.0;
    s.sigma = 1.8;
    DamageModel d;
    const int64_t frames = 40;
    TrackerConfig c = borderConfig(s, d, frames);
    CHECK(!c.refs.empty());
    const SynthRun r = runTracker(s, d, c, frames);
    const Residual res = residualOf(r, d);
    std::printf("  noisy: rms %.4f px over %d measured lines of %d, all lines %.4f\n", res.rmsMeasured, res.measured, res.lines, res.rmsAll);
    CHECK_MSG(res.rmsMeasured < 0.5, "residual %.4f px RMS", res.rmsMeasured);
    CHECK(res.measured > res.lines * 3 / 4);
}

TEST_CASE("AVX2 and plain kernels agree") {
    if (!cpuHasAvx2()) return;
    SynthScene s;
    DamageModel d;
    TrackerConfig c = borderConfig(s, d, 4);
    setSimdEnabled(false);
    const SynthRun a = runTracker(s, d, c, 4);
    setSimdEnabled(true);
    const SynthRun b = runTracker(s, d, c, 4);
    double worst = 0;
    for (size_t i = 0; i < a.fields.size(); ++i)
        for (size_t l = 0; l < a.fields[i].off.size(); ++l) worst = std::max(worst, double(std::fabs(a.fields[i].off[l] - b.fields[i].off[l])));
    CHECK_MSG(worst < 1e-3, "kernels differ by %.6f px", worst);
}

TEST_MAIN()

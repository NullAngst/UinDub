// SPDX-License-Identifier: GPL-3.0-or-later
// A fade to black across the border: measurement stops instead of chasing
// noise, offsets ease to zero, and nothing snaps when the picture returns.
#include "core/Synth.h"
#include "testing.h"

#include <cmath>
#include <cstdio>

using namespace ud;

TEST_CASE("fade to black and back") {
    SynthScene s;
    s.noise = 1.0;
    // Full picture, fade out over fields 20-40, black 40-100, fade in 100-120.
    s.fade = [](int64_t f) -> double {
        if (f < 20) return 1.0;
        if (f < 40) return 1.0 - double(f - 20) / 20.0;
        if (f < 100) return 0.0;
        if (f < 120) return double(f - 100) / 20.0;
        return 1.0;
    };
    const DamageModel d;
    const int64_t frames = 80;
    TrackerConfig c = borderConfig(s, d, frames);
    const SynthRun r = runTracker(s, d, c, frames);
    CHECK_EQ(int64_t(r.fields.size()), frames * 2);
    // Find where measuring stopped and where it started again.
    int64_t lastMeasured = -1, firstBack = -1;
    for (const FieldOffsets& f : r.fields) {
        const bool m = f.track.flags & kFieldMeasured;
        if (f.field < 70 && m) lastMeasured = f.field;
        if (f.field >= 70 && m && firstBack < 0) firstBack = f.field;
    }
    std::printf("  measured until field %lld, back at %lld\n", (long long)lastMeasured, (long long)firstBack);
    CHECK(lastMeasured >= 25 && lastMeasured < 45);
    CHECK(firstBack > 95 && firstBack < 125);
    // Easing: the largest correction shrinks field by field, then sits at zero.
    double prevMax = 1e9;
    bool monotonic = true;
    double darkMax = 0;
    for (const FieldOffsets& f : r.fields) {
        if (f.field <= lastMeasured || f.field >= firstBack) continue;
        double mx = 0;
        for (float v : f.off) mx = std::max(mx, double(std::fabs(v)));
        if (mx > prevMax + 1e-6) monotonic = false;
        prevMax = mx;
        if (f.field > lastMeasured + 8) darkMax = std::max(darkMax, mx);
    }
    std::printf("  largest correction in the dark after easing: %.4f px\n", darkMax);
    CHECK(monotonic);
    CHECK_MSG(darkMax < 0.05, "%.4f px of correction in the dark", darkMax);
    // No snap on return: the first fields back measure the real damage.
    const Residual back = residualOf(r, d, firstBack, firstBack + 10);
    std::printf("  first 10 fields back: rms %.4f px\n", back.rmsMeasured);
    CHECK(back.rmsMeasured < 0.5);
    const Residual before = residualOf(r, d, 0, 20);
    CHECK(before.rmsMeasured < 0.2);
}

TEST_MAIN()

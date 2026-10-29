// SPDX-License-Identifier: GPL-3.0-or-later
// A hard cut mid-clip: a drawn edge ends at the cut; the border carries on.
#include "core/Synth.h"
#include "testing.h"

#include <cstdio>

using namespace ud;

TEST_CASE("drawn edge ends at a cut, border continues") {
    SynthScene s;
    s.cutFrame = 20;
    const DamageModel d;
    const int64_t frames = 40;
    TrackerConfig c = borderConfig(s, d, frames);
    Picture f0;
    renderFrame(s, d, 0, FieldOrder::TFF, f0);
    Reference r;
    r.id = 2;
    r.kind = RefKind::Drawn;
    RefSpan sp;
    std::string err;
    CHECK(makeDrawnSpan(f0, {{300, 80}, {300, 400}}, 0, frames, c.s, sp, err));
    r.spans.push_back(sp);
    c.refs.push_back(r);
    const SynthRun run = runTracker(s, d, c, frames);
    std::printf("  cuts:");
    for (int64_t f : run.result.cuts) std::printf(" %lld", (long long)f);
    std::printf("\n");
    CHECK_EQ(int(run.result.cuts.size()), 1);
    if (!run.result.cuts.empty()) CHECK_EQ(run.result.cuts[0], int64_t(20));
    CHECK_EQ(int(run.result.autoEnds.size()), 1);
    if (!run.result.autoEnds.empty()) {
        CHECK_EQ(run.result.autoEnds[0].first, 1);
        CHECK_EQ(run.result.autoEnds[0].second, int64_t(20));
    }
    int endedAfter = 0, borderAfter = 0, fieldsAfter = 0;
    for (const FieldOffsets& f : run.fields) {
        if (f.field / 2 < 20) {
            CHECK(f.track.refs[1].state == RefState::Active);
            continue;
        }
        ++fieldsAfter;
        endedAfter += f.track.refs[1].state == RefState::Ended;
        borderAfter += f.track.refs[0].state == RefState::Active;
    }
    CHECK_EQ(endedAfter, fieldsAfter);
    CHECK_EQ(borderAfter, fieldsAfter);
    const Residual after = residualOf(run, d, 40, 80);
    std::printf("  border after the cut: rms %.4f px\n", after.rmsMeasured);
    CHECK(after.rmsMeasured < 0.15);
}

TEST_MAIN()

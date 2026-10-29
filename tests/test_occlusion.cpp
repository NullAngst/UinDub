// SPDX-License-Identifier: GPL-3.0-or-later
// A box sweeps across a drawn edge: the edge goes Lost, holds, and comes back
// within 3 fields of the box clearing it. The occluder is textured, the
// way a hand or a sleeve is; a flat box with an edge exactly like the
// reference's would be indistinguishable from the edge moving.
#include "core/Synth.h"
#include "testing.h"

#include <cmath>
#include <cstdio>

using namespace ud;

TEST_CASE("occluder crosses a drawn edge") {
    SynthScene s;
    const double edgeX = 300.0;
    const double boxW = 80, speed = 3.0, start = 150;
    s.box = [&](int64_t f, double& x0, double& x1) {
        x0 = start + speed * double(f);
        x1 = x0 + boxW;
        return true;
    };
    s.boxY0 = 40;
    s.boxY1 = 440;
    // A bright, textured occluder, the way a sleeve or a hand looks.
    s.boxLevel = 225;
    s.boxTexture = 25;
    const DamageModel d;
    const int64_t frames = 60;
    TrackerConfig c = borderConfig(s, d, frames);
    // Draw on a frame before the box arrives.
    Picture f0;
    renderFrame(s, d, 0, FieldOrder::TFF, f0);
    Reference r;
    r.id = 2;
    r.kind = RefKind::Drawn;
    RefSpan sp;
    std::string err;
    CHECK(makeDrawnSpan(f0, {{edgeX, 80}, {edgeX, 400}}, 0, frames, c.s, sp, err));
    r.spans.push_back(sp);
    c.refs.push_back(r);
    const SynthRun run = runTracker(s, d, c, frames);
    // The box's far edge clears the search window around the edge here.
    const double clearX = edgeX + c.s.searchRange + c.s.templateWidth / 2.0;
    const int64_t clearField = int64_t(std::ceil((clearX - start) / speed));
    int64_t lostAt = -1, backAt = -1;
    double heldA = 0, drift = 0;
    for (const FieldOffsets& f : run.fields) {
        const RefFieldTrack& t = f.track.refs[1];
        if (t.state == RefState::Lost && lostAt < 0) {
            lostAt = f.field;
            heldA = t.a;
        }
        if (lostAt >= 0 && backAt < 0 && t.state == RefState::Lost) drift = std::max(drift, double(std::fabs(t.a - heldA)));
        if (lostAt >= 0 && backAt < 0 && t.state == RefState::Active) backAt = f.field;
    }
    std::printf("  lost at field %lld, box clears at %lld, back at %lld, held motion drifted %.3f px\n", (long long)lostAt,
                (long long)clearField, (long long)backAt, drift);
    CHECK(lostAt > 0);
    CHECK(drift < 0.05);
    CHECK(backAt >= 0);
    CHECK_MSG(backAt <= clearField + 3, "re-acquired %lld fields after clearing", (long long)(backAt - clearField));
    // The border kept the correction right the whole time.
    const Residual res = residualOf(run, d);
    CHECK(res.rmsMeasured < 0.2);
    // The box never dragged the drawn edge's motion.
    double worstA = 0;
    for (const FieldOffsets& f : run.fields) worstA = std::max(worstA, double(std::fabs(f.track.refs[1].a)));
    std::printf("  largest drawn-edge motion %.3f px (static edge)\n", worstA);
    CHECK(worstA < 1.5);
}

TEST_MAIN()

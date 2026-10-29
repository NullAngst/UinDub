// SPDX-License-Identifier: GPL-3.0-or-later
// The jitter damage plus a 2 px per field pan of a drawn edge. The pan is
// real motion and must survive; the jitter must still come out.
#include "core/Synth.h"
#include "testing.h"

#include <cmath>
#include <cstdio>

using namespace ud;

namespace {

SynthScene panScene() {
    SynthScene s;
    s.panBar = true;
    s.panX = 330;
    s.panW = 60;
    s.panPerField = 2.0;
    // Static bars kept clear of the pan's path.
    s.bars = {{150, 40}, {560, 30}};
    return s;
}

Reference drawnOn(const SynthScene& s, const DamageModel& d, int64_t frames, int id) {
    Picture f0;
    renderFrame(s, d, 0, FieldOrder::TFF, f0);
    Reference r;
    r.id = id;
    r.kind = RefKind::Drawn;
    r.name = "Pan edge";
    RefSpan sp;
    std::string err;
    EdgeLockSettings st;
    if (makeDrawnSpan(f0, {{s.panX + 1, 60}, {s.panX + 1, 420}}, 0, frames, st, sp, err)) r.spans.push_back(sp);
    else std::printf("  drawn span: %s\n", err.c_str());
    return r;
}

// Mean over each field's lines of (offset - damage), with the clip constant removed.
std::vector<double> perFieldError(const SynthRun& r, const DamageModel& d, double bias, int y0, int y1) {
    std::vector<double> out;
    for (const FieldOffsets& f : r.fields) {
        double s = 0;
        int n = 0;
        for (size_t l = 0; l < f.off.size(); ++l) {
            const int y = 2 * int(l) + f.parity;
            if (y < y0 || y > y1 || f.conf[l] < kConfMeasuredMin) continue;
            s += f.off[l] - d.offset(f.field, y) - bias;
            ++n;
        }
        out.push_back(n ? s / n : 0.0);
    }
    return out;
}

}  // namespace

TEST_CASE("border plus drawn edge: pan kept, jitter removed") {
    const SynthScene s = panScene();
    const DamageModel d;
    const int64_t frames = 20;
    TrackerConfig c = borderConfig(s, d, frames);
    c.refs.push_back(drawnOn(s, d, frames, 2));
    CHECK_EQ(int(c.refs.size()), 2);
    const SynthRun r = runTracker(s, d, c, frames);
    const Residual res = residualOf(r, d);
    std::printf("  border+drawn: rms %.4f px over %d lines\n", res.rmsMeasured, res.measured);
    CHECK(res.rmsMeasured < 0.15);
    // The drawn edge's real motion follows the pan: 2 px per field, measured
    // at the middle of the edge (translation plus tilt there).
    auto motionAt = [&](const FieldOffsets& f) { return double(f.track.refs[1].a) + double(f.track.refs[1].b) * 240.0; };
    const double base = motionAt(r.fields[0]);
    double worst = 0;
    for (const FieldOffsets& f : r.fields) worst = std::max(worst, std::fabs(motionAt(f) - base - 2.0 * double(f.field)));
    // The motion also soaks up each field's average timebase error, which the
    // look-ahead smoothing mostly removes; the output never sees it here,
    // since the border carries the correction.
    std::printf("  motion tracks the pan to within %.3f px\n", worst);
    CHECK_MSG(worst < 0.5, "motion off by %.3f", worst);
    // The pan survives in the output: no field's correction carries any of it.
    const std::vector<double> pf = perFieldError(r, d, res.bias, 0, 485);
    double worstPf = 0;
    for (double v : pf) worstPf = std::max(worstPf, std::fabs(v));
    std::printf("  worst per-field mean error %.4f px\n", worstPf);
    CHECK_MSG(worstPf < 0.2, "pan leaked: %.3f px", worstPf);
}

TEST_CASE("drawn edge alone: the pan survives within 0.2 px") {
    const SynthScene s = panScene();
    const DamageModel d;
    const int64_t frames = 20;
    TrackerConfig c;
    c.width = s.width;
    c.frameHeight = s.height;
    c.refs.push_back(drawnOn(s, d, frames, 1));
    const SynthRun r = runTracker(s, d, c, frames);
    const Residual res = residualOf(r, d);
    std::printf("  drawn only: rms %.4f px over %d of %d lines, bias %.3f\n", res.rmsMeasured, res.measured, res.lines, res.bias);
    CHECK_MSG(res.rmsMeasured < 0.5, "jitter not removed: %.3f", res.rmsMeasured);
    // If the pan leaked into the correction, the per-field error would grow by 2 px a field.
    const std::vector<double> pf = perFieldError(r, d, res.bias, 60, 420);
    double worst = 0;
    for (size_t i = 0; i < pf.size(); ++i) worst = std::max(worst, std::fabs(pf[i]));
    std::printf("  worst per-field mean error %.3f px\n", worst);
    CHECK_MSG(worst < 0.2, "pan leaked: %.3f px", worst);
}

TEST_MAIN()

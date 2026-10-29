// SPDX-License-Identifier: GPL-3.0-or-later
// QTGMC through the bundled VapourSynth: loads in-process, renders at Draft
// and Slower, at both rates, with the right frame count and field order.
#include "core/Synth.h"
#include "testing.h"

#include "core/Qtgmc.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>

using namespace ud;

namespace {

// A bar that moves 6 px per field, so each field's time shows in its position.
SynthScene movingBar() {
    SynthScene s;
    s.bars = {};
    s.panBar = true;
    s.panX = 200;
    s.panW = 40;
    s.panPerField = 6;
    return s;
}

double barLeft(const AVFrame* f, int y) {
    // Left edge of the bright bar on line y, in px, from the 16-bit luma.
    const uint16_t* row = reinterpret_cast<const uint16_t*>(f->data[0] + size_t(y) * size_t(f->linesize[0]));
    const double mid = (110.0 + 225.0) * 0.5 * 256.0;
    for (int x = 100; x < f->width - 1; ++x)
        if (row[x] < mid && row[x + 1] >= mid) return x + (mid - row[x]) / double(row[x + 1] - row[x]);
    return -1;
}

}  // namespace

TEST_CASE("QTGMC runs in-process") {
    const VsRuntimeInfo& rt = vapoursynthRuntime();
    std::printf("  VapourSynth: %s\n  python %s\n  vsscript %s\n", rt.ok ? rt.version.c_str() : rt.error.c_str(), rt.python.c_str(),
                rt.vsscript.c_str());
    if (!rt.ok && std::getenv("UINDUB_SKIP_QTGMC")) {
        std::printf("  skipped: UINDUB_SKIP_QTGMC is set\n");
        return;
    }
    CHECK_MSG(rt.ok, "%s", rt.error.c_str());
    if (!rt.ok) return;
    const SynthScene s = movingBar();
    const DamageModel none = DamageModel::none();
    const int64_t frames = 12;
    for (const char* preset : {"Draft", "Slower"}) {
        for (bool dbl : {true, false}) {
            for (FieldOrder order : {FieldOrder::TFF, FieldOrder::BFF}) {
                QtgmcConfig c;
                c.preset = preset;
                c.doubleRate = dbl;
                c.tff = order == FieldOrder::TFF;
                c.width = s.width;
                c.height = s.height;
                QtgmcRunner q;
                std::string err;
                const bool ok = q.open(c, frames, [&](int64_t n) {
                    Picture pic;
                    renderFrame(s, none, n, order, pic);
                    FramePtr f = fromPicture(pic, AV_PIX_FMT_YUV444P16, true);
                    setFrameInterlace(f.get(), true, order == FieldOrder::TFF);
                    return f;
                }, err);
                CHECK_MSG(ok, "%s", err.c_str());
                if (!ok) continue;
                CHECK_EQ(q.outputFrames(), dbl ? frames * 2 : frames);
                // Output frame k shows temporal field k (double rate) or 2k.
                double worst = 0;
                int got = 0;
                for (int64_t k = 0; k < q.outputFrames(); ++k) {
                    FramePtr f = q.get(k, err);
                    CHECK_MSG(f != nullptr, "%s", err.c_str());
                    if (!f) break;
                    ++got;
                    CHECK_EQ(f->width, s.width);
                    CHECK_EQ(f->height, s.height);
                    if (k < 2 || k >= q.outputFrames() - 2) continue;  // QTGMC edges
                    const int64_t field = dbl ? k : 2 * k;
                    const double expect = s.panX + s.panPerField * double(field);
                    const double at = barLeft(f.get(), 240);
                    worst = std::max(worst, std::fabs(at - expect));
                }
                std::printf("  %s %s %s: %d frames, bar off by at most %.2f px\n", preset, dbl ? "double" : "single",
                            fieldOrderName(order), got, worst);
                CHECK_EQ(int64_t(got), q.outputFrames());
                CHECK_MSG(worst < 2.0, "field order or timing wrong: %.2f px", worst);
            }
        }
    }
}

TEST_MAIN()

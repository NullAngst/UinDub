// SPDX-License-Identifier: GPL-3.0-or-later
// Each color control against known values in float; legalize keeps output
// in 16-235; auto levels hits 16 and 235 on a ramp.
#include "testing.h"

#include "core/Color.h"

#include <cmath>
#include <cstdio>

using namespace ud;

namespace {

Picture flat(double y, double cb = 128, double cr = 128, int w = 64, int h = 8) {
    Picture p;
    p.alloc(w, h, false);
    for (float& v : p.p[0].d) v = float(y);
    for (float& v : p.p[1].d) v = float(cb);
    for (float& v : p.p[2].d) v = float(cr);
    return p;
}

bool near(double a, double b, double tol = 0.01) { return std::fabs(a - b) <= tol; }

}  // namespace

TEST_CASE("each control on known values") {
    {
        Picture p = flat(100);
        ColorSettings c;
        c.brightness = 10;
        applyColor(p, c);
        CHECK(near(p.p[0].d[0], 110));
    }
    {
        Picture p = flat(100);
        ColorSettings c;
        c.contrast = 2;
        applyColor(p, c);
        CHECK(near(p.p[0].d[0], 184));  // 16 + 84 * 2
    }
    {
        Picture p = flat(16 + 219 * 0.25);
        ColorSettings c;
        c.gamma = 2;
        applyColor(p, c);
        CHECK(near(p.p[0].d[0], 16 + 219 * 0.5));
        Picture b = flat(16), wh = flat(235);
        applyColor(b, c);
        applyColor(wh, c);
        CHECK(near(b.p[0].d[0], 16));  // black and white held
        CHECK(near(wh.p[0].d[0], 235));
    }
    {
        ColorSettings c;
        c.blackPoint = 30;
        c.whitePoint = 200;
        Picture a = flat(30), b = flat(200), m = flat(115);
        applyColor(a, c);
        applyColor(b, c);
        applyColor(m, c);
        CHECK(near(a.p[0].d[0], 16));
        CHECK(near(b.p[0].d[0], 235));
        CHECK(near(m.p[0].d[0], 16 + 219 * 0.5));
    }
    {
        Picture p = flat(100, 160, 96);
        ColorSettings c;
        c.saturation = 0.5;
        applyColor(p, c);
        CHECK(near(p.p[1].d[0], 144));
        CHECK(near(p.p[2].d[0], 112));
    }
    {
        Picture p = flat(100, 160, 128);
        ColorSettings c;
        c.hue = 30;
        applyColor(p, c);
        CHECK(near(p.p[1].d[0], 128 + 32 * std::cos(M_PI / 6), 0.02));
        CHECK(near(p.p[2].d[0], 128 + 32 * std::sin(M_PI / 6), 0.02));
    }
    {
        // Gain on all three channels scales a gray, chroma stays neutral.
        Picture p = flat(16 + 219 * 0.5);
        ColorSettings c;
        c.gain = {0.5, 0.5, 0.5};
        applyColor(p, c);
        CHECK(near(p.p[0].d[0], 16 + 219 * 0.25, 0.05));
        CHECK(near(p.p[1].d[0], 128, 0.05));
        CHECK(near(p.p[2].d[0], 128, 0.05));
        // Red gain only: a gray turns red, Cr goes up.
        Picture q = flat(16 + 219 * 0.5);
        ColorSettings r;
        r.gain = {1.2, 1, 1};
        applyColor(q, r);
        CHECK(q.p[2].d[0] > 130);
    }
    {
        // Lift raises black; gamma per channel lifts mids.
        Picture p = flat(16);
        ColorSettings c;
        c.lift = {0.1, 0.1, 0.1};
        applyColor(p, c);
        CHECK(near(p.p[0].d[0], 16 + 219 * 0.1, 0.05));
    }
    {
        // Chroma delay moves a chroma step 2 px right and 1 line down.
        Picture p = flat(100, 128, 128, 64, 8);
        for (int y = 0; y < 8; ++y)
            for (int x = 32; x < 64; ++x) p.p[1].row(y)[x] = 180;
        for (int x = 0; x < 64; ++x) p.p[2].row(4)[x] = 200;
        ColorSettings c;
        c.chromaDelayX = 2;
        c.chromaDelayY = 1;
        applyColor(p, c);
        CHECK(near(p.p[1].row(2)[33], 128, 3.0));
        CHECK(near(p.p[1].row(2)[34] + p.p[1].row(2)[33], 128 + 128 + 52 * 0.5 * 2, 30.0));
        CHECK(p.p[1].row(2)[36] > 175);
        CHECK(near(p.p[2].row(5)[10], 200, 0.5));
        CHECK(near(p.p[2].row(4)[10], 128, 0.5));
        // Luma untouched.
        CHECK(near(p.p[0].row(3)[40], 100));
    }
    {
        // Blur keeps the chroma sum and spreads an impulse.
        Picture p = flat(100, 128, 128, 64, 1);
        p.p[1].row(0)[32] = 228;
        ColorSettings c;
        c.chromaBlur = 1.5;
        applyColor(p, c);
        double sum = 0;
        for (int x = 0; x < 64; ++x) sum += p.p[1].row(0)[x] - 128;
        CHECK(near(sum, 100, 0.5));
        CHECK(p.p[1].row(0)[32] < 228 - 50);
        CHECK(p.p[1].row(0)[31] > 140);
    }
}

TEST_CASE("legalize keeps output in range and the clip share counts") {
    Picture p = flat(200, 200, 60);
    ColorSettings c;
    c.contrast = 2;  // 200 -> 384
    c.saturation = 2;
    ColorStats st;
    applyColor(p, c, &st);
    for (float v : p.p[0].d) CHECK(v >= 16 && v <= 235);
    for (float v : p.p[1].d) CHECK(v >= 16 && v <= 240);
    for (float v : p.p[2].d) CHECK(v >= 16 && v <= 240);
    CHECK(near(st.clipped, 1.0));
    Picture q = flat(100);
    ColorSettings mild;
    mild.brightness = 5;
    applyColor(q, mild, &st);
    CHECK(near(st.clipped, 0.0));
    CHECK(ColorSettings{}.neutral());
    CHECK(!mild.neutral());
}

TEST_CASE("auto levels on a ramp") {
    Picture p;
    p.alloc(256, 64, false);
    for (int y = 0; y < 64; ++y)
        for (int x = 0; x < 256; ++x) p.p[0].row(y)[x] = float(30 + (200 - 30) * x / 255.0);
    for (int c = 1; c <= 2; ++c)
        for (float& v : p.p[c].d) v = 128;
    LevelHistogram h;
    h.add(p, 0, 0);
    double b = 0, w = 0;
    CHECK(h.suggest(b, w));
    std::printf("  suggested black %.0f white %.0f\n", b, w);
    CHECK(near(b, 30, 1.0));
    CHECK(near(w, 200, 1.0));
    ColorSettings c;
    c.blackPoint = b;
    c.whitePoint = w;
    applyColor(p, c);
    float lo = 255, hi = 0;
    for (float v : p.p[0].d) {
        lo = std::min(lo, v);
        hi = std::max(hi, v);
    }
    CHECK(near(lo, 16, 1.5));
    CHECK(near(hi, 235, 1.5));
}

TEST_MAIN()

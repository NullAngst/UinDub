// SPDX-License-Identifier: GPL-3.0-or-later
// Lanczos-3 shift by 0.25, 0.5, 0.75 px against a known band-limited ramp:
// error under 0.5 code.
#include "testing.h"

#include "core/Simd.h"

#include <cmath>
#include <cstdio>
#include <vector>

using namespace ud;

namespace {
// The known band-limited ramp: black to white over 24 px, its corners
// rounded by a Gaussian (sigma 2 px) so nothing in it reaches Nyquist.
double ramp(double x) {
    // A linear ramp convolved with a Gaussian, in closed form.
    const double s = 2.0, a = 348.0, b = 372.0;
    auto integ = [&](double u) {  // integral of the Gaussian CDF
        return u * 0.5 * (1.0 + std::erf(u / (s * M_SQRT2))) + s * std::exp(-u * u / (2 * s * s)) / std::sqrt(2 * M_PI);
    };
    return 16.0 + 219.0 * (integ(x - a) - integ(x - b)) / (b - a);
}
// The steepest edge a tape gives: full swing with sigma 1.6 px. Lanczos-3
// lands within a code of it; the 0.5 code bound is for the ramp above.
double edge(double x) { return 16.0 + 219.0 * 0.5 * (1.0 + std::erf((x - 360.0) / (1.6 * M_SQRT2))); }
}  // namespace

TEST_CASE("sub-pixel shifts") {
    const int w = 720;
    std::vector<float> src(static_cast<size_t>(w)), dst(static_cast<size_t>(w));
    for (int x = 0; x < w; ++x) src[size_t(x)] = float(ramp(x));
    for (bool simd : {false, true}) {
        if (simd && !cpuHasAvx2()) continue;
        setSimdEnabled(simd);
        for (double s : {0.25, 0.5, 0.75, -0.25, -0.5, 1.75, -3.5}) {
            shiftRow(src.data(), dst.data(), w, s, 16.0f, true);
            double worst = 0;
            for (int x = 8; x < w - 8; ++x) worst = std::max(worst, std::fabs(dst[size_t(x)] - ramp(x + s)));
            std::printf("  %s shift %+.2f: ramp worst %.4f codes\n", simd ? "avx2" : "plain", s, worst);
            CHECK_MSG(worst < 0.5, "shift %.2f off by %.3f", s, worst);
            std::vector<float> e(static_cast<size_t>(w)), ed(static_cast<size_t>(w));
            for (int x = 0; x < w; ++x) e[size_t(x)] = float(edge(x));
            shiftRow(e.data(), ed.data(), w, s, 16.0f, true);
            double worstEdge = 0;
            for (int x = 8; x < w - 8; ++x) worstEdge = std::max(worstEdge, std::fabs(ed[size_t(x)] - edge(x + s)));
            CHECK_MSG(worstEdge < 1.0, "steep edge, shift %.2f off by %.3f", s, worstEdge);
            // The warp with no slope is the same shift.
            std::vector<float> w2(static_cast<size_t>(w));
            warpRow(src.data(), w2.data(), w, s, 0.0, 360.0, 16.0f, true);
            double diff = 0;
            for (int x = 0; x < w; ++x) diff = std::max(diff, double(std::fabs(w2[size_t(x)] - dst[size_t(x)])));
            CHECK(diff < 1e-3);
        }
    }
    setSimdEnabled(true);
    // A zero shift returns the row.
    shiftRow(src.data(), dst.data(), w, 0.0, 16.0f, true);
    double z = 0;
    for (int x = 0; x < w; ++x) z = std::max(z, double(std::fabs(dst[size_t(x)] - src[size_t(x)])));
    CHECK(z < 1e-4);
}

TEST_MAIN()

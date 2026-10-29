// SPDX-License-Identifier: GPL-3.0-or-later
#include "Color.h"

#include "Simd.h"
#include "shared/Percentile.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

namespace ud {

bool ColorSettings::neutral() const { return *this == ColorSettings{}; }

bool ColorSettings::operator==(const ColorSettings& o) const {
    return brightness == o.brightness && contrast == o.contrast && gamma == o.gamma && blackPoint == o.blackPoint &&
           whitePoint == o.whitePoint && saturation == o.saturation && hue == o.hue && lift == o.lift && gammaRgb == o.gammaRgb &&
           gain == o.gain && chromaDelayX == o.chromaDelayX && chromaDelayY == o.chromaDelayY && chromaBlur == o.chromaBlur;
}

namespace {

void blurRow(float* row, int w, double sigma, std::vector<float>& tmp) {
    if (sigma <= 0.05) return;
    const int r = std::max(1, int(std::ceil(3 * sigma)));
    std::vector<float> k(size_t(2 * r + 1));
    double sum = 0;
    for (int i = -r; i <= r; ++i) {
        k[size_t(i + r)] = float(std::exp(-0.5 * i * i / (sigma * sigma)));
        sum += k[size_t(i + r)];
    }
    for (float& v : k) v = float(v / sum);
    tmp.assign(row, row + w);
    for (int x = 0; x < w; ++x) {
        float s = 0;
        for (int i = -r; i <= r; ++i) s += k[size_t(i + r)] * tmp[size_t(std::clamp(x + i, 0, w - 1))];
        row[x] = s;
    }
}

}  // namespace

void legalize(Picture& pic) {
    for (float& v : pic.p[0].d) v = std::clamp(v, 16.0f, 235.0f);
    if (pic.gray) return;
    for (int c = 1; c <= 2; ++c)
        for (float& v : pic.p[c].d) v = std::clamp(v, 16.0f, 240.0f);
}

void applyColor(Picture& pic, const ColorSettings& c, ColorStats* stats) {
    const int w = pic.width, h = pic.height;
    const bool chroma = !pic.gray;
    std::vector<float> tmp(static_cast<size_t>(w));
    // Legal before the settings touch it, so the clip share counts only what they did.
    std::vector<uint8_t> wasLegal;
    if (stats) {
        wasLegal.resize(size_t(w) * size_t(h));
        for (size_t i = 0; i < wasLegal.size(); ++i) {
            bool ok = pic.p[0].d[i] >= 16.0f && pic.p[0].d[i] <= 235.0f;
            if (chroma) ok = ok && pic.p[1].d[i] >= 16.0f && pic.p[1].d[i] <= 240.0f && pic.p[2].d[i] >= 16.0f && pic.p[2].d[i] <= 240.0f;
            wasLegal[i] = ok;
        }
    }

    // VHS chroma lags luma: line it back up, then soften it if asked.
    if (chroma && (c.chromaDelayX != 0 || c.chromaDelayY != 0 || c.chromaBlur > 0)) {
        for (int p = 1; p <= 2; ++p) {
            Plane& P = pic.p[p];
            if (c.chromaDelayX != 0)
                for (int y = 0; y < h; ++y) {
                    shiftRow(P.row(y), tmp.data(), w, -c.chromaDelayX, 128.0f, true);
                    std::memcpy(P.row(y), tmp.data(), sizeof(float) * size_t(w));
                }
            if (c.chromaDelayY != 0) {
                Plane src = P;
                const double dy = c.chromaDelayY;
                for (int y = 0; y < h; ++y) {
                    const double sy = y - dy;
                    const int y0 = int(std::floor(sy));
                    const float t = float(sy - y0);
                    const float* a = src.row(std::clamp(y0, 0, h - 1));
                    const float* b = src.row(std::clamp(y0 + 1, 0, h - 1));
                    float* o = P.row(y);
                    for (int x = 0; x < w; ++x) o[x] = a[x] * (1 - t) + b[x] * t;
                }
            }
            if (c.chromaBlur > 0)
                for (int y = 0; y < h; ++y) blurRow(P.row(y), w, c.chromaBlur, tmp);
        }
    }

    const double bp = c.blackPoint, wp = std::max(c.blackPoint + 1.0, c.whitePoint);
    const bool levels = bp != 16 || wp != 235;
    const double ig = 1.0 / std::max(0.05, c.gamma);
    const double hr = c.hue * M_PI / 180.0, ch = std::cos(hr), sh = std::sin(hr);
    const bool cdl = c.lift != std::array<double, 3>{0, 0, 0} || c.gammaRgb != std::array<double, 3>{1, 1, 1} ||
                     c.gain != std::array<double, 3>{1, 1, 1};
    std::array<double, 3> ign{};
    for (int i = 0; i < 3; ++i) ign[size_t(i)] = 1.0 / std::max(0.05, c.gammaRgb[size_t(i)]);
    float* Y = pic.p[0].d.data();
    float* U = chroma ? pic.p[1].d.data() : nullptr;
    float* V = chroma ? pic.p[2].d.data() : nullptr;
    const size_t n = size_t(w) * size_t(h);
    for (size_t i = 0; i < n; ++i) {
        double y = Y[i];
        if (levels) y = 16.0 + (y - bp) * 219.0 / (wp - bp);
        y += c.brightness;
        y = 16.0 + (y - 16.0) * c.contrast;
        if (c.gamma != 1) {
            const double t = (y - 16.0) / 219.0;
            if (t > 0 && t < 1) y = 16.0 + 219.0 * std::pow(t, ig);
        }
        if (!chroma) {
            Y[i] = float(y);
            continue;
        }
        double u = U[i] - 128.0, v = V[i] - 128.0;
        if (c.saturation != 1) {
            u *= c.saturation;
            v *= c.saturation;
        }
        if (c.hue != 0) {
            const double uu = u * ch - v * sh, vv = u * sh + v * ch;
            u = uu;
            v = vv;
        }
        if (cdl) {
            // BT.601 limited to normalized RGB, per-channel lift / gamma / gain, back.
            const double yy = (y - 16.0) / 219.0, pb = u / 224.0, pr = v / 224.0;
            double rgb[3] = {yy + 1.402 * pr, yy - 0.344136 * pb - 0.714136 * pr, yy + 1.772 * pb};
            for (int k = 0; k < 3; ++k) {
                double x = rgb[k];
                x = c.gain[size_t(k)] * (x + c.lift[size_t(k)] * (1.0 - x));
                if (c.gammaRgb[size_t(k)] != 1 && x > 0) x = std::pow(x, ign[size_t(k)]);
                rgb[k] = x;
            }
            const double ny = 0.299 * rgb[0] + 0.587 * rgb[1] + 0.114 * rgb[2];
            y = 16.0 + 219.0 * ny;
            u = 224.0 * (rgb[2] - ny) / 1.772;
            v = 224.0 * (rgb[0] - ny) / 1.402;
        }
        Y[i] = float(y);
        U[i] = float(128.0 + u);
        V[i] = float(128.0 + v);
    }
    if (stats) {
        uint64_t pushed = 0;
        for (size_t i = 0; i < n; ++i) {
            if (!wasLegal[i]) continue;
            bool ok = Y[i] >= 15.5f && Y[i] <= 235.5f;
            if (chroma) ok = ok && U[i] >= 15.5f && U[i] <= 240.5f && V[i] >= 15.5f && V[i] <= 240.5f;
            pushed += !ok;
        }
        stats->clipped = n ? double(pushed) / double(n) : 0.0;
    }
    legalize(pic);
}

void LevelHistogram::add(const Picture& pic, int border, int bottom) {
    const int x0 = std::min(border, pic.width / 4), x1 = std::max(x0 + 1, pic.width - border);
    const int y1 = std::max(1, pic.height - bottom);
    for (int y = 0; y < y1; ++y) {
        const float* row = pic.p[0].row(y);
        for (int x = x0; x < x1; ++x) {
            ++luma[size_t(std::clamp(int(std::lround(row[x])), 0, 255))];
            ++total;
        }
    }
}

bool LevelHistogram::suggest(double& black, double& white) const {
    if (!total) return false;
    const int b = percentile(luma, total, 0.001), w = percentile(luma, total, 0.999);
    if (b < 0 || w <= b + 10) return false;
    black = b;
    white = w;
    return true;
}

}  // namespace ud

// SPDX-License-Identifier: GPL-3.0-or-later
// Edge Lock measurement: templates, per-line matching, border finding.
#include "EdgeLock.h"
#include "Simd.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numeric>

namespace ud {

namespace {
constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();
constexpr double kMinStep = 4.0;  // codes; below this an edge is not there to measure

double median(std::vector<double> v) {
    if (v.empty()) return 0;
    const size_t m = v.size() / 2;
    std::nth_element(v.begin(), v.begin() + long(m), v.end());
    return v[m];
}
}  // namespace

bool EdgeLockSettings::operator==(const EdgeLockSettings& o) const {
    return searchRange == o.searchRange && topLines == o.topLines && topRange == o.topRange && templateWidth == o.templateWidth &&
           confidence == o.confidence && maxGap == o.maxGap && motionSmoothing == o.motionSmoothing && strength == o.strength &&
           fillRepeat == o.fillRepeat && lengthCorrection == o.lengthCorrection;
}

const RefSpan* Reference::spanAt(int64_t frame) const {
    for (const RefSpan& s : spans)
        if (frame >= s.start && frame < s.end) return &s;
    return nullptr;
}

EdgeTemplate makeTemplate(const std::vector<float>& profile, double step) {
    EdgeTemplate t;
    t.step = step;
    if (profile.size() < 8) return t;
    double mean = 0;
    for (float v : profile) mean += v;
    mean /= double(profile.size());
    double norm = 0;
    t.t.resize(profile.size());
    for (size_t i = 0; i < profile.size(); ++i) {
        t.t[i] = float(profile[i] - mean);
        norm += double(t.t[i]) * t.t[i];
    }
    norm = std::sqrt(norm);
    if (norm < 1e-6) {
        t.t.clear();
        return t;
    }
    for (float& v : t.t) v = float(v / norm);
    return t;
}

void gradientRow(const float* y, int w, float* g) {
    if (w < 3) {
        for (int x = 0; x < w; ++x) g[x] = 0;
        return;
    }
    g[0] = 0;
    g[w - 1] = 0;
    for (int x = 1; x < w - 1; ++x) g[x] = 0.5f * (y[x + 1] - y[x - 1]);
}

void sampleProfile(const float* grad, int w, double center, int width, float* out) {
    const int half = width / 2;
    for (int k = 0; k < width; ++k) {
        const double x = center - half + k;
        const double f = std::floor(x);
        const int i = int(f);
        const double t = x - f;
        auto at = [&](int j) { return j >= 0 && j < w ? grad[j] : 0.0f; };
        out[k] = float(at(i) * (1.0 - t) + at(i + 1) * t);
    }
}

double stepAt(const float* luma, int w, double pos) {
    const int p = int(std::lround(pos));
    double l = 0, r = 0;
    int nl = 0, nr = 0;
    for (int d = 2; d <= 5; ++d) {
        if (p - d >= 0) {
            l += luma[p - d];
            ++nl;
        }
        if (p + d < w) {
            r += luma[p + d];
            ++nr;
        }
    }
    if (!nl || !nr) return 0;
    return std::fabs(r / nr - l / nl);
}

LineMeasure measureLine(const float* luma, const float* grad, int w, const EdgeTemplate& t, double center, int range, bool preferCenter) {
    LineMeasure m;
    const int T = t.width();
    if (!t.valid() || !std::isfinite(center)) return m;
    const int half = T / 2;
    const int xb = int(std::lround(center));
    const int n = 2 * range + 1;
    // NCC at every whole shift; the template has zero mean and unit norm.
    double ncc[2 * 64 + 1];
    if (n > int(sizeof ncc / sizeof ncc[0]) || T > 64) return m;
    // The gradient around the search, zero past the ends of the row: the
    // blanking edge often sits closer to the left side than half a template.
    float pad[2 * 64 + 1 + 64 + 2];
    const int base = xb - range - half;
    for (int k = 0; k < n + T; ++k) {
        const int x = base + k;
        pad[k] = x >= 1 && x < w - 1 ? grad[x] : 0.0f;
    }
    for (int i = 0; i < n; ++i) {
        const int s = i - range;
        const int ws = xb + s - half;
        if (ws + T <= 0 || ws >= w) {
            ncc[i] = -2;
            continue;
        }
        const float* g = pad + (ws - base);
        double sum = 0, sum2 = 0;
        for (int k = 0; k < T; ++k) {
            sum += g[k];
            sum2 += double(g[k]) * g[k];
        }
        const double var = sum2 - sum * sum / T;
        if (var < 1e-6) {
            ncc[i] = 0;
            continue;
        }
        ncc[i] = double(dot(t.t.data(), g, T)) / std::sqrt(var);
    }
    int best = -1;
    for (int i = 0; i < n; ++i)
        if (best < 0 || ncc[i] > ncc[best]) best = i;
    if (best < 0 || ncc[best] <= -1.5) return m;
    if (preferCenter) {
        // A wide search takes the near-best peak closest to where the edge was.
        for (int i = 0; i < n; ++i) {
            const bool localMax = (i == 0 || ncc[i] >= ncc[i - 1]) && (i == n - 1 || ncc[i] >= ncc[i + 1]);
            if (localMax && ncc[i] >= ncc[best] - 0.05 && std::abs(i - range) < std::abs(best - range)) best = i;
        }
    }
    double second = -1;
    for (int i = 0; i < n; ++i) {
        if (std::abs(i - best) < 3) continue;
        const bool localMax = (i == 0 || ncc[i] >= ncc[i - 1]) && (i == n - 1 || ncc[i] >= ncc[i + 1]);
        if (localMax) second = std::max(second, ncc[i]);
    }
    double delta = 0;
    if (best > 0 && best < n - 1 && ncc[best - 1] > -1.5 && ncc[best + 1] > -1.5) {
        const double a = ncc[best - 1], b = ncc[best], c = ncc[best + 1];
        const double den = a - 2 * b + c;
        if (den < -1e-9) delta = std::clamp(0.5 * (a - c) / den, -0.5, 0.5);
    }
    m.peak = ncc[best];
    m.pos = xb + (best - range) + delta;
    m.clamped = std::abs(best - range) >= range;
    m.step = stepAt(luma, w, m.pos);
    {
        // Level and texture of the window, to tell a dark scene from an occluder.
        double sl = 0, sg = 0;
        int nl = 0;
        for (int k = -half; k < half; ++k) {
            const int x = int(std::lround(m.pos)) + k;
            if (x < 0 || x >= w) continue;
            sl += luma[x];
            sg += std::fabs(grad[x]);
            ++nl;
        }
        m.level = nl ? sl / nl : 0.0;
        m.flat = nl ? sg / nl < 0.75 : true;
    }
    m.contrast = m.step >= kMinStep;
    const double p1 = std::max(0.0, m.peak);
    const double amb = second < 0 ? 1.0 : std::clamp((p1 - second) / 0.25, 0.0, 1.0);
    const double cref = std::max(6.0, 0.5 * t.step);
    const double c = m.contrast ? std::clamp(m.step / cref, 0.0, 1.0) : 0.0;
    m.conf = m.clamped ? 0.0 : p1 * amb * c;
    return m;
}

TemplateBuild buildTemplate(const Picture& frame, const std::vector<double>& xs, int width, int range, double minConf) {
    TemplateBuild b;
    const int w = frame.width, h = frame.height;
    b.found.assign(size_t(h), kNaN);
    std::vector<float> grad(static_cast<size_t>(w)), prof(static_cast<size_t>(width));
    std::vector<int> lines;
    for (int y = 0; y < h && y < int(xs.size()); ++y)
        if (std::isfinite(xs[size_t(y)])) lines.push_back(y);
    if (lines.size() < 8) return b;
    // Gradients once.
    std::vector<float> grads(size_t(w) * lines.size());
    for (size_t i = 0; i < lines.size(); ++i) gradientRow(frame.p[0].row(lines[i]), w, grads.data() + i * size_t(w));

    std::vector<double> at(lines.size());
    for (size_t i = 0; i < lines.size(); ++i) at[i] = xs[size_t(lines[i])];
    std::vector<bool> use(lines.size(), true);
    for (int iter = 0; iter < 3; ++iter) {
        std::vector<double> acc(size_t(width), 0.0);
        double stepSum = 0;
        int count = 0;
        for (size_t i = 0; i < lines.size(); ++i) {
            if (!use[i]) continue;
            sampleProfile(grads.data() + i * size_t(w), w, at[i], width, prof.data());
            for (int k = 0; k < width; ++k) acc[size_t(k)] += prof[size_t(k)];
            stepSum += stepAt(frame.p[0].row(lines[i]), w, at[i]);
            ++count;
        }
        if (count < 8) return b;
        b.profile.assign(size_t(width), 0.0f);
        for (int k = 0; k < width; ++k) b.profile[size_t(k)] = float(acc[size_t(k)] / count);
        b.step = stepSum / count;
        if (iter == 2) break;
        const EdgeTemplate t = makeTemplate(b.profile, b.step);
        if (!t.valid()) return b;
        // The first pass uses a template blurred by the jitter, so it asks for less.
        const double need = iter == 0 ? minConf * 0.5 : minConf;
        int good = 0;
        for (size_t i = 0; i < lines.size(); ++i) {
            const LineMeasure m = measureLine(frame.p[0].row(lines[i]), grads.data() + i * size_t(w), w, t, xs[size_t(lines[i])], range);
            use[i] = m.conf >= need;
            if (use[i]) {
                at[i] = m.pos;
                ++good;
            } else {
                at[i] = xs[size_t(lines[i])];
            }
        }
        if (good < 8) return b;
    }
    // Final positions with the finished template.
    const EdgeTemplate t = makeTemplate(b.profile, b.step);
    std::vector<double> offs;
    int good = 0;
    for (size_t i = 0; i < lines.size(); ++i) {
        const LineMeasure m = measureLine(frame.p[0].row(lines[i]), grads.data() + i * size_t(w), w, t, xs[size_t(lines[i])], range);
        if (m.conf >= minConf) {
            b.found[size_t(lines[i])] = m.pos;
            offs.push_back(m.pos - xs[size_t(lines[i])]);
            ++good;
        }
    }
    b.medianOffset = median(offs);
    b.ok = good >= 8 && good * 10 >= int(lines.size()) * 3;
    return b;
}

BorderFind findBorders(const Picture& frame, const EdgeLockSettings& s) {
    BorderFind r;
    const int w = frame.width, h = frame.height;
    if (w < 64 || h < 16) return r;
    // Column means of the gradient and of luma.
    std::vector<double> colG(size_t(w), 0.0), colY(size_t(w), 0.0);
    std::vector<float> g(static_cast<size_t>(w));
    for (int y = 0; y < h; ++y) {
        const float* row = frame.p[0].row(y);
        gradientRow(row, w, g.data());
        for (int x = 0; x < w; ++x) {
            colG[size_t(x)] += g[size_t(x)];
            colY[size_t(x)] += row[x];
        }
    }
    for (int x = 0; x < w; ++x) {
        colG[size_t(x)] /= h;
        colY[size_t(x)] /= h;
    }
    auto darkRun = [&](int from, int to) {
        double sum = 0;
        int n = 0;
        for (int x = std::max(0, from); x <= std::min(w - 1, to); ++x) {
            sum += colY[size_t(x)];
            ++n;
        }
        return n ? sum / n : 255.0;
    };
    // Left: picture rising out of blanking.
    int bestL = -1;
    double scoreL = 0;
    for (int x = 6; x < w / 4; ++x) {
        if (darkRun(x - 10, x - 4) > 32) continue;
        if (colG[size_t(x)] > scoreL) {
            scoreL = colG[size_t(x)];
            bestL = x;
        }
    }
    int bestR = -1;
    double scoreR = 0;
    for (int x = w - w / 4; x < w - 6; ++x) {
        if (darkRun(x + 4, x + 10) > 32) continue;
        if (-colG[size_t(x)] > scoreR) {
            scoreR = -colG[size_t(x)];
            bestR = x;
        }
    }
    auto centroid = [&](int x0, double sign) {
        double sw = 0, sx = 0;
        for (int x = std::max(1, x0 - 6); x <= std::min(w - 2, x0 + 6); ++x) {
            const double v = std::max(0.0, sign * colG[size_t(x)]);
            sw += v;
            sx += v * x;
        }
        return sw > 0 ? sx / sw : double(x0);
    };
    const int range = std::max(s.searchRange, s.topRange);
    if (bestL >= 0 && scoreL >= 2.0) {
        const double x = centroid(bestL, 1.0);
        std::vector<double> xs(size_t(h), x);
        r.leftT = buildTemplate(frame, xs, s.templateWidth, range, s.confidence);
        if (r.leftT.ok) {
            r.left = true;
            std::vector<double> f;
            for (double v : r.leftT.found)
                if (std::isfinite(v)) f.push_back(v);
            r.leftX = median(f);
        }
    }
    if (bestR >= 0 && scoreR >= 2.0) {
        const double x = centroid(bestR, -1.0);
        std::vector<double> xs(size_t(h), x);
        r.rightT = buildTemplate(frame, xs, s.templateWidth, range, s.confidence);
        if (r.rightT.ok) {
            r.right = true;
            std::vector<double> f;
            for (double v : r.rightT.found)
                if (std::isfinite(v)) f.push_back(v);
            r.rightX = median(f);
        }
    }
    return r;
}

double spanX(const RefSpan& s, RefKind kind, double y) {
    if (s.points.empty()) return kNaN;
    if (kind != RefKind::Drawn) return s.points[0].x;
    if (s.points.size() < 2) return kNaN;
    if (y < s.points.front().y || y > s.points.back().y) return kNaN;
    for (size_t i = 1; i < s.points.size(); ++i) {
        const PointF& a = s.points[i - 1];
        const PointF& b = s.points[i];
        if (y <= b.y) {
            if (b.y - a.y < 1e-9) return b.x;
            const double t = (y - a.y) / (b.y - a.y);
            return a.x + (b.x - a.x) * t;
        }
    }
    return s.points.back().x;
}

void spanLines(const RefSpan& s, RefKind kind, int h, int& first, int& last) {
    if (kind != RefKind::Drawn || s.points.size() < 2) {
        first = 0;
        last = kind != RefKind::Drawn ? h - 1 : -1;
        return;
    }
    first = std::max(0, int(std::ceil(s.points.front().y)));
    last = std::min(h - 1, int(std::floor(s.points.back().y)));
}

bool makeBorderSpan(const Picture& frame, bool right, int64_t frames, const EdgeLockSettings& s, RefSpan& out) {
    const BorderFind b = findBorders(frame, s);
    if (right ? !b.right : !b.left) return false;
    out = RefSpan{};
    out.start = 0;
    out.end = frames;
    out.points = {{right ? b.rightX : b.leftX, 0}};
    out.templ = right ? b.rightT.profile : b.leftT.profile;
    out.templStep = right ? b.rightT.step : b.leftT.step;
    return true;
}

bool makeDrawnSpan(const Picture& frame, const std::vector<PointF>& ptsIn, int64_t frameIndex, int64_t end, const EdgeLockSettings& s,
                   RefSpan& out, std::string& err) {
    std::vector<PointF> pts = ptsIn;
    std::sort(pts.begin(), pts.end(), [](const PointF& a, const PointF& b) { return a.y < b.y; });
    if (pts.size() < 2 || pts.back().y - pts.front().y < 8) {
        err = "Draw along at least 8 lines of the edge.";
        return false;
    }
    RefSpan sp;
    sp.points = pts;
    const int h = frame.height;
    std::vector<double> xs(size_t(h), kNaN);
    for (int y = 0; y < h; ++y) xs[size_t(y)] = spanX(sp, RefKind::Drawn, y);
    TemplateBuild b = buildTemplate(frame, xs, s.templateWidth, s.searchRange, s.confidence);
    if (!b.ok) {
        err = "That edge does not have enough contrast to lock onto. Try a sharper edge, or a frame where it is clearer.";
        return false;
    }
    // The hand is off by a pixel or two: fit a line to where the edge really is
    // and move the drawing onto it, so motion starts at zero.
    double sw = 0, sy = 0, sr = 0, syy = 0, syr = 0;
    for (int y = 0; y < h; ++y) {
        if (!std::isfinite(b.found[size_t(y)]) || !std::isfinite(xs[size_t(y)])) continue;
        const double r = b.found[size_t(y)] - xs[size_t(y)];
        if (std::fabs(r - b.medianOffset) > 4) continue;
        sw += 1;
        sy += y;
        sr += r;
        syy += double(y) * y;
        syr += double(y) * r;
    }
    double a0 = b.medianOffset, a1 = 0;
    const double det = sw * syy - sy * sy;
    if (sw >= 8 && std::fabs(det) > 1e-9) {
        a1 = (sw * syr - sy * sr) / det;
        a0 = (sr - a1 * sy) / sw;
    }
    for (PointF& p : sp.points) p.x += a0 + a1 * p.y;
    sp.templ = b.profile;
    sp.templStep = b.step;
    sp.start = frameIndex;
    sp.end = end;
    sp.defFrame = frameIndex;
    out = sp;
    return true;
}

}  // namespace ud

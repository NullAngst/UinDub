// SPDX-License-Identifier: GPL-3.0-or-later
#include "EdgeTracker.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace ud {

namespace {

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();
constexpr int kLostFields = 10;        // fields with too few lines before a reference goes Lost
constexpr double kLostShare = 0.6;     // under 60% of lines measured counts toward Lost
constexpr double kDarkShare = 0.2;     // under 20% of lines with contrast: dark
constexpr double kCutHistogram = 0.4;  // histogram distance for a scene cut
constexpr double kCutShare = 0.25;     // a drawn edge under 25% measured at a cut lost its template
constexpr int kEaseFields = 8;         // dark and lost spans ease to zero over this many fields
constexpr double kSplineLambda = 16.0;
constexpr double kOutlierPx = 6.0;     // a line this far from its neighbors' median is a bad match
constexpr double kTukeyPx = 10.0;
constexpr double kMotionGuardPx = 4.0;  // largest field-to-field surprise in real motion
constexpr int kStableFields = 6;
constexpr double kRecoverStep[2] = {0.6, 1.4};  // step range, against the template, to come back from Lost
constexpr double kMinMedianPeak = 0.8;  // a drawn edge whose lines match worse than this is not the edge
constexpr double kDarkLevel = 40.0;     // codes: a window this dark with no edge is a dark scene        // fields an edge tracks before its velocity is trusted

struct MotionSample {
    int64_t field = 0;
    double a = 0, b = 0, q = 0;
    int seg = 0;
};

struct RefRun {
    int spanIdx = -1;
    int64_t endedSpanStart = std::numeric_limits<int64_t>::min();
    int64_t endedFrame = 0;
    RefState state = RefState::Inactive;
    int lostCount = 0;
    int seg = 0;
    bool wide = false;
    int stable = 0;          // fields tracked in a row since coming back
    bool recovering = false; // back from Lost or Dark: hold still until the edge proves itself
    std::deque<MotionSample> hist;
    bool haveHeld = false;
    double heldA = 0, heldB = 0;
};

struct PendRef {
    RefState state = RefState::Inactive;
    int covered = 0, measured = 0;
    double q = 0;
    int seg = 0;
    std::vector<double> r, conf, shape, peak;  // per field line; r NaN when not measured
};

struct Pending {
    int64_t field = 0;
    int parity = 0;
    int lines = 0, width = 0;
    uint8_t flags = 0;
    std::array<PendRef, kMaxReferences> refs;
};

// Weighted least squares line r = a + b * y.
bool fitLine(const std::vector<double>& y, const std::vector<double>& r, const std::vector<double>& w, double& a, double& b) {
    double sw = 0, sy = 0, sr = 0, syy = 0, syr = 0;
    for (size_t i = 0; i < y.size(); ++i) {
        if (w[i] <= 0) continue;
        sw += w[i];
        sy += w[i] * y[i];
        sr += w[i] * r[i];
        syy += w[i] * y[i] * y[i];
        syr += w[i] * y[i] * r[i];
    }
    if (sw <= 1e-9) return false;
    const double det = sw * syy - sy * sy;
    if (std::fabs(det) < 1e-9 * sw * sw) {
        a = sr / sw;
        b = 0;
        return true;
    }
    b = (sw * syr - sy * sr) / det;
    a = (sr - b * sy) / sw;
    return true;
}

// Lines that disagree with their neighbors by a lot are bad matches (an
// occluder's own edge, a dropout), whatever their correlation said.
void rejectSpatialOutliers(std::vector<double>& r, std::vector<double>& conf, int& measured) {
    const int n = int(r.size());
    std::vector<double> win;
    std::vector<int> bad;
    for (int l = 0; l < n; ++l) {
        if (!std::isfinite(r[size_t(l)])) continue;
        win.clear();
        for (int k = std::max(0, l - 4); k <= std::min(n - 1, l + 4); ++k)
            if (k != l && std::isfinite(r[size_t(k)])) win.push_back(r[size_t(k)]);
        if (win.size() < 3) continue;
        std::nth_element(win.begin(), win.begin() + long(win.size() / 2), win.end());
        if (std::fabs(r[size_t(l)] - win[win.size() / 2]) > kOutlierPx) bad.push_back(l);
    }
    for (int l : bad) {
        r[size_t(l)] = kNaN;
        conf[size_t(l)] = 0;
        --measured;
    }
}

double median(std::vector<double> v) {
    if (v.empty()) return 0;
    std::nth_element(v.begin(), v.begin() + long(v.size() / 2), v.end());
    return v[v.size() / 2];
}

double easeAt(int d, int maxGap) {
    // Full value up to half the max gap from a measured line, then a raised
    // cosine down to zero over 8 lines.
    const int full = std::max(1, maxGap / 2);
    if (d <= full) return 1.0;
    const double t = double(d - full) / 8.0;
    if (t >= 1.0) return 0.0;
    return 0.5 * (1.0 + std::cos(M_PI * t));
}

// Fills the lines with no measurement. Measured lines keep their own value.
void gapFill(std::vector<double>& v, std::vector<uint8_t>& cb, const std::vector<double>& w, int maxGap, std::vector<double>* smoothOut) {
    const int n = int(v.size());
    std::vector<double> z;
    std::vector<double> vv(v.size());
    for (int i = 0; i < n; ++i) vv[size_t(i)] = w[size_t(i)] > 0 ? v[size_t(i)] : 0.0;
    whittaker(vv, w, kSplineLambda, z);
    if (smoothOut) *smoothOut = z;
    int first = -1, last = -1;
    for (int i = 0; i < n; ++i)
        if (w[size_t(i)] > 0) {
            if (first < 0) first = i;
            last = i;
        }
    for (int i = 0; i < n; ++i) {
        if (w[size_t(i)] > 0) continue;
        if (i < first || i > last) {
            // Above the first or below the last measurement: hold the nearest, eased.
            const int near = i < first ? first : last;
            const double e = easeAt(std::abs(i - near), maxGap);
            v[size_t(i)] = z[size_t(near)] * e;
            cb[size_t(i)] = e > 0 ? kConfInterpolated : kConfNone;
            continue;
        }
        int a = i, b = i;
        while (a >= 0 && w[size_t(a)] <= 0) --a;
        while (b < n && w[size_t(b)] <= 0) ++b;
        const int gap = b - a - 1;
        if (gap <= maxGap) {
            v[size_t(i)] = z[size_t(i)];
            cb[size_t(i)] = kConfInterpolated;
        } else {
            const double e = easeAt(std::min(i - a, b - i), maxGap);
            v[size_t(i)] = z[size_t(i)] * e;
            cb[size_t(i)] = e > 0 ? kConfInterpolated : kConfNone;
        }
    }
}

}  // namespace

void whittaker(const std::vector<double>& v, const std::vector<double>& w, double lambda, std::vector<double>& z) {
    // (W + lambda D'D) z = W v, D the second difference. Pentadiagonal and
    // symmetric positive definite once two points have weight; solved by
    // banded elimination.
    const int n = int(v.size());
    z.assign(size_t(n), 0.0);
    if (n == 0) return;
    int nw = 0;
    for (double x : w) nw += x > 0;
    if (n < 3 || nw < 2) {
        double s = 0, sw = 0;
        for (int i = 0; i < n; ++i) {
            s += w[size_t(i)] * v[size_t(i)];
            sw += w[size_t(i)];
        }
        const double m = sw > 0 ? s / sw : 0.0;
        std::fill(z.begin(), z.end(), m);
        return;
    }
    // Band storage: A[i][k] for column i + k - 2, k = 0..4.
    std::vector<std::array<double, 5>> A(static_cast<size_t>(n));
    std::vector<double> rhs(static_cast<size_t>(n));
    for (int i = 0; i < n; ++i) {
        A[size_t(i)].fill(0.0);
        A[size_t(i)][2] = w[size_t(i)];
        rhs[size_t(i)] = w[size_t(i)] * v[size_t(i)];
    }
    // D'D: each row j of D is (1, -2, 1) at columns j, j+1, j+2.
    for (int j = 0; j + 2 < n; ++j) {
        const int c[3] = {j, j + 1, j + 2};
        const double d[3] = {1.0, -2.0, 1.0};
        for (int p = 0; p < 3; ++p)
            for (int q = 0; q < 3; ++q) A[size_t(c[p])][size_t(c[q] - c[p] + 2)] += lambda * d[p] * d[q];
    }
    // Forward elimination within the band.
    for (int i = 0; i < n; ++i) {
        const double piv = A[size_t(i)][2];
        if (std::fabs(piv) < 1e-12) continue;
        for (int r = i + 1; r <= std::min(n - 1, i + 2); ++r) {
            const double f = A[size_t(r)][size_t(i - r + 2)] / piv;
            if (f == 0) continue;
            for (int c = i; c <= std::min(n - 1, i + 2); ++c) A[size_t(r)][size_t(c - r + 2)] -= f * A[size_t(i)][size_t(c - i + 2)];
            rhs[size_t(r)] -= f * rhs[size_t(i)];
        }
    }
    for (int i = n - 1; i >= 0; --i) {
        double s = rhs[size_t(i)];
        for (int c = i + 1; c <= std::min(n - 1, i + 2); ++c) s -= A[size_t(i)][size_t(c - i + 2)] * z[size_t(c)];
        const double piv = A[size_t(i)][2];
        z[size_t(i)] = std::fabs(piv) > 1e-12 ? s / piv : 0.0;
    }
}

struct EdgeTracker::Impl {
    std::vector<std::vector<EdgeTemplate>> templ;
    std::array<RefRun, kMaxReferences> run;
    std::deque<Pending> pend;
    std::array<float, 64> prevHist{};
    bool havePrev = false;
    int sinceMeasured = 1000;
    std::vector<double> lastSmooth, lastSmoothSlope;
    int64_t lastResetFrame = -1;
};

EdgeTracker::EdgeTracker(const TrackerConfig& c) : m_c(c), m_i(std::make_unique<Impl>()) {
    if (m_c.refs.size() > size_t(kMaxReferences)) m_c.refs.resize(kMaxReferences);
    for (const Reference& r : m_c.refs) {
        std::vector<EdgeTemplate> ts;
        for (const RefSpan& s : r.spans) ts.push_back(makeTemplate(s.templ, s.templStep));
        m_i->templ.push_back(ts);
    }
}

EdgeTracker::~EdgeTracker() = default;

static int rangeFor(const EdgeLockSettings& s, int frameLine) { return frameLine < s.topLines ? s.topRange : s.searchRange; }

PreparedField EdgeTracker::prepare(int64_t field, FieldLuma&& luma) const {
    PreparedField p;
    p.field = field;
    p.parity = luma.parity;
    p.luma = std::move(luma);
    const int w = p.luma.width, lines = p.luma.lines;
    p.grad.resize(size_t(w) * size_t(lines));
    for (int l = 0; l < lines; ++l) gradientRow(p.luma.row(l), w, p.grad.data() + size_t(l) * size_t(w));
    double total = 0;
    for (int l = 0; l < lines; l += 2) {
        const float* row = p.luma.row(l);
        for (int x = 0; x < w; x += 2) {
            const int bin = std::clamp(int(row[x]) >> 2, 0, 63);
            p.hist[size_t(bin)] += 1;
            total += 1;
        }
    }
    if (total > 0)
        for (float& h : p.hist) h = float(h / total);
    // Border references cannot move, so they are measured here, in parallel.
    const int64_t frame = field / 2;
    for (size_t i = 0; i < m_c.refs.size(); ++i) {
        const Reference& r = m_c.refs[i];
        if (!r.enabled || !r.isBorder()) continue;
        int si = -1;
        for (size_t k = 0; k < r.spans.size(); ++k)
            if (frame >= r.spans[k].start && frame < r.spans[k].end) si = int(k);
        if (si < 0) continue;
        const RefSpan& span = r.spans[size_t(si)];
        const EdgeTemplate& t = m_i->templ[i][size_t(si)];
        std::vector<LineMeasure>& out = p.border[i];
        out.resize(size_t(lines));
        for (int l = 0; l < lines; ++l) {
            const int y = 2 * l + p.parity;
            out[size_t(l)] = measureLine(p.luma.row(l), p.grad.data() + size_t(l) * size_t(w), w, t, spanX(span, r.kind, y), rangeFor(m_c.s, y));
        }
        p.borderDone[i] = true;
    }
    return p;
}

void EdgeTracker::push(PreparedField&& f) {
    Impl& I = *m_i;
    const EdgeLockSettings& s = m_c.s;
    const int w = f.luma.width, lines = f.luma.lines;
    const int64_t frame = f.field / 2;
    Pending pd;
    pd.field = f.field;
    pd.parity = f.parity;
    pd.lines = lines;
    pd.width = w;

    if (m_c.resetFrames.count(frame) && I.lastResetFrame != frame) {
        I.lastResetFrame = frame;
        pd.flags |= kFieldReset;
        for (RefRun& rr : I.run) {
            ++rr.seg;
            rr.lostCount = 0;
            rr.wide = true;
            if (rr.state == RefState::Lost) rr.state = RefState::Active;
        }
    }
    double histDist = 0;
    if (I.havePrev)
        for (size_t k = 0; k < 64; ++k) histDist += std::fabs(double(f.hist[k]) - double(I.prevHist[k]));
    histDist *= 0.5;
    I.prevHist = f.hist;
    I.havePrev = true;

    std::array<bool, kMaxReferences> wasActiveDrawn{};
    for (size_t i = 0; i < m_c.refs.size(); ++i) {
        const Reference& ref = m_c.refs[i];
        RefRun& rr = I.run[i];
        PendRef& pr = pd.refs[i];
        pr.r.assign(size_t(lines), kNaN);
        pr.conf.assign(size_t(lines), 0.0);
        pr.shape.assign(size_t(lines), kNaN);
        pr.peak.assign(size_t(lines), 0.0);
        int si = -1;
        for (size_t k = 0; k < ref.spans.size(); ++k)
            if (frame >= ref.spans[k].start && frame < ref.spans[k].end) si = int(k);
        if (!ref.enabled || si < 0) {
            rr.spanIdx = -1;
            rr.state = RefState::Inactive;
            pr.state = RefState::Inactive;
            continue;
        }
        const RefSpan& span = ref.spans[size_t(si)];
        if (si != rr.spanIdx) {
            // A new span starts fresh.
            const int64_t endedStart = rr.endedSpanStart, endedFrame = rr.endedFrame;
            rr = RefRun{};
            rr.endedSpanStart = endedStart;
            rr.endedFrame = endedFrame;
            rr.spanIdx = si;
            rr.state = RefState::Active;
            rr.seg = int(si) * 1000 + 1;
        }
        if (!ref.isBorder() && rr.endedSpanStart == span.start && frame >= rr.endedFrame) {
            rr.state = RefState::Ended;
            pr.state = RefState::Ended;
            continue;
        }
        wasActiveDrawn[i] = !ref.isBorder() && rr.state == RefState::Active;
        const EdgeTemplate& t = I.templ[i][size_t(si)];

        // Where to look: the shape, plus the predicted real motion for a drawn
        // edge. The velocity is the median of recent steps, so a few bad fields
        // cannot set the edge running after something else.
        double pa = 0, pb = 0;
        bool havePrediction = false;
        if (!ref.isBorder()) {
            std::vector<const MotionSample*> recent;  // newest first
            for (auto it = rr.hist.rbegin(); it != rr.hist.rend() && recent.size() < 7; ++it)
                if (it->seg == rr.seg) recent.push_back(&*it);
            if (!recent.empty()) {
                havePrediction = true;
                pa = recent.front()->a;
                std::vector<double> bs;
                for (const MotionSample* m : recent) bs.push_back(m->b);
                std::nth_element(bs.begin(), bs.begin() + long(bs.size() / 2), bs.end());
                pb = bs[bs.size() / 2];
                // Fresh after a loss: hold still until the edge has proven itself.
                if (rr.state != RefState::Lost && rr.state != RefState::Dark && !rr.recovering && recent.size() >= 3) {
                    std::vector<double> vel;
                    for (size_t k = 0; k + 1 < recent.size(); ++k) {
                        const double df = double(recent[k]->field - recent[k + 1]->field);
                        if (df > 0) vel.push_back((recent[k]->a - recent[k + 1]->a) / df);
                    }
                    if (!vel.empty()) {
                        std::nth_element(vel.begin(), vel.begin() + long(vel.size() / 2), vel.end());
                        pa += vel[vel.size() / 2] * double(f.field - recent.front()->field);
                    }
                }
            } else if (rr.haveHeld) {
                havePrediction = true;
                pa = rr.heldA;
                pb = rr.heldB;
            }
        }
        const bool wide = rr.wide || rr.state == RefState::Lost;
        int contrast = 0;
        std::vector<double> ys;
        for (int l = 0; l < lines; ++l) {
            const int y = 2 * l + f.parity;
            const double sx = spanX(span, ref.kind, y);
            if (!std::isfinite(sx)) continue;
            pr.shape[size_t(l)] = sx;
            ++pr.covered;
            LineMeasure m;
            if (ref.isBorder() && f.borderDone[i] && !wide) {
                m = f.border[i][size_t(l)];
            } else {
                int range = rangeFor(s, y);
                if (wide) range = std::min(60, range * 3);
                m = measureLine(f.luma.row(l), f.grad.data() + size_t(l) * size_t(w), w, t, sx + pa + pb * y, range, wide || !ref.isBorder());
            }
            // A line is dark when nothing crosses the edge and the picture
            // there is black or flat; a textured occluder is not dark, it is lost.
            contrast += m.contrast || !(m.level < kDarkLevel || m.flat);
            // Coming back, the edge has to look like itself again, not just line
            // up: an occluder's own edge passing by has a different step.
            const bool lookAlike = !(wide || rr.recovering) || ref.isBorder() ||
                                   (m.step >= kRecoverStep[0] * t.step && m.step <= kRecoverStep[1] * t.step);
            if (m.conf >= s.confidence && lookAlike) {
                pr.r[size_t(l)] = m.pos - sx;
                pr.conf[size_t(l)] = m.conf;
                pr.peak[size_t(l)] = m.peak;
                ++pr.measured;
            }
        }
        if (pr.measured > 0) rejectSpatialOutliers(pr.r, pr.conf, pr.measured);
        const bool dark = pr.covered > 0 && contrast < kDarkShare * pr.covered;
        bool motionValid = false;
        double ma = 0, mb = 0;
        if (!ref.isBorder() && pr.measured >= std::max(8, int(0.3 * pr.covered))) {
            std::vector<double> yy, r, wv, peaks;
            for (int l = 0; l < lines; ++l)
                if (std::isfinite(pr.r[size_t(l)])) {
                    yy.push_back(2.0 * l + f.parity);
                    r.push_back(pr.r[size_t(l)]);
                    wv.push_back(pr.conf[size_t(l)]);
                    peaks.push_back(pr.peak[size_t(l)]);
                }
            // Iteratively reweighted, starting from the prediction when there is
            // one, so lines with a big timebase error, or an occluder's own
            // edge, cannot drag the motion.
            bool fitted = havePrediction;
            if (havePrediction) {
                ma = pa;
                mb = pb;
            } else {
                fitted = fitLine(yy, r, wv, ma, mb);
            }
            if (fitted) {
                for (int it = 0; it < 8; ++it) {
                    std::vector<double> w2(wv.size());
                    for (size_t k = 0; k < wv.size(); ++k) {
                        const double res = (r[k] - (ma + mb * yy[k])) / kTukeyPx;
                        w2[k] = std::fabs(res) < 1.0 ? wv[k] * (1 - res * res) * (1 - res * res) : 0.0;
                    }
                    if (!fitLine(yy, r, w2, ma, mb)) {
                        fitted = false;
                        break;
                    }
                }
            }
            // Real motion changes slowly. A jump away from the prediction is
            // something else matching, not the edge moving.
            // Coming back from Lost, the wider window only finds the edge; it has to
            // be close to where it was held, or it is something else.
            const double guard = wide ? std::max(kMotionGuardPx, 0.5 * s.searchRange) : (rr.recovering ? 0.5 * kMotionGuardPx : kMotionGuardPx);
            if (fitted && havePrediction && std::fabs(ma - pa) > guard) fitted = false;
            if (fitted && median(peaks) < kMinMedianPeak) fitted = false;
            if (fitted) {
                motionValid = true;
                for (int l = 0; l < lines; ++l) {
                    if (!std::isfinite(pr.r[size_t(l)])) continue;
                    if (std::fabs(pr.r[size_t(l)] - (ma + mb * (2.0 * l + f.parity))) > kTukeyPx) {
                        pr.r[size_t(l)] = kNaN;
                        pr.conf[size_t(l)] = 0;
                        --pr.measured;
                    }
                }
            } else {
                std::fill(pr.r.begin(), pr.r.end(), kNaN);
                std::fill(pr.conf.begin(), pr.conf.end(), 0.0);
                pr.measured = 0;
            }
        }
        if (!ref.isBorder() && !motionValid && pr.measured > 0) {
            // Too few lines to check against the motion: none are trusted.
            std::fill(pr.r.begin(), pr.r.end(), kNaN);
            std::fill(pr.conf.begin(), pr.conf.end(), 0.0);
            pr.measured = 0;
        }
        pr.q = pr.covered ? double(pr.measured) / pr.covered : 0.0;
        if (dark) {
            rr.state = RefState::Dark;
            rr.stable = 0;
            rr.recovering = true;
        } else if (pr.q < kLostShare) {
            if (rr.state == RefState::Dark) rr.state = RefState::Active;
            if (++rr.lostCount >= kLostFields) {
                rr.state = RefState::Lost;
                rr.recovering = true;
            }
            rr.stable = 0;
        } else {
            rr.lostCount = 0;
            rr.state = RefState::Active;
            rr.wide = false;
            if (++rr.stable >= kStableFields) rr.recovering = false;
        }
        if (!ref.isBorder() && motionValid && rr.state == RefState::Active) rr.hist.push_back({f.field, ma, mb, pr.q, rr.seg});
        pr.state = rr.state;
        pr.seg = rr.seg;
    }

    // Scene cut: a histogram jump, and every drawn edge losing its template at once.
    int drawnActive = 0, drawnLostAtOnce = 0;
    for (size_t i = 0; i < m_c.refs.size(); ++i) {
        if (!wasActiveDrawn[i]) continue;
        ++drawnActive;
        if (pd.refs[i].q < kCutShare) ++drawnLostAtOnce;
    }
    if (drawnActive > 0 && drawnLostAtOnce == drawnActive && histDist > kCutHistogram) {
        pd.flags |= kFieldCut;
        m_result.cuts.push_back(frame);
        for (size_t i = 0; i < m_c.refs.size(); ++i) {
            if (!wasActiveDrawn[i]) continue;
            RefRun& rr = I.run[i];
            const RefSpan& span = m_c.refs[i].spans[size_t(rr.spanIdx)];
            rr.endedSpanStart = span.start;
            rr.endedFrame = frame;
            rr.state = RefState::Ended;
            PendRef& pr = pd.refs[i];
            pr.state = RefState::Ended;
            std::fill(pr.r.begin(), pr.r.end(), kNaN);
            std::fill(pr.conf.begin(), pr.conf.end(), 0.0);
            pr.measured = 0;
            m_result.autoEnds.push_back({int(i), frame});
        }
    }
    bool anyCovered = false, allDark = true;
    for (size_t i = 0; i < m_c.refs.size(); ++i) {
        if (pd.refs[i].covered == 0 || pd.refs[i].state == RefState::Ended) continue;
        anyCovered = true;
        allDark &= pd.refs[i].state == RefState::Dark;
    }
    if (anyCovered && allDark) pd.flags |= kFieldDark;

    I.pend.push_back(std::move(pd));
    const size_t half = size_t(std::max(0, s.motionSmoothing / 2));
    while (I.pend.size() > half) finalizeOne();
}

void EdgeTracker::finalizeOne() {
    Impl& I = *m_i;
    const EdgeLockSettings& s = m_c.s;
    const size_t half = size_t(std::max(0, s.motionSmoothing / 2));
    Pending p = std::move(I.pend.front());
    I.pend.pop_front();
    // finalize p
    FieldOffsets fo;
    fo.field = p.field;
    fo.parity = p.parity;
    fo.track.flags = p.flags;
    const int n = p.lines;
    const int w = p.width;
    std::vector<double> v(size_t(n), 0.0), wgt(size_t(n), 0.0);
    std::vector<uint8_t> cb(size_t(n), kConfNone);
    // Per line, the measured position and error of each reference, for length correction.
    std::vector<std::array<double, kMaxReferences>> lineE(static_cast<size_t>(n)), lineX(static_cast<size_t>(n));
    for (auto& a : lineE) a.fill(kNaN);
    for (auto& a : lineX) a.fill(kNaN);
    const int H = int(half);
    for (size_t i = 0; i < m_c.refs.size(); ++i) {
        const Reference& ref = m_c.refs[i];
        PendRef& pr = p.refs[i];
        RefRun& rr = I.run[i];
        RefFieldTrack& tr = fo.track.refs[i];
        tr.state = pr.state;
        tr.measured = uint8_t(std::lround(std::clamp(pr.q, 0.0, 1.0) * 255.0));
        double Ma = 0, Mb = 0;
        if (!ref.isBorder() && pr.state != RefState::Inactive && pr.state != RefState::Ended) {
            // Real motion: local linear fit over the window, inside this segment.
            std::vector<double> xs, as, bs, ws;
            for (const MotionSample& m : rr.hist) {
                if (m.seg != pr.seg || std::llabs(m.field - p.field) > H) continue;
                xs.push_back(double(m.field - p.field));
                as.push_back(m.a);
                bs.push_back(m.b);
                ws.push_back(std::max(0.05, m.q));
            }
            // A field that measured its own motion gets the fit through the
            // window (at a segment end the window is one-sided, which is fine:
            // the fit is still evaluated at a sample). A field that did not is
            // held, never extrapolated.
            bool own = false;
            for (double x : xs) own |= x == 0;
            bool have = false;
            if (own && xs.size() >= 3) {
                double a0, a1, b0, b1;
                have = fitLine(xs, as, ws, a0, a1) && fitLine(xs, bs, ws, b0, b1);
                if (have) {
                    Ma = a0;
                    Mb = b0;
                }
            } else if (own) {
                double sa = 0, sb = 0, sw = 0;
                for (size_t k = 0; k < xs.size(); ++k) {
                    const double wk = ws[k] / (1.0 + std::fabs(xs[k]));
                    sa += wk * as[k];
                    sb += wk * bs[k];
                    sw += wk;
                }
                if (sw > 0) {
                    Ma = sa / sw;
                    Mb = sb / sw;
                    have = true;
                }
            }
            if (pr.state == RefState::Lost || pr.state == RefState::Dark || !have) {
                if (rr.haveHeld) {
                    Ma = rr.heldA;
                    Mb = rr.heldB;
                }
            } else if (pr.state == RefState::Active) {
                rr.haveHeld = true;
                rr.heldA = Ma;
                rr.heldB = Mb;
            }
            tr.a = float(Ma);
            tr.b = float(Mb);
        }
        if (pr.state == RefState::Inactive || pr.state == RefState::Ended) continue;
        for (int l = 0; l < n; ++l) {
            if (!std::isfinite(pr.r[size_t(l)])) continue;
            const double y = 2.0 * l + p.parity;
            const double e = ref.isBorder() ? pr.r[size_t(l)] : pr.r[size_t(l)] - (Ma + Mb * y);
            lineE[size_t(l)][i] = e;
            lineX[size_t(l)][i] = pr.shape[size_t(l)] + (ref.isBorder() ? 0.0 : Ma + Mb * y);
        }
    }
    // Drop motion samples the window no longer needs.
    for (RefRun& rr : I.run)
        while (!rr.hist.empty() && rr.hist.front().field < p.field - H) rr.hist.pop_front();

    // Combine per line: the left border wins when it measured, then the
    // right border, then drawn edges weighted by confidence.
    int measuredLines = 0;
    for (int l = 0; l < n; ++l) {
        double best = kNaN, bc = 0;
        for (RefKind k : {RefKind::LeftBorder, RefKind::RightBorder}) {
            if (std::isfinite(best)) break;
            for (size_t i = 0; i < m_c.refs.size(); ++i)
                if (m_c.refs[i].kind == k && std::isfinite(lineE[size_t(l)][i])) {
                    best = lineE[size_t(l)][i];
                    bc = p.refs[i].conf[size_t(l)];
                    break;
                }
        }
        if (!std::isfinite(best)) {
            double sw = 0, sv = 0, cmax = 0;
            for (size_t i = 0; i < m_c.refs.size(); ++i) {
                if (m_c.refs[i].isBorder() || !std::isfinite(lineE[size_t(l)][i])) continue;
                const double c = p.refs[i].conf[size_t(l)];
                sw += c;
                sv += c * lineE[size_t(l)][i];
                cmax = std::max(cmax, c);
            }
            if (sw > 0) {
                best = sv / sw;
                bc = cmax;
            }
        }
        if (std::isfinite(best)) {
            v[size_t(l)] = best;
            wgt[size_t(l)] = std::max(0.05, bc);
            cb[size_t(l)] = uint8_t(std::clamp(int(std::lround(double(kConfMeasuredMin) + bc * (255.0 - double(kConfMeasuredMin)))), int(kConfMeasuredMin), 255));
            ++measuredLines;
        }
    }
    std::vector<double> slope;
    if (s.lengthCorrection) slope.assign(size_t(n), 0.0);
    if (measuredLines == 0) {
        // Nothing measured: ease what was there to zero instead of snapping.
        ++I.sinceMeasured;
        const double e = I.sinceMeasured <= kEaseFields ? 1.0 - double(I.sinceMeasured) / kEaseFields : 0.0;
        for (int l = 0; l < n; ++l) {
            const double base = l < int(I.lastSmooth.size()) ? I.lastSmooth[size_t(l)] : 0.0;
            v[size_t(l)] = base * e;
            cb[size_t(l)] = e > 0 && base != 0 ? kConfEased : kConfNone;
            if (s.lengthCorrection && l < int(I.lastSmoothSlope.size())) slope[size_t(l)] = I.lastSmoothSlope[size_t(l)] * e;
        }
    } else {
        I.sinceMeasured = 0;
        if (s.lengthCorrection) {
            // Two references far enough apart on a line give its length too.
            const double xref = 0.5 * w;
            std::vector<double> sw(size_t(n), 0.0);
            for (int l = 0; l < n; ++l) {
                int li = -1, ri = -1;
                for (size_t i = 0; i < m_c.refs.size(); ++i) {
                    if (!std::isfinite(lineE[size_t(l)][i])) continue;
                    if (li < 0 || lineX[size_t(l)][i] < lineX[size_t(l)][size_t(li)]) li = int(i);
                    if (ri < 0 || lineX[size_t(l)][i] > lineX[size_t(l)][size_t(ri)]) ri = int(i);
                }
                if (li < 0 || ri < 0 || li == ri) continue;
                const double xl = lineX[size_t(l)][size_t(li)], xr = lineX[size_t(l)][size_t(ri)];
                if (xr - xl < w / 3.0) continue;
                const double el = lineE[size_t(l)][size_t(li)], er = lineE[size_t(l)][size_t(ri)];
                const double k = (er - el) / (xr - xl);
                slope[size_t(l)] = k;
                v[size_t(l)] = el + (xref - xl) * k;
                sw[size_t(l)] = 1.0;
            }
            std::vector<uint8_t> scb(size_t(n), kConfNone);
            gapFill(slope, scb, sw, s.maxGap, &I.lastSmoothSlope);
        }
        gapFill(v, cb, wgt, s.maxGap, &I.lastSmooth);
        fo.track.flags |= kFieldMeasured;
    }
    fo.off.resize(size_t(n));
    fo.conf = cb;
    double sum = 0;
    float mx = 0, mn = 0;
    for (int l = 0; l < n; ++l) {
        fo.off[size_t(l)] = float(v[size_t(l)]);
        sum += std::fabs(v[size_t(l)]);
        double lo = v[size_t(l)], hi = v[size_t(l)];
        if (s.lengthCorrection) {
            const double xref = 0.5 * w;
            const double e0 = v[size_t(l)] + (0 - xref) * slope[size_t(l)];
            const double e1 = v[size_t(l)] + (w - 1 - xref) * slope[size_t(l)];
            lo = std::min(e0, e1);
            hi = std::max(e0, e1);
        }
        mx = std::max(mx, float(hi));
        mn = std::min(mn, float(lo));
    }
    if (s.lengthCorrection) {
        fo.slope.resize(size_t(n));
        for (int l = 0; l < n; ++l) fo.slope[size_t(l)] = float(slope[size_t(l)]);
    }
    fo.track.meanAbs = n ? float(sum / n) : 0.0f;
    fo.track.maxPos = mx;
    fo.track.maxNeg = mn;
    if (onField) onField(std::move(fo));
}

void EdgeTracker::finish() {
    while (!m_i->pend.empty()) finalizeOne();
}

void EdgeTracker::measureForOverlay(const TrackerConfig& c, int slot, const RefSpan& span, const FieldLuma& luma, double motionA,
                                    double motionB, std::vector<double>& pos, std::vector<double>& conf) {
    const Reference& ref = c.refs[size_t(slot)];
    const EdgeTemplate t = makeTemplate(span.templ, span.templStep);
    const int w = luma.width, lines = luma.lines;
    pos.assign(size_t(lines), kNaN);
    conf.assign(size_t(lines), 0.0);
    std::vector<float> g(static_cast<size_t>(w));
    for (int l = 0; l < lines; ++l) {
        const int y = 2 * l + luma.parity;
        const double sx = spanX(span, ref.kind, y);
        if (!std::isfinite(sx)) continue;
        gradientRow(luma.row(l), w, g.data());
        const LineMeasure m = measureLine(luma.row(l), g.data(), w, t, sx + motionA + motionB * y, rangeFor(c.s, y));
        conf[size_t(l)] = m.conf;
        if (m.conf >= c.s.confidence) pos[size_t(l)] = m.pos;
    }
}

}  // namespace ud

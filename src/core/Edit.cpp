// SPDX-License-Identifier: GPL-3.0-or-later
#include "Edit.h"

#include <algorithm>
#include <cmath>

namespace ud {

EditList EditList::build(int64_t frames, const EditSettings& e, const std::function<int64_t(int64_t)>& snapIn) {
    auto snap = [&](int64_t f) { return std::clamp<int64_t>(snapIn ? snapIn(f) : f, 0, frames); };
    EditList el;
    const int64_t in = snap(std::clamp<int64_t>(e.in, 0, frames));
    const int64_t out = snap(e.out < 0 ? frames : std::clamp<int64_t>(e.out, 0, frames));
    std::vector<CutSpan> cuts;
    for (const CutSpan& c : e.cuts) {
        CutSpan s{snap(std::clamp<int64_t>(c.start, 0, frames)), snap(std::clamp<int64_t>(c.end, 0, frames))};
        if (s.end > s.start) cuts.push_back(s);
    }
    std::sort(cuts.begin(), cuts.end(), [](const CutSpan& a, const CutSpan& b) { return a.start < b.start; });
    // Merge overlapping cuts.
    std::vector<CutSpan> merged;
    for (const CutSpan& c : cuts) {
        if (!merged.empty() && c.start <= merged.back().end) merged.back().end = std::max(merged.back().end, c.end);
        else merged.push_back(c);
    }
    int64_t pos = in;
    for (const CutSpan& c : merged) {
        if (c.end <= pos) continue;
        if (c.start >= out) break;
        if (c.start > pos) el.m_ranges.push_back({pos, std::min(c.start, out)});
        pos = std::max(pos, c.end);
    }
    if (pos < out) el.m_ranges.push_back({pos, out});
    return el;
}

int64_t EditList::outputFrames() const {
    int64_t n = 0;
    for (const Range& r : m_ranges) n += r.length();
    return n;
}

int64_t EditList::mapFrame(int64_t src) const {
    int64_t off = 0;
    for (const Range& r : m_ranges) {
        if (src >= r.start && src < r.end) return off + (src - r.start);
        off += r.length();
    }
    return -1;
}

bool EditList::mapTime(double srcSec, const Rational& fps, double& outSec) const {
    const double f = srcSec * fps.toDouble();
    double off = 0;
    for (const Range& r : m_ranges) {
        if (f >= double(r.start) && f < double(r.end)) {
            outSec = (off + f - double(r.start)) / fps.toDouble();
            return true;
        }
        off += double(r.length());
    }
    return false;
}

int64_t EditList::sourceOf(int64_t outPos) const {
    int64_t off = 0;
    for (const Range& r : m_ranges) {
        if (outPos < off + r.length()) return r.start + (outPos - off);
        off += r.length();
    }
    return m_ranges.empty() ? 0 : m_ranges.back().end - 1;
}

std::vector<Chapter> mapChapters(const std::vector<Chapter>& src, const EditList& el) {
    std::vector<Chapter> out;
    for (const Chapter& c : src) {
        for (const Range& r : el.ranges()) {
            const int64_t a = std::max(c.start, r.start), b = std::min(c.end, r.end);
            if (b <= a) continue;
            const int64_t at = el.mapFrame(a);
            if (!out.empty() && out.back().start == at) continue;
            out.push_back({at, 0, c.title});
        }
    }
    std::sort(out.begin(), out.end(), [](const Chapter& a, const Chapter& b) { return a.start < b.start; });
    out.erase(std::unique(out.begin(), out.end(), [](const Chapter& a, const Chapter& b) { return a.start == b.start; }), out.end());
    const int64_t total = el.outputFrames();
    for (size_t i = 0; i < out.size(); ++i) out[i].end = i + 1 < out.size() ? out[i + 1].start : total;
    return out;
}

std::vector<Caption> mapCaptions(const std::vector<Caption>& src, const EditList& el, const Rational& fps) {
    std::vector<Caption> out;
    const double f = fps.toDouble();
    for (const Caption& c : src) {
        double off = 0;
        for (const Range& r : el.ranges()) {
            const double rs = double(r.start) / f, re = double(r.end) / f;
            const double a = std::max(c.start, rs), b = std::min(c.end, re);
            if (b > a + 1e-6) out.push_back({off + (a - rs), off + (b - rs), c.text});
            off += re - rs;
        }
    }
    return out;
}

int64_t AudioPlan::totalSamples() const {
    int64_t n = 0;
    for (const Part& p : parts) n += p.end - p.start;
    return n;
}

AudioPlan planAudio(const EditList& el, const SourceInfo& src) {
    AudioPlan p;
    if (!src.hasAudio) return p;
    for (const Range& r : el.ranges()) p.parts.push_back({src.sampleOfFrame(r.start), src.sampleOfFrame(r.end)});
    p.half = int64_t(std::lround(kCrossfadeSeconds * 0.5 * src.sampleRate));
    return p;
}

bool AudioCutter::open(const SourceInfo& src, const AudioPlan& plan, std::string& err) {
    m_plan = plan;
    m_channels = std::max(1, src.channels);
    m_out = 0;
    return m_reader.open(src, err);
}

bool AudioCutter::next(int64_t count, std::vector<int32_t>& out) {
    out.clear();
    const int64_t total = m_plan.totalSamples();
    count = std::min(count, total - m_out);
    if (count <= 0) return true;
    out.reserve(size_t(count) * size_t(m_channels));
    // Output start of each part.
    std::vector<int64_t> J(m_plan.parts.size() + 1, 0);
    for (size_t k = 0; k < m_plan.parts.size(); ++k) J[k + 1] = J[k] + (m_plan.parts[k].end - m_plan.parts[k].start);
    const int64_t H = m_plan.half;
    const size_t last = m_plan.parts.size() - 1;
    std::vector<int32_t> a, b;
    int64_t o = m_out;
    const int64_t stop = m_out + count;
    while (o < stop) {
        size_t k = 0;
        while (k + 1 < m_plan.parts.size() && o >= J[k + 1]) ++k;
        // Inside the crossfade around the join at J[k] (k > 0) or J[k + 1]?
        int join = -1;
        if (k > 0 && o < J[k] + H) join = int(k);
        else if (k < last && o >= J[k + 1] - H) join = int(k + 1);
        if (join < 0) {
            const int64_t regEnd = std::min(stop, k < last ? J[k + 1] - H : J[k + 1]);
            const int64_t n = regEnd - o;
            if (!m_reader.read(m_plan.parts[k].start + (o - J[k]), n, a)) return false;
            out.insert(out.end(), a.begin(), a.end());
            o += n;
            continue;
        }
        const size_t j = size_t(join);
        const int64_t regStart = J[j] - H, regEnd = std::min(stop, J[j] + H);
        const int64_t n = regEnd - o;
        // A runs on past the end of the earlier part into the cut; B starts
        // inside the cut before the later part. Both are real tape.
        if (!m_reader.read(m_plan.parts[j - 1].end + (o - J[j]), n, a)) return false;
        if (!m_reader.read(m_plan.parts[j].start + (o - J[j]), n, b)) return false;
        for (int64_t i = 0; i < n; ++i) {
            const double t = (double(o + i - regStart) + 0.5) / double(2 * H);
            const double gb = std::sin(0.5 * M_PI * t) * std::sin(0.5 * M_PI * t), ga = 1.0 - gb;
            for (int c = 0; c < m_channels; ++c) {
                const size_t idx = size_t(i) * size_t(m_channels) + size_t(c);
                const double v = ga * a[idx] + gb * b[idx];
                out.push_back(int32_t(std::clamp(std::lround(v), -2147483648L, 2147483647L)));
            }
        }
        o += n;
    }
    m_out = stop;
    return true;
}

}  // namespace ud

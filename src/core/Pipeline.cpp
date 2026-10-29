// SPDX-License-Identifier: GPL-3.0-or-later
#include "Pipeline.h"

#include "Simd.h"

#include <algorithm>
#include <cmath>
#include <cstring>

extern "C" {
#include <libavutil/pixdesc.h>
}

namespace ud {

void cropPicture(Picture& p, int l, int r, int t, int b) {
    l = std::clamp(l, 0, p.width - 2);
    r = std::clamp(r, 0, p.width - 2 - l);
    t = std::clamp(t, 0, p.height - 2);
    b = std::clamp(b, 0, p.height - 2 - t);
    if (!l && !r && !t && !b) return;
    const int w = p.width - l - r, h = p.height - t - b;
    Picture o;
    o.alloc(w, h, p.gray);
    for (int c = 0; c < p.planes(); ++c)
        for (int y = 0; y < h; ++y) std::memcpy(o.p[c].row(y), p.p[c].row(y + t) + l, sizeof(float) * size_t(w));
    p = std::move(o);
}

void resizeWidth(Picture& p, int width) {
    if (width == p.width || width <= 0) return;
    const double scale = double(width) / double(p.width);
    // Downscaling widens the kernel so nothing aliases.
    const double support = 3.0 * std::max(1.0, 1.0 / scale);
    const double kscale = std::min(1.0, scale);
    struct Tap {
        int first = 0;
        std::vector<float> w;
    };
    std::vector<Tap> taps(static_cast<size_t>(width));
    auto sinc = [](double x) { return std::fabs(x) < 1e-9 ? 1.0 : std::sin(M_PI * x) / (M_PI * x); };
    for (int x = 0; x < width; ++x) {
        const double c = (x + 0.5) / scale - 0.5;
        const int a = int(std::floor(c - support)) + 1, b = int(std::floor(c + support));
        Tap& t = taps[size_t(x)];
        t.first = a;
        double sum = 0;
        for (int i = a; i <= b; ++i) {
            const double z = (double(i) - c) * kscale;
            const double v = std::fabs(z) < 3.0 ? sinc(z) * sinc(z / 3.0) : 0.0;
            t.w.push_back(float(v));
            sum += v;
        }
        for (float& v : t.w) v = float(v / sum);
    }
    Picture o;
    o.alloc(width, p.height, p.gray);
    for (int c = 0; c < p.planes(); ++c)
        for (int y = 0; y < p.height; ++y) {
            const float* s = p.p[c].row(y);
            float* d = o.p[c].row(y);
            for (int x = 0; x < width; ++x) {
                const Tap& t = taps[size_t(x)];
                float v = 0;
                for (size_t k = 0; k < t.w.size(); ++k) v += t.w[k] * s[std::clamp(t.first + int(k), 0, p.width - 1)];
                d[x] = v;
            }
        }
    p = std::move(o);
}

OutputGeometry outputGeometry(const Project& p, const SourceInfo& s, AVPixelFormat enc) {
    OutputGeometry g;
    const AVPixFmtDescriptor* d = av_pix_fmt_desc_get(enc);
    const bool gray = d && d->nb_components < 3;
    const int sx = gray || !d ? 0 : d->log2_chroma_w, sy = gray || !d ? 0 : d->log2_chroma_h;
    const bool interlacedOut = p.deint.mode == DeintMode::None;
    g.cropLeft = std::max(0, p.crop.left);
    g.cropRight = std::max(0, p.crop.right);
    g.cropTop = std::max(0, p.crop.top);
    g.cropBottom = std::max(0, p.crop.bottom);
    if (interlacedOut) {
        // An odd crop of interlaced lines would swap the fields.
        if (g.cropTop & 1) {
            ++g.cropTop;
            g.notes.push_back("Top crop rounded up to even to keep field parity.");
        }
        if (g.cropBottom & 1) {
            ++g.cropBottom;
            g.notes.push_back("Bottom crop rounded up to even to keep field parity.");
        }
    }
    int w = s.width - g.cropLeft - g.cropRight, h = s.height - g.cropTop - g.cropBottom;
    w = std::max(16, w);
    h = std::max(16, h);
    if (sx && (w & 1)) {
        ++g.cropRight;
        --w;
        g.notes.push_back("Width snapped to even, since the output's chroma is halved horizontally.");
    }
    if (sy && (h & 1)) {
        ++g.cropBottom;
        --h;
        g.notes.push_back("Height snapped to even, since 4:2:0 cannot hold an odd size.");
    }
    g.croppedWidth = w;
    g.height = h;
    if (p.crop.squarePixels && s.sar.valid() && s.sar.num != s.sar.den) {
        int nw = int(std::lround(double(w) * s.sar.num / s.sar.den));
        if (sx && (nw & 1)) ++nw;
        g.width = nw;
        g.sar = {1, 1};
        g.resize = true;
    } else {
        g.width = w;
        g.sar = s.sar;
    }
    return g;
}

bool InputStage::open(const Project& p, const SourceInfo& s, std::string& err) {
    m_edge = false;
    m_interlaced = true;
    m_off.clear();
    for (const EdgeOverride& o : p.overrides)
        if (o.kind == EdgeOverride::Kind::DontCorrect) m_off.push_back(o);
    if (!p.edgeActive()) return true;
    if (!p.analysisCurrent()) {
        err = "Edge Lock is on but the clip has not been analyzed with these references and settings. Run Analyze (F5) first.";
        return false;
    }
    if (!m_store.open(p.offsetsPath(), err)) return false;
    if (m_store.header().lines != s.height || m_store.header().frames != s.frames) {
        err = "The offsets file does not match this source. Run Analyze (F5) again.";
        return false;
    }
    m_params.strength = p.edge.strength;
    m_params.fillRepeat = p.edge.fillRepeat;
    m_params.xref = m_store.header().xref;
    m_edge = p.edge.strength != 0;
    return true;
}

bool InputStage::dontCorrect(int64_t frame) const {
    for (const EdgeOverride& o : m_off)
        if (frame >= o.start && frame < o.end) return true;
    return false;
}

bool InputStage::fromDecoded(const AVFrame* f, int64_t n, Picture& out, bool* corrected) {
    if (corrected) *corrected = false;
    if (!toPicture(f, out, m_interlaced)) return false;
    if (!m_edge || dontCorrect(n)) return true;
    FrameOffsets fo;
    if (!m_store.get(n, fo) || fo.zero()) return true;
    // Off means off: a frame with nothing to correct is never resampled.
    applyOffsets(out, fo, m_params);
    if (corrected) *corrected = true;
    return true;
}

bool InputStage::frame(SourceReader& r, int64_t n, Picture& out, bool* corrected) {
    FramePtr f = r.read(n);
    if (!f) return false;
    return fromDecoded(f.get(), n, out, corrected);
}

void postProcess(Picture& pic, const Project& p, const SourceInfo& s, const OutputGeometry& g, int64_t sourceFrame, ColorStats* stats) {
    const ColorSettings& c = p.colorFor(sourceFrame, s.chapters);
    if (!c.neutral()) applyColor(pic, c, stats);
    else if (stats) stats->clipped = 0;
    cropPicture(pic, g.cropLeft, g.cropRight, g.cropTop, g.cropBottom);
    if (g.resize) resizeWidth(pic, g.width);
}

}  // namespace ud

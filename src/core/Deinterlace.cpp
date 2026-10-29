// SPDX-License-Identifier: GPL-3.0-or-later
#include "Deinterlace.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

extern "C" {
#include <libavfilter/avfilter.h>
#include <libavfilter/buffersink.h>
#include <libavfilter/buffersrc.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libavutil/pixdesc.h>
}

namespace ud {

const char* deintModeName(DeintMode m) {
    switch (m) {
    case DeintMode::Qtgmc: return "qtgmc";
    case DeintMode::Bwdif: return "bwdif";
    case DeintMode::Yadif: return "yadif";
    case DeintMode::Ivtc: return "ivtc";
    case DeintMode::None: return "none";
    }
    return "qtgmc";
}

DeintMode deintModeFromName(const std::string& s) {
    if (s == "bwdif") return DeintMode::Bwdif;
    if (s == "yadif") return DeintMode::Yadif;
    if (s == "ivtc") return DeintMode::Ivtc;
    if (s == "none") return DeintMode::None;
    return DeintMode::Qtgmc;
}

Rational deintOutputRate(const DeintSettings& d, const Rational& in) {
    switch (d.mode) {
    case DeintMode::None: return in;
    case DeintMode::Ivtc: return {in.num * 4, in.den * 5};
    default: return d.doubleRate ? Rational{in.num * 2, in.den} : in;
    }
}

struct FilterDeinterlacer::Impl {
    AVFilterGraph* graph = nullptr;
    AVFilterContext* src = nullptr;
    AVFilterContext* sink = nullptr;
    int64_t pts = 0;
    bool tff = true;
    ~Impl() { avfilter_graph_free(&graph); }
};

FilterDeinterlacer::FilterDeinterlacer() = default;
FilterDeinterlacer::~FilterDeinterlacer() = default;

void FilterDeinterlacer::close() { m_i.reset(); }

bool FilterDeinterlacer::open(DeintMode mode, bool doubleRate, bool tff, int width, int height, AVPixelFormat fmt, const Rational& fps,
                              const Rational& sar, std::string& err) {
    m_i = std::make_unique<Impl>();
    m_i->tff = tff;
    m_i->graph = avfilter_graph_alloc();
    if (!m_i->graph) {
        err = "out of memory";
        return false;
    }
    char args[512];
    std::snprintf(args, sizeof args, "video_size=%dx%d:pix_fmt=%d:time_base=%d/%d:pixel_aspect=%d/%d:frame_rate=%d/%d", width, height,
                  int(fmt), fps.den, fps.num, std::max(1, sar.num), std::max(1, sar.den), fps.num, fps.den);
    int r = avfilter_graph_create_filter(&m_i->src, avfilter_get_by_name("buffer"), "in", args, nullptr, m_i->graph);
    if (r < 0) {
        err = "buffer source: " + avError(r);
        return false;
    }
    r = avfilter_graph_create_filter(&m_i->sink, avfilter_get_by_name("buffersink"), "out", nullptr, nullptr, m_i->graph);
    if (r < 0) {
        err = "buffer sink: " + avError(r);
        return false;
    }
    const char* parity = tff ? "tff" : "bff";
    char chain[512];
    switch (mode) {
    case DeintMode::Bwdif:
        std::snprintf(chain, sizeof chain, "bwdif=mode=%s:parity=%s:deint=all", doubleRate ? "send_field" : "send_frame", parity);
        break;
    case DeintMode::Yadif:
        std::snprintf(chain, sizeof chain, "yadif=mode=%s:parity=%s:deint=all", doubleRate ? "send_field" : "send_frame", parity);
        break;
    case DeintMode::Ivtc:
        // Field matching rebuilds the film frames, yadif cleans the few that
        // will not match, decimate drops the repeat in every five.
        std::snprintf(chain, sizeof chain, "fieldmatch=order=%s:combmatch=full,yadif=deint=interlaced:parity=%s,decimate", parity, parity);
        break;
    default: std::snprintf(chain, sizeof chain, "null"); break;
    }
    AVFilterInOut* outputs = avfilter_inout_alloc();
    AVFilterInOut* inputs = avfilter_inout_alloc();
    outputs->name = av_strdup("in");
    outputs->filter_ctx = m_i->src;
    outputs->pad_idx = 0;
    outputs->next = nullptr;
    inputs->name = av_strdup("out");
    inputs->filter_ctx = m_i->sink;
    inputs->pad_idx = 0;
    inputs->next = nullptr;
    r = avfilter_graph_parse_ptr(m_i->graph, chain, &inputs, &outputs, nullptr);
    avfilter_inout_free(&inputs);
    avfilter_inout_free(&outputs);
    if (r < 0) {
        err = std::string("filter graph '") + chain + "': " + avError(r);
        return false;
    }
    r = avfilter_graph_config(m_i->graph, nullptr);
    if (r < 0) {
        err = "filter graph config: " + avError(r);
        return false;
    }
    return true;
}

bool FilterDeinterlacer::push(FramePtr f, std::string& err) {
    if (!m_i) return false;
    int r;
    if (f) {
        f->pts = m_i->pts++;
        setFrameInterlace(f.get(), true, m_i->tff);
        r = av_buffersrc_add_frame_flags(m_i->src, f.get(), AV_BUFFERSRC_FLAG_KEEP_REF);
    } else {
        r = av_buffersrc_add_frame_flags(m_i->src, nullptr, 0);
    }
    if (r < 0) {
        err = "deinterlace: " + avError(r);
        return false;
    }
    return true;
}

FramePtr FilterDeinterlacer::pull() {
    if (!m_i) return nullptr;
    FramePtr f = makeFrame();
    const int r = av_buffersink_get_frame(m_i->sink, f.get());
    if (r < 0) return nullptr;
    setFrameInterlace(f.get(), false, false);
    return f;
}

// ---------------------------------------------------------------------------

double combMetric(const AVFrame* f) {
    const AVPixFmtDescriptor* d = av_pix_fmt_desc_get(AVPixelFormat(f->format));
    if (!d) return 0;
    const int w = f->width, h = f->height;
    std::vector<uint16_t> a(static_cast<size_t>(w)), b(static_cast<size_t>(w)), c(static_cast<size_t>(w));
    const uint8_t* data[4] = {f->data[0], f->data[1], f->data[2], f->data[3]};
    const double scale = 1.0 / double(1 << std::max(0, d->comp[0].depth - 8));
    double sum = 0;
    long n = 0;
    for (int y = 1; y + 1 < h; y += 2) {
        av_read_image_line2(a.data(), data, f->linesize, d, 0, y - 1, 0, w, 0, 2);
        av_read_image_line2(b.data(), data, f->linesize, d, 0, y, 0, w, 0, 2);
        av_read_image_line2(c.data(), data, f->linesize, d, 0, y + 1, 0, w, 0, 2);
        for (int x = 16; x < w - 16; x += 2) {
            const double v = (double(a[size_t(x)]) - b[size_t(x)]) * (double(c[size_t(x)]) - b[size_t(x)]) * scale * scale;
            if (v > 0) sum += v;
            ++n;
        }
    }
    return n ? sum / double(n) : 0.0;
}

int cadencePhase(const std::vector<double>& comb, int64_t firstFrame) {
    if (comb.size() < 10) return -1;
    double score[5] = {0, 0, 0, 0, 0};
    for (int p = 0; p < 5; ++p)
        for (size_t i = 0; i < comb.size(); ++i) {
            const int64_t n = firstFrame + int64_t(i);
            const int pos = int(((n - p) % 5 + 5) % 5);
            score[p] += (pos == 0 || pos == 1) ? comb[i] : -0.66 * comb[i];
        }
    int best = 0;
    for (int p = 1; p < 5; ++p)
        if (score[p] > score[best]) best = p;
    double second = -1e300;
    for (int p = 0; p < 5; ++p)
        if (p != best) second = std::max(second, score[p]);
    double total = 0;
    for (double c : comb) total += c;
    if (total <= 0 || score[best] <= 0 || score[best] - second < 0.1 * total) return -1;
    return best;
}

int64_t snapToCadence(int64_t frame, int phase) {
    if (phase < 0) return frame;
    // Combed frames sit at phase and phase + 1; the cycle starts cleanly three after.
    const int start = (phase + 3) % 5;
    const int64_t off = ((frame - start) % 5 + 5) % 5;
    return off <= 2 ? frame - off : frame + (5 - off);
}

int64_t snapToCadenceInside(int64_t frame, int phase, int64_t frames) {
    if (phase < 0) return frame;
    int64_t s = snapToCadence(frame, phase);
    if (s < 0) s += 5;
    if (s > frames) s -= 5;
    return std::clamp<int64_t>(s, 0, frames);
}

}  // namespace ud

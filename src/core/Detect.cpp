// SPDX-License-Identifier: GPL-3.0-or-later
#include "Detect.h"

#include "Av.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

extern "C" {
#include <libavfilter/avfilter.h>
#include <libavfilter/buffersink.h>
#include <libavfilter/buffersrc.h>
#include <libavutil/channel_layout.h>
#include <libavutil/dict.h>
}

namespace ud {

namespace {

// One source, one chain of FFmpeg filters, one sink.
class Chain {
public:
    Chain() = default;
    Chain(const Chain&) = delete;
    Chain& operator=(const Chain&) = delete;
    ~Chain() {
        av_dict_free(&m_meta);
        avfilter_graph_free(&m_graph);
    }

    bool open(const char* srcName, const std::string& srcArgs, const std::string& chain, std::string& err) {
        m_graph = avfilter_graph_alloc();
        if (!m_graph) {
            err = "out of memory";
            return false;
        }
        const bool audio = std::strcmp(srcName, "abuffer") == 0;
        int r = avfilter_graph_create_filter(&m_src, avfilter_get_by_name(srcName), "in", srcArgs.c_str(), nullptr, m_graph);
        if (r < 0) {
            err = std::string(srcName) + ": " + avError(r);
            return false;
        }
        r = avfilter_graph_create_filter(&m_sink, avfilter_get_by_name(audio ? "abuffersink" : "buffersink"), "out", nullptr, nullptr, m_graph);
        if (r < 0) {
            err = "sink: " + avError(r);
            return false;
        }
        AVFilterInOut* outputs = avfilter_inout_alloc();
        AVFilterInOut* inputs = avfilter_inout_alloc();
        outputs->name = av_strdup("in");
        outputs->filter_ctx = m_src;
        inputs->name = av_strdup("out");
        inputs->filter_ctx = m_sink;
        r = avfilter_graph_parse_ptr(m_graph, chain.c_str(), &inputs, &outputs, nullptr);
        avfilter_inout_free(&inputs);
        avfilter_inout_free(&outputs);
        if (r < 0) {
            err = "filter '" + chain + "': " + avError(r);
            return false;
        }
        r = avfilter_graph_config(m_graph, nullptr);
        if (r < 0) {
            err = "filter graph: " + avError(r);
            return false;
        }
        return true;
    }

    // Pushes a frame (nullptr ends the stream) and keeps the metadata of the
    // newest frame out.
    bool push(AVFrame* f) {
        if (!m_src) return false;
        if (av_buffersrc_add_frame_flags(m_src, f, AV_BUFFERSRC_FLAG_KEEP_REF) < 0) return false;
        FramePtr o = makeFrame();
        while (av_buffersink_get_frame(m_sink, o.get()) >= 0) {
            // Newest value per key; a frame without metadata leaves the last values.
            av_dict_copy(&m_meta, o->metadata, 0);
            av_frame_unref(o.get());
        }
        return true;
    }

    double meta(const char* key, double fallback = 0) const {
        const AVDictionaryEntry* e = av_dict_get(m_meta, key, nullptr, 0);
        return e ? std::strtod(e->value, nullptr) : fallback;
    }
    bool has(const char* key) const { return av_dict_get(m_meta, key, nullptr, 0) != nullptr; }

private:
    AVFilterGraph* m_graph = nullptr;
    AVFilterContext* m_src = nullptr;
    AVFilterContext* m_sink = nullptr;
    AVDictionary* m_meta = nullptr;
};

std::string videoArgs(const AVFrame* f, const SourceInfo& s) {
    char a[256];
    std::snprintf(a, sizeof a, "video_size=%dx%d:pix_fmt=%d:time_base=%d/%d:pixel_aspect=%d/%d", f->width, f->height, f->format, s.fps.den, s.fps.num,
                  std::max(1, s.sar.num), std::max(1, s.sar.den));
    return a;
}

}  // namespace

IdetResult runIdet(const SourceInfo& s, int64_t a, int64_t b, int windows, int windowFrames, const DetectProgress& progress) {
    IdetResult r;
    a = std::clamp<int64_t>(a, 0, s.frames);
    b = std::clamp<int64_t>(b < 0 ? s.frames : b, a, s.frames);
    if (b - a < 8) {
        r.error = "too few frames to look at";
        return r;
    }
    SourceReader reader;
    if (!reader.open(s, r.error)) return r;
    const int64_t len = std::min<int64_t>(windowFrames, b - a);
    const int64_t room = (b - a) - len;
    const int n = int(std::max<int64_t>(1, std::min<int64_t>(windows, (b - a) / len)));
    for (int w = 0; w < n; ++w) {
        // A fresh filter per window, so a jump between windows is not counted as a field change.
        const int64_t start = a + (n > 1 ? room * w / (n - 1) : 0);
        Chain c;
        for (int64_t k = 0; k < len; ++k) {
            if (progress && !progress((double(w) + double(k) / double(len)) / double(n))) {
                r.error = "cancelled";
                return r;
            }
            FramePtr f = reader.read(start + k);
            if (!f) break;
            if (k == 0 && !c.open("buffer", videoArgs(f.get(), s), "idet", r.error)) return r;
            f->pts = k;
            if (!c.push(f.get())) break;
            ++r.frames;
        }
        c.push(nullptr);
        r.tff += int64_t(c.meta("lavfi.idet.multiple.tff"));
        r.bff += int64_t(c.meta("lavfi.idet.multiple.bff"));
        r.progressive += int64_t(c.meta("lavfi.idet.multiple.progressive"));
        r.undetermined += int64_t(c.meta("lavfi.idet.multiple.undetermined"));
        r.repeatedTop += int64_t(c.meta("lavfi.idet.repeated.top"));
        r.repeatedBottom += int64_t(c.meta("lavfi.idet.repeated.bottom"));
        r.repeatedNeither += int64_t(c.meta("lavfi.idet.repeated.neither"));
    }
    const int64_t decided = r.tff + r.bff + r.progressive;
    if (decided == 0) {
        r.error = "idet could not decide on any frame (a still or blank picture?)";
        return r;
    }
    if (r.progressive * 2 > decided) {
        r.order = FieldOrder::Progressive;
        r.confidence = double(r.progressive) / double(decided);
    } else {
        r.order = r.tff >= r.bff ? FieldOrder::TFF : FieldOrder::BFF;
        r.confidence = double(std::max(r.tff, r.bff)) / double(std::max<int64_t>(1, r.tff + r.bff));
    }
    // 3:2 pulldown repeats a field in 2 frames of every 5: 40% of frames on
    // clean film. Tape noise hides some repeats, and still shots hide them
    // all, so the line sits well under that. Plain video repeats close to none.
    const int64_t rep = r.repeatedTop + r.repeatedBottom;
    const int64_t all = rep + r.repeatedNeither;
    r.film = all > 0 && double(rep) / double(all) > 0.15 && std::min(r.repeatedTop, r.repeatedBottom) * 3 >= std::max(r.repeatedTop, r.repeatedBottom);
    if (progress) progress(1.0);
    r.ok = true;
    return r;
}

CropDetectResult runCropDetect(const SourceInfo& s, int64_t a, int64_t b, int samples, const DetectProgress& progress) {
    CropDetectResult r;
    a = std::clamp<int64_t>(a, 0, s.frames);
    b = std::clamp<int64_t>(b < 0 ? s.frames : b, a, s.frames);
    if (b <= a) {
        r.error = "nothing to look at";
        return r;
    }
    SourceReader reader;
    if (!reader.open(s, r.error)) return r;
    const int n = int(std::max<int64_t>(1, std::min<int64_t>(samples, b - a)));
    Chain c;
    int width = 0, height = 0;
    for (int i = 0; i < n; ++i) {
        if (progress && !progress(double(i) / double(n))) {
            r.error = "cancelled";
            return r;
        }
        const int64_t at = a + (b - a) * i / n + (b - a) / (2 * n);
        FramePtr f = reader.read(std::min(at, b - 1));
        if (!f) continue;
        if (!width) {
            width = f->width;
            height = f->height;
            // 24 on an 8-bit scale, whatever the depth. reset=0 keeps the
            // widest picture seen, so a border counts only if it is black in every sample.
            if (!c.open("buffer", videoArgs(f.get(), s), "cropdetect=limit=0.094:round=2:reset=0:skip=0", r.error)) return r;
        }
        f->pts = i;
        if (!c.push(f.get())) break;
        ++r.frames;
    }
    c.push(nullptr);
    if (!width || !c.has("lavfi.cropdetect.x1")) {
        r.error = "cropdetect gave no answer";
        return r;
    }
    const int x1 = int(c.meta("lavfi.cropdetect.x1")), x2 = int(c.meta("lavfi.cropdetect.x2"));
    const int y1 = int(c.meta("lavfi.cropdetect.y1")), y2 = int(c.meta("lavfi.cropdetect.y2"));
    if (x2 <= x1 || y2 <= y1) {
        r.error = "the whole picture looks black";
        return r;
    }
    r.left = std::max(0, x1);
    r.right = std::max(0, width - 1 - x2);
    r.top = std::max(0, y1);
    r.bottom = std::max(0, height - 1 - y2);
    if (progress) progress(1.0);
    r.ok = true;
    return r;
}

LoudnessResult runLoudness(const SourceInfo& s, const EditList& el, const DetectProgress& progress) {
    LoudnessResult r;
    if (!s.hasAudio) {
        r.error = "this source has no audio";
        return r;
    }
    AudioCutter cut;
    if (!cut.open(s, planAudio(el, s), r.error)) return r;
    const int ch = std::max(1, s.channels);
    AVChannelLayout layout;
    av_channel_layout_default(&layout, ch);
    char lay[64];
    av_channel_layout_describe(&layout, lay, sizeof lay);
    char args[256];
    std::snprintf(args, sizeof args, "sample_rate=%d:sample_fmt=s32:channel_layout=%s:time_base=1/%d", s.sampleRate, lay, s.sampleRate);
    Chain c;
    if (!c.open("abuffer", args, "ebur128=metadata=1", r.error)) {
        av_channel_layout_uninit(&layout);
        return r;
    }
    const int64_t total = cut.total();
    const int block = std::max(1, s.sampleRate / 10);
    std::vector<int32_t> buf;
    int32_t peak = 0;
    while (cut.produced() < total) {
        if (progress && !progress(double(cut.produced()) / double(std::max<int64_t>(1, total)))) {
            av_channel_layout_uninit(&layout);
            r.error = "cancelled";
            return r;
        }
        const int64_t at = cut.produced();
        if (!cut.next(block, buf) || buf.empty()) break;
        const int nb = int(buf.size() / size_t(ch));
        for (int32_t v : buf) {
            const int32_t m = v == INT32_MIN ? INT32_MAX : std::abs(v);
            peak = std::max(peak, m);
            // Within a 16-bit step of full scale counts as clipped.
            if (m >= INT32_MAX - 65536) ++r.clipped;
        }
        FramePtr f = makeFrame();
        f->format = AV_SAMPLE_FMT_S32;
        f->nb_samples = nb;
        f->sample_rate = s.sampleRate;
        av_channel_layout_copy(&f->ch_layout, &layout);
        if (av_frame_get_buffer(f.get(), 0) < 0) break;
        std::memcpy(f->data[0], buf.data(), buf.size() * sizeof(int32_t));
        f->pts = at;
        if (!c.push(f.get())) break;
        r.samples += nb;
    }
    c.push(nullptr);
    av_channel_layout_uninit(&layout);
    if (!c.has("lavfi.r128.I")) {
        r.error = "ebur128 gave no answer (under half a second of audio?)";
        return r;
    }
    r.integrated = c.meta("lavfi.r128.I", -70);
    r.range = c.meta("lavfi.r128.LRA");
    r.peakDb = peak > 0 ? 20.0 * std::log10(double(peak) / 2147483647.0) : -144;
    if (progress) progress(1.0);
    r.ok = true;
    return r;
}

}  // namespace ud

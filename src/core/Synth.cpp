// SPDX-License-Identifier: GPL-3.0-or-later
#include "Synth.h"

#include "shared/Json.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>

extern "C" {
#include <libavutil/channel_layout.h>
#include <libavutil/opt.h>
}

namespace ud {

namespace fs = std::filesystem;

namespace {
struct QuietLog {
    QuietLog() { quietFfmpegLog(); }
} g_quiet;
}  // namespace

static double smoothStep(double x, double sigma) { return 0.5 * (1.0 + std::erf(x / (sigma * M_SQRT2))); }

static uint64_t hmix(uint64_t x) {
    x ^= x >> 33;
    x *= 0xff51afd7ed558ccdULL;
    x ^= x >> 33;
    x *= 0xc4ceb9fe1a85ec53ULL;
    x ^= x >> 33;
    return x;
}

static double gauss(uint64_t key) {
    const uint64_t a = hmix(key), b = hmix(key ^ 0xA5A5A5A5DEADBEEFULL);
    const double u1 = (double(a >> 11) + 1.0) / double(1ULL << 53), u2 = double(b >> 11) / double(1ULL << 53);
    return std::sqrt(-2.0 * std::log(u1)) * std::cos(2.0 * M_PI * u2);
}

double sceneLuma(const SynthScene& s, int64_t field, int y, double x) {
    const double sg = s.sigma;
    double pic = s.base;
    const bool second = s.cutFrame >= 0 && field / 2 >= s.cutFrame;
    if (!second) {
        for (const auto& [bx, bw] : s.bars) pic += (s.bar - s.base) * (smoothStep(x - bx, sg) - smoothStep(x - (bx + bw), sg));
    } else {
        // After the cut: a different picture, darker with other bars.
        pic = s.base - 30;
        for (double bx : {100.0, 240.0, 520.0}) pic += (s.bar - s.base) * (smoothStep(x - bx, sg) - smoothStep(x - (bx + 25), sg));
    }
    if (s.panBar) {
        const double px = s.panX + s.panPerField * double(field);
        pic += (s.bar + 40 - s.base) * (smoothStep(x - px, sg) - smoothStep(x - (px + s.panW), sg));
    }
    if (s.box) {
        double x0 = 0, x1 = 0;
        if (s.box(field, x0, x1) && y >= s.boxY0 && y <= s.boxY1) {
            const double inside = smoothStep(x - x0, sg) - smoothStep(x - x1, sg);
            // Texture, like clothing or a hand: soft vertical stripes.
            const double tex = s.boxTexture * std::sin(2.0 * M_PI * (x - x0) / 7.0) * std::sin(0.31 * y);
            pic = pic * (1.0 - inside) + (s.boxLevel + tex) * inside;
        }
    }
    double gain = s.fade ? s.fade(field) : 1.0;
    pic = s.black + (pic - s.black) * gain;
    const double inPic = smoothStep(x - s.borderL, sg) * (s.rightBorder ? 1.0 - smoothStep(x - s.borderR, sg) : 1.0);
    return s.black + (pic - s.black) * inPic;
}

void renderFieldLuma(const SynthScene& s, const DamageModel& d, int64_t field, int parity, FieldLuma& out) {
    out.width = s.width;
    out.parity = parity;
    out.lines = (s.height - parity + 1) / 2;
    out.y.resize(size_t(out.width) * size_t(out.lines));
    for (int l = 0; l < out.lines; ++l) {
        const int y = 2 * l + parity;
        const double off = d.offset(field, y);
        float* row = out.row(l);
        for (int x = 0; x < s.width; ++x) {
            double v = sceneLuma(s, field, y, double(x) - off);
            if (s.noise > 0) v += s.noise * gauss((uint64_t(s.seed) << 40) ^ (uint64_t(field) << 20) ^ (uint64_t(y) << 10) ^ uint64_t(x));
            row[x] = float(v);
        }
    }
}

int64_t telecineFilmFrame(const SynthScene& s, int64_t n, int parity) {
    // AA BB BC CD DD (top, bottom) for film frames A B C D.
    static const int top[5] = {0, 1, 1, 2, 3}, bottom[5] = {0, 1, 2, 3, 3};
    const int64_t m = n + s.telecineOffset;
    const int64_t c = m / 5;
    const int p = int(m % 5);
    return 4 * c + (parity == 0 ? top[p] : bottom[p]) - 0;
}

void renderFrame(const SynthScene& s, const DamageModel& d, int64_t frame, FieldOrder order, Picture& out) {
    out.alloc(s.width, s.height, false);
    if (s.telecine) {
        SynthScene film = s;
        film.telecine = false;
        film.panBar = true;
        film.panPerField = 0;
        for (int y = 0; y < s.height; ++y) {
            const int64_t fi = telecineFilmFrame(s, frame, y & 1);
            film.panX = s.panX + s.panPerFilm * double(fi);
            float* row = out.p[0].row(y);
            for (int x = 0; x < s.width; ++x) row[x] = float(sceneLuma(film, 0, y, double(x)));
            std::fill(out.p[1].row(y), out.p[1].row(y) + s.width, 128.0f);
            std::fill(out.p[2].row(y), out.p[2].row(y) + s.width, 128.0f);
        }
        return;
    }
    const int p0 = firstParity(order);
    for (int k = 0; k < 2; ++k) {
        const int parity = k == 0 ? p0 : 1 - p0;
        FieldLuma fl;
        const int64_t field = 2 * frame + k;
        renderFieldLuma(s, d, field, parity, fl);
        for (int l = 0; l < fl.lines; ++l) {
            const int y = 2 * l + parity;
            std::copy(fl.row(l), fl.row(l) + s.width, out.p[0].row(y));
            const double off = d.offset(field, y);
            float* cb = out.p[1].row(y);
            float* cr = out.p[2].row(y);
            for (int x = 0; x < s.width; ++x) {
                // Chroma follows the bars, so a chroma move with luma is visible.
                const double lum = sceneLuma(s, field, y, double(x) - off) - s.base;
                cb[x] = float(128.0 - 0.25 * lum);
                cr[x] = float(128.0 + 0.3 * lum);
            }
        }
    }
}

TrackerConfig borderConfig(const SynthScene& s, const DamageModel& d, int64_t frames, bool right) {
    TrackerConfig c;
    c.width = s.width;
    c.frameHeight = s.height;
    c.order = FieldOrder::TFF;
    Picture f0;
    renderFrame(s, d, 0, c.order, f0);
    Reference r;
    r.id = 1;
    r.kind = RefKind::LeftBorder;
    r.name = "Left border";
    RefSpan sp;
    if (makeBorderSpan(f0, false, frames, c.s, sp)) {
        r.spans.push_back(sp);
        c.refs.push_back(r);
    }
    if (right) {
        Reference rr;
        rr.id = 2;
        rr.kind = RefKind::RightBorder;
        rr.name = "Right border";
        RefSpan sp2;
        if (makeBorderSpan(f0, true, frames, c.s, sp2)) {
            rr.spans.push_back(sp2);
            c.refs.push_back(rr);
        }
    }
    return c;
}

SynthRun runTracker(const SynthScene& s, const DamageModel& d, const TrackerConfig& cfg, int64_t frames) {
    SynthRun run;
    EdgeTracker t(cfg);
    t.onField = [&](FieldOffsets&& f) { run.fields.push_back(std::move(f)); };
    const int p0 = firstParity(cfg.order);
    for (int64_t fr = 0; fr < frames; ++fr)
        for (int k = 0; k < 2; ++k) {
            FieldLuma fl;
            renderFieldLuma(s, d, 2 * fr + k, k == 0 ? p0 : 1 - p0, fl);
            t.push(t.prepare(2 * fr + k, std::move(fl)));
        }
    t.finish();
    run.result = t.result();
    return run;
}

Residual residualOf(const SynthRun& r, const DamageModel& d, int64_t from, int64_t to) {
    Residual res;
    // Where a border "belongs" is only known up to one constant for the whole
    // clip (it is defined as its average position), and a constant shift of
    // the whole picture is no error at all. That constant comes out first.
    double bias = 0;
    int nb = 0;
    for (const FieldOffsets& f : r.fields) {
        if (f.field < from || (to >= 0 && f.field >= to)) continue;
        for (size_t l = 0; l < f.off.size(); ++l)
            if (f.conf[l] >= kConfMeasuredMin) {
                bias += double(f.off[l]) - d.offset(f.field, 2 * int(l) + f.parity);
                ++nb;
            }
    }
    res.bias = nb ? bias / nb : 0.0;
    double sm = 0, sa = 0;
    for (const FieldOffsets& f : r.fields) {
        if (f.field < from || (to >= 0 && f.field >= to)) continue;
        for (size_t l = 0; l < f.off.size(); ++l) {
            const int y = 2 * int(l) + f.parity;
            const double e = double(f.off[l]) - d.offset(f.field, y) - res.bias;
            sa += e * e;
            ++res.lines;
            if (f.conf[l] >= kConfMeasuredMin) {
                sm += e * e;
                ++res.measured;
                res.maxAbs = std::max(res.maxAbs, std::fabs(e));
            }
        }
    }
    res.rmsMeasured = res.measured ? std::sqrt(sm / res.measured) : 1e9;
    res.rmsAll = res.lines ? std::sqrt(sa / res.lines) : 1e9;
    return res;
}

bool ffprobeJson(const std::string& path, const std::string& args, JsonValue& out, std::string& err) {
    const char* env = std::getenv("UD_FFPROBE");
    const std::string probe = env && *env ? env : "ffprobe";
    // Only stdout is JSON; decoder complaints on stderr go to a side file,
    // shown only if ffprobe fails.
    std::string errFile = (fs::temp_directory_path() / "uindub-ffprobe-XXXXXX").string();
    const int fd = mkstemp(errFile.data());
    if (fd >= 0) ::close(fd);
    const std::string cmd = probe + " -v error -of json " + args + " '" + path + "' 2>'" + errFile + "'";
    FILE* p = popen(cmd.c_str(), "r");
    if (!p) {
        err = "cannot run ffprobe";
        std::remove(errFile.c_str());
        return false;
    }
    std::string text;
    char buf[4096];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof buf, p)) > 0) text.append(buf, n);
    const int rc = pclose(p);
    std::string stderrText;
    {
        std::ifstream in(errFile);
        stderrText.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    }
    std::remove(errFile.c_str());
    if (rc != 0) {
        err = "ffprobe failed: " + stderrText;
        return false;
    }
    return JsonValue::parse(text, out, err);
}

std::string tempDir(const std::string& name) {
    const char* t = std::getenv("TMPDIR");
    fs::path p = fs::path(t && *t ? t : "/tmp") / "uindub-tests" / name;
    std::error_code ec;
    fs::remove_all(p, ec);
    fs::create_directories(p, ec);
    return p.string();
}

// ---------------------------------------------------------------------------
// synthetic capture files

namespace {

struct Writer {
    AVFormatContext* oc = nullptr;
    AVCodecContext* vc = nullptr;
    AVCodecContext* ac = nullptr;
    AVStream* vs = nullptr;
    AVStream* as = nullptr;
    ~Writer() {
        if (vc) avcodec_free_context(&vc);
        if (ac) avcodec_free_context(&ac);
        if (oc) {
            if (oc->pb) avio_closep(&oc->pb);
            avformat_free_context(oc);
        }
    }
};

bool drain(Writer& w, AVCodecContext* c, AVStream* st) {
    AVPacket* p = av_packet_alloc();
    bool ok = true;
    while (avcodec_receive_packet(c, p) == 0) {
        av_packet_rescale_ts(p, c->time_base, st->time_base);
        p->stream_index = st->index;
        if (av_interleaved_write_frame(w.oc, p) < 0) ok = false;
    }
    av_packet_free(&p);
    return ok;
}

}  // namespace

bool writeSynthCapture(const SynthFile& f, const SynthScene& sceneIn, const DamageModel& d, std::string& err) {
    SynthScene scene = sceneIn;
    if (f.pal) {
        scene.height = 576;
        scene.width = 720;
    }
    if (f.moving && !scene.panBar) {
        scene.panBar = true;
        scene.panPerField = 3.0;
        scene.panX = 200;
    }
    const Rational fps = f.pal ? Rational{25, 1} : Rational{30000, 1001};
    const Rational sar = f.pal ? Rational{12, 11} : Rational{10, 11};
    const std::string base = f.path.substr(0, f.path.find_last_of('.'));
    const AVCodec* codec = avcodec_find_encoder_by_name(f.codec.c_str());
    if (!codec) {
        err = "no encoder " + f.codec;
        return false;
    }
    AVPixelFormat pix = AVPixelFormat(f.pixfmt);
    if (f.pixfmt < 0) {
        if (f.codec == "rawvideo") pix = AV_PIX_FMT_YUYV422;
        else if (f.codec == "mjpeg") pix = AV_PIX_FMT_YUVJ422P;
        else pix = AV_PIX_FMT_YUV422P;
    }
    SourceInfo timing;
    timing.fps = fps;
    timing.sampleRate = f.sampleRate;
    std::vector<std::string> files;
    std::vector<std::pair<int64_t, int64_t>> segs;
    const int nseg = std::max(1, f.segments);
    for (int s = 0; s < nseg; ++s) {
        const int64_t a = f.frames * s / nseg, b = f.frames * (s + 1) / nseg;
        segs.push_back({a, b - a});
        char name[64];
        std::snprintf(name, sizeof name, "_part%03d", s + 1);
        files.push_back(nseg == 1 ? f.path : base + name + "." + f.container);
    }
    for (int s = 0; s < nseg; ++s) {
        Writer w;
        if (avformat_alloc_output_context2(&w.oc, nullptr, f.container == "avi" ? "avi" : "matroska", files[size_t(s)].c_str()) < 0) {
            err = "no muxer";
            return false;
        }
        w.vs = avformat_new_stream(w.oc, nullptr);
        w.vc = avcodec_alloc_context3(codec);
        w.vc->width = scene.width;
        w.vc->height = scene.height;
        w.vc->pix_fmt = pix;
        w.vc->time_base = AVRational{fps.den, fps.num};
        w.vc->framerate = AVRational{fps.num, fps.den};
        w.vc->sample_aspect_ratio = AVRational{sar.num, sar.den};
        w.vc->field_order = f.tag == FieldOrder::TFF ? AV_FIELD_TT : (f.tag == FieldOrder::BFF ? AV_FIELD_BB : AV_FIELD_UNKNOWN);
        w.vc->color_range = f.codec == "mjpeg" ? AVCOL_RANGE_JPEG : AVCOL_RANGE_MPEG;
        w.vc->color_primaries = f.pal ? AVCOL_PRI_BT470BG : AVCOL_PRI_SMPTE170M;
        w.vc->color_trc = f.pal ? AVCOL_TRC_GAMMA28 : AVCOL_TRC_SMPTE170M;
        w.vc->colorspace = f.pal ? AVCOL_SPC_BT470BG : AVCOL_SPC_SMPTE170M;
        w.vc->gop_size = 1;
        if (f.codec == "ffv1") {
            av_opt_set_int(w.vc->priv_data, "slicecrc", 1, 0);
            w.vc->level = 3;
        }
        if (f.codec == "mjpeg") w.vc->strict_std_compliance = FF_COMPLIANCE_UNOFFICIAL;
        if (w.oc->oformat->flags & AVFMT_GLOBALHEADER) w.vc->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
        if (avcodec_open2(w.vc, codec, nullptr) < 0) {
            err = "cannot open encoder " + f.codec;
            return false;
        }
        avcodec_parameters_from_context(w.vs->codecpar, w.vc);
        w.vs->time_base = w.vc->time_base;
        w.vs->sample_aspect_ratio = w.vc->sample_aspect_ratio;
        w.vs->avg_frame_rate = w.vc->framerate;
        if (f.audio) {
            const AVCodec* pcm = avcodec_find_encoder(AV_CODEC_ID_PCM_S16LE);
            w.as = avformat_new_stream(w.oc, nullptr);
            w.ac = avcodec_alloc_context3(pcm);
            w.ac->sample_rate = f.sampleRate;
            av_channel_layout_default(&w.ac->ch_layout, f.channels);
            w.ac->sample_fmt = AV_SAMPLE_FMT_S16;
            w.ac->time_base = AVRational{1, f.sampleRate};
            if (avcodec_open2(w.ac, pcm, nullptr) < 0) {
                err = "cannot open pcm";
                return false;
            }
            avcodec_parameters_from_context(w.as->codecpar, w.ac);
            w.as->time_base = w.ac->time_base;
        }
        if (avio_open(&w.oc->pb, files[size_t(s)].c_str(), AVIO_FLAG_WRITE) < 0 || avformat_write_header(w.oc, nullptr) < 0) {
            err = "cannot write " + files[size_t(s)];
            return false;
        }
        const int64_t start = segs[size_t(s)].first, count = segs[size_t(s)].second;
        const int64_t s0 = timing.sampleOfFrame(start);
        for (int64_t n = start; n < start + count; ++n) {
            Picture pic;
            renderFrame(scene, d, n, f.truth == FieldOrder::BFF ? FieldOrder::BFF : FieldOrder::TFF, pic);
            FramePtr fr = fromPicture(pic, pix, true);
            if (!fr) {
                err = "convert failed";
                return false;
            }
            fr->pts = n - start;
            fr->sample_aspect_ratio = AVRational{sar.num, sar.den};
            fr->color_range = w.vc->color_range;
            setFrameInterlace(fr.get(), true, f.tag != FieldOrder::BFF);
            if (f.codec == "mjpeg") {
                // fromPicture writes limited values; MJPEG wants full range.
                for (int p = 0; p < 3; ++p) {
                    const int h = p ? scene.height : scene.height;
                    const int ww = p ? scene.width / 2 : scene.width;
                    for (int y = 0; y < h; ++y) {
                        uint8_t* row = fr->data[p] + size_t(y) * size_t(fr->linesize[p]);
                        for (int x = 0; x < ww; ++x) {
                            const double v = p == 0 ? (row[x] - 16.0) * 255.0 / 219.0 : 128.0 + (row[x] - 128.0) * 255.0 / 224.0;
                            row[x] = uint8_t(std::clamp(std::lround(v), 0L, 255L));
                        }
                    }
                }
            }
            if (avcodec_send_frame(w.vc, fr.get()) < 0 || !drain(w, w.vc, w.vs)) {
                err = "encode failed";
                return false;
            }
            if (f.audio) {
                const int64_t a = timing.sampleOfFrame(n), b = timing.sampleOfFrame(n + 1);
                AVFrame* af = av_frame_alloc();
                af->nb_samples = int(b - a);
                af->format = AV_SAMPLE_FMT_S16;
                af->sample_rate = f.sampleRate;
                av_channel_layout_copy(&af->ch_layout, &w.ac->ch_layout);
                av_frame_get_buffer(af, 0);
                int16_t* o = reinterpret_cast<int16_t*>(af->data[0]);
                for (int64_t k = a; k < b; ++k)
                    for (int c = 0; c < f.channels; ++c)
                        o[(k - a) * f.channels + c] = int16_t(std::lround(9000.0 * std::sin(2 * M_PI * 440.0 * double(k) / f.sampleRate + c)));
                af->pts = a - s0;
                avcodec_send_frame(w.ac, af);
                av_frame_free(&af);
                drain(w, w.ac, w.as);
            }
        }
        avcodec_send_frame(w.vc, nullptr);
        drain(w, w.vc, w.vs);
        if (f.audio) {
            avcodec_send_frame(w.ac, nullptr);
            drain(w, w.ac, w.as);
        }
        av_write_trailer(w.oc);
    }

    if (!f.pingdub) return true;
    const std::string cbase = nseg == 1 ? base : base;
    JsonValue j = JsonValue::object();
    j.set("pingdub_version", "1.0.0-test");
    j.set("capture_start", f.captureStart);
    j.set("tape_label", "Synthetic tape");
    j.set("stop_reason", "user");
    JsonValue dev = JsonValue::object();
    dev.set("name", "Synthetic").set("driver", f.driver).set("backend", "v4l2").set("input", "S-Video");
    j.set("device", dev);
    JsonValue vid = JsonValue::object();
    vid.set("standard", f.pal ? "PAL" : "NTSC").set("width", scene.width).set("height", scene.height);
    vid.set("frame_rate", std::to_string(fps.num) + "/" + std::to_string(fps.den));
    vid.set("field_order", fieldOrderName(f.tag));
    vid.set("sar", std::to_string(sar.num) + ":" + std::to_string(sar.den));
    j.set("video", vid);
    JsonValue segArr = JsonValue::array();
    for (int s = 0; s < nseg; ++s) {
        JsonValue g = JsonValue::object();
        g.set("file", files[size_t(s)]).set("start_frame", (long long)segs[size_t(s)].first).set("frames", (long long)segs[size_t(s)].second);
        segArr.push(g);
    }
    j.set("segments", segArr);
    JsonValue fl = JsonValue::array();
    for (const std::string& x : files) fl.push(x);
    j.set("files", fl);
    JsonValue tot = JsonValue::object();
    tot.set("frames", (long long)f.frames);
    j.set("totals", tot);
    JsonValue ch = JsonValue::array();
    if (f.chapters) {
        const int64_t mid = f.frames / 2;
        JsonValue c1 = JsonValue::object();
        c1.set("start", 0).set("end", (long long)mid).set("title", "Recording 1 (00:00:00)");
        JsonValue c2 = JsonValue::object();
        c2.set("start", (long long)mid).set("end", (long long)f.frames).set("title", "Recording 2");
        ch.push(c1).push(c2);
    }
    j.set("chapters", ch);
    std::string jerr;
    if (!writeJsonFile(cbase + ".json", j, jerr)) {
        err = jerr;
        return false;
    }
    std::ofstream csv(cbase + ".log.csv");
    csv << "wallclock,slot,timecode,event,count,detail\n";
    csv << "2026-10-29T10:00:00,,,capture_start,0,\"test\"\n";
    if (f.events) {
        csv << "2026-10-29T10:00:01," << f.frames / 3 << ",,insert,2,\"gap\"\n";
        csv << "2026-10-29T10:00:02," << f.frames / 2 << ",,signal_lost,1,\"no sync\"\n";
        csv << "2026-10-29T10:00:03," << f.frames / 2 + 2 << ",,error_frame,1,\"damaged\"\n";
    }
    csv << "2026-10-29T10:00:09," << f.frames << ",,capture_stop,0,\"user\"\n";
    if (f.captions) {
        std::ofstream srt(cbase + ".cc1.srt");
        const double dur = frameSeconds(f.frames, fps);
        srt << "1\n00:00:00,100 --> 00:00:00,400\nFIRST LINE\n\n";
        char buf[128];
        auto ts = [&](double t) {
            const int ms = int(std::lround(t * 1000));
            std::snprintf(buf, sizeof buf, "%02d:%02d:%02d,%03d", ms / 3600000, (ms / 60000) % 60, (ms / 1000) % 60, ms % 1000);
            return std::string(buf);
        };
        srt << "2\n" << ts(dur * 0.5) << " --> " << ts(dur * 0.5 + 0.3) << "\nMIDDLE LINE\n\n";
        srt << "3\n" << ts(dur * 0.85) << " --> " << ts(dur * 0.85 + 0.1) << "\nLATE LINE\n\n";
        std::ofstream scc(cbase + ".cc1.scc");
        scc << "Scenarist_SCC V1.0\n";
    }
    return true;
}

}  // namespace ud

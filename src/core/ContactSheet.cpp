// SPDX-License-Identifier: GPL-3.0-or-later
#include "ContactSheet.h"

#include "Av.h"
#include "Font8x8.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>

extern "C" {
#include <libavutil/imgutils.h>
#include <libavutil/pixdesc.h>
#include <libswscale/swscale.h>
}

namespace ud {

namespace fs = std::filesystem;

namespace {

constexpr int kGap = 8, kLabel = 22;
constexpr int kTitleScale = 3, kClockScale = 2;
constexpr int kHeader = 8 * kTitleScale + 2 * kGap;
constexpr uint8_t kBack = 40, kText = 235;

struct Page {
    int w = 0, h = 0;
    std::vector<uint8_t> rgb;
    void clear() { rgb.assign(size_t(w) * size_t(h) * 3, kBack); }
    uint8_t* at(int x, int y) { return rgb.data() + (size_t(y) * size_t(w) + size_t(x)) * 3; }
};

// UTF-8 in, ASCII out: anything the font has no glyph for becomes '?'.
std::string toAscii(const std::string& s) {
    std::string o;
    for (size_t i = 0; i < s.size(); ++i) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        if (c < 0x80) {
            o += (c >= kFontFirst && c <= kFontLast) ? char(c) : ' ';
            continue;
        }
        // Skip the continuation bytes of one code point.
        while (i + 1 < s.size() && (static_cast<unsigned char>(s[i + 1]) & 0xC0) == 0x80) ++i;
        o += '?';
    }
    return o;
}

int textWidth(const std::string& t, int scale) { return int(t.size()) * 8 * scale; }

void drawText(Page& p, int x, int y, const std::string& t, int scale, uint8_t v) {
    for (char ch : t) {
        const int g = int(static_cast<unsigned char>(ch)) - kFontFirst;
        if (g >= 0 && g <= kFontLast - kFontFirst)
            for (int r = 0; r < 8; ++r)
                for (int c = 0; c < 8; ++c) {
                    if (!(kFont8x8[g][r] & (1 << c))) continue;
                    for (int dy = 0; dy < scale; ++dy)
                        for (int dx = 0; dx < scale; ++dx) {
                            const int px = x + c * scale + dx, py = y + r * scale + dy;
                            if (px < 0 || py < 0 || px >= p.w || py >= p.h) continue;
                            uint8_t* d = p.at(px, py);
                            d[0] = d[1] = d[2] = v;
                        }
                }
        x += 8 * scale;
    }
}

int swsMatrixOf(const AVFrame* f) {
    switch (f->colorspace) {
    case AVCOL_SPC_BT709: return SWS_CS_ITU709;
    case AVCOL_SPC_BT470BG: return SWS_CS_ITU601;
    default: return SWS_CS_SMPTE170M;
    }
}

bool writeJpeg(Page& page, int usedHeight, const std::string& path, std::string& err) {
    const int h = std::min(page.h, (usedHeight + 1) & ~1);
    const AVCodec* c = avcodec_find_encoder(AV_CODEC_ID_MJPEG);
    if (!c) {
        err = "this FFmpeg build has no JPEG encoder";
        return false;
    }
    CodecPtr ctx;
    AVPixelFormat used = AV_PIX_FMT_NONE;
    for (AVPixelFormat pf : {AV_PIX_FMT_YUV420P, AV_PIX_FMT_YUVJ420P}) {
        ctx.reset(avcodec_alloc_context3(c));
        ctx->width = page.w;
        ctx->height = h;
        ctx->pix_fmt = pf;
        ctx->color_range = AVCOL_RANGE_JPEG;
        ctx->time_base = AVRational{1, 25};
        ctx->flags |= AV_CODEC_FLAG_QSCALE;
        ctx->global_quality = FF_QP2LAMBDA * 3;
        if (avcodec_open2(ctx.get(), c, nullptr) == 0) {
            used = pf;
            break;
        }
        ctx.reset();
    }
    if (!ctx) {
        err = "cannot open the JPEG encoder";
        return false;
    }
    FramePtr fr = makeFrame();
    fr->format = used;
    fr->width = page.w;
    fr->height = h;
    fr->color_range = AVCOL_RANGE_JPEG;
    if (av_frame_get_buffer(fr.get(), 32) < 0) {
        err = "out of memory";
        return false;
    }
    SwsContext* sws = sws_getContext(page.w, h, AV_PIX_FMT_RGB24, page.w, h, used, SWS_BICUBIC | SWS_ACCURATE_RND, nullptr, nullptr, nullptr);
    if (!sws) {
        err = "cannot convert the page";
        return false;
    }
    const int* m = sws_getCoefficients(SWS_CS_ITU601);
    sws_setColorspaceDetails(sws, m, 1, m, 1, 0, 1 << 16, 1 << 16);
    const uint8_t* src[1] = {page.rgb.data()};
    const int stride[1] = {page.w * 3};
    sws_scale(sws, src, stride, 0, h, fr->data, fr->linesize);
    sws_freeContext(sws);
    fr->pts = 0;
    fr->quality = ctx->global_quality;
    PacketPtr pkt = makePacket();
    if (avcodec_send_frame(ctx.get(), fr.get()) < 0 || avcodec_send_frame(ctx.get(), nullptr) < 0 || avcodec_receive_packet(ctx.get(), pkt.get()) < 0) {
        err = "the JPEG encoder failed";
        return false;
    }
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) {
        err = "cannot write " + path;
        return false;
    }
    bool ok = std::fwrite(pkt->data, 1, size_t(pkt->size), f) == size_t(pkt->size);
    ok = std::fclose(f) == 0 && ok;
    if (!ok) err = "cannot write " + path;
    return ok;
}

}  // namespace

std::string contactPagePath(const std::string& videoPath, int page) {
    const fs::path p(videoPath);
    char suffix[32];
    std::snprintf(suffix, sizeof suffix, ".contact%02d.jpg", page);
    return (p.parent_path() / (p.stem().string() + suffix)).string();
}

std::string runtimeString(double seconds) {
    const long long t = std::max(0LL, (long long)std::floor(seconds + 1e-6));
    char buf[32];
    std::snprintf(buf, sizeof buf, "%lld:%02lld:%02lld", t / 3600, (t / 60) % 60, t % 60);
    return buf;
}

std::string contactHeader(const std::string& title, double seconds) { return title + " - " + runtimeString(seconds); }

ContactSheetResult writeContactSheet(const std::string& videoPath, const ContactSheetOptions& o, const std::function<bool(double)>& progress) {
    ContactSheetResult res;
    AVFormatContext* raw = nullptr;
    if (avformat_open_input(&raw, videoPath.c_str(), nullptr, nullptr) < 0) {
        res.error = "cannot open " + videoPath;
        return res;
    }
    InputPtr in(raw);
    if (avformat_find_stream_info(in.get(), nullptr) < 0) {
        res.error = "cannot read " + videoPath;
        return res;
    }
    const int vi = av_find_best_stream(in.get(), AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (vi < 0) {
        res.error = "no video in " + videoPath;
        return res;
    }
    for (unsigned k = 0; k < in->nb_streams; ++k)
        if (int(k) != vi) in->streams[k]->discard = AVDISCARD_ALL;
    AVStream* st = in->streams[vi];
    const AVCodec* dec = avcodec_find_decoder(st->codecpar->codec_id);
    if (!dec) {
        res.error = "no decoder for " + videoPath;
        return res;
    }
    CodecPtr dc(avcodec_alloc_context3(dec));
    avcodec_parameters_to_context(dc.get(), st->codecpar);
    dc->thread_count = 0;
    if (avcodec_open2(dc.get(), dec, nullptr) < 0) {
        res.error = "cannot open the decoder";
        return res;
    }

    double runtime = o.runtime;
    if (runtime <= 0) {
        if (st->duration != AV_NOPTS_VALUE) runtime = double(st->duration) * av_q2d(st->time_base);
        else if (in->duration != AV_NOPTS_VALUE) runtime = double(in->duration) / AV_TIME_BASE;
    }
    const double interval = std::max(0.04, o.interval);
    std::vector<double> times;
    for (double t = 0; t < runtime - 1e-3 || times.empty(); t += interval) times.push_back(t);

    // Tile size from the display aspect.
    const int W = st->codecpar->width, H = st->codecpar->height;
    const AVRational sarR = st->codecpar->sample_aspect_ratio.num ? st->codecpar->sample_aspect_ratio : st->sample_aspect_ratio;
    const double sar = sarR.num > 0 && sarR.den > 0 ? av_q2d(sarR) : 1.0;
    const double dar = H > 0 ? double(W) * sar / double(H) : 4.0 / 3.0;
    const int tw = o.tileWidth;
    const int th = std::clamp(int(std::lround(double(tw) / std::max(0.1, dar) / 2.0)) * 2, 16, 4 * tw);
    const int cols = std::max(1, o.columns), rows = std::max(1, o.rows);

    Page page;
    page.w = cols * tw + (cols + 1) * kGap;
    page.h = kHeader + rows * (th + kLabel) + rows * kGap + kGap;
    std::string header = toAscii(contactHeader(o.title.empty() ? fs::path(videoPath).stem().string() : o.title, runtime));
    // A title wider than the page is cut short; the runtime stays.
    const int room = page.w - 2 * kGap;
    if (textWidth(header, kTitleScale) > room) {
        const std::string tail = " - " + runtimeString(runtime);
        const size_t keep = size_t(std::max(0, room / (8 * kTitleScale) - int(tail.size()) - 3));
        header = header.substr(0, std::min(keep, header.size() - tail.size())) + "..." + tail;
    }
    auto newPage = [&] {
        page.clear();
        drawText(page, (page.w - textWidth(header, kTitleScale)) / 2, kGap, header, kTitleScale, kText);
    };

    PacketPtr pkt = makePacket();
    FramePtr frame = makeFrame();
    SwsContext* sws = nullptr;
    std::vector<uint8_t> tile(size_t(tw) * size_t(th) * 3);
    double decodedTo = -1;  // pts (seconds) of the last decoded frame
    bool eof = false;
    // The first decoded frame at or after `t`.
    auto frameAt = [&](double t) -> bool {
        if (t < decodedTo - 1e-6 || t > decodedTo + 3.0 || eof) {
            const int64_t ts = int64_t(std::floor(t / av_q2d(st->time_base)));
            av_seek_frame(in.get(), vi, ts, AVSEEK_FLAG_BACKWARD);
            avcodec_flush_buffers(dc.get());
            eof = false;
            decodedTo = -1;
        }
        for (;;) {
            const int r = avcodec_receive_frame(dc.get(), frame.get());
            if (r == 0) {
                const int64_t pts = frame->best_effort_timestamp != AV_NOPTS_VALUE ? frame->best_effort_timestamp : frame->pts;
                decodedTo = pts == AV_NOPTS_VALUE ? decodedTo + 1.0 / 30.0 : double(pts) * av_q2d(st->time_base);
                if (decodedTo + 1e-3 >= t) return true;
                av_frame_unref(frame.get());
                continue;
            }
            if (r == AVERROR_EOF) {
                eof = true;
                return false;
            }
            if (r != AVERROR(EAGAIN)) return false;
            const int rr = av_read_frame(in.get(), pkt.get());
            if (rr < 0) {
                avcodec_send_packet(dc.get(), nullptr);
                continue;
            }
            if (pkt->stream_index == vi) avcodec_send_packet(dc.get(), pkt.get());
            av_packet_unref(pkt.get());
        }
    };

    int onPage = 0;
    int pageNo = 0;
    auto flush = [&](int rowsUsed) -> bool {
        const int used = kHeader + rowsUsed * (th + kLabel + kGap) + kGap;
        const std::string path = contactPagePath(videoPath, ++pageNo);
        std::string e;
        if (!writeJpeg(page, used, path, e)) {
            res.error = e;
            return false;
        }
        res.pages.push_back(path);
        return true;
    };
    newPage();
    bool failed = false;
    for (size_t i = 0; i < times.size(); ++i) {
        if (progress && !progress(double(i) / double(times.size()))) {
            sws_freeContext(sws);
            res.error = "cancelled";
            return res;
        }
        const bool have = frameAt(times[i]);
        const int col = onPage % cols, row = onPage / cols;
        const int x = kGap + col * (tw + kGap), y = kHeader + row * (th + kLabel + kGap);
        if (have) {
            sws = sws_getCachedContext(sws, frame->width, frame->height, AVPixelFormat(frame->format), tw, th, AV_PIX_FMT_RGB24,
                                       SWS_AREA | SWS_ACCURATE_RND, nullptr, nullptr, nullptr);
            if (sws) {
                const int* m = sws_getCoefficients(swsMatrixOf(frame.get()));
                sws_setColorspaceDetails(sws, m, frame->color_range == AVCOL_RANGE_JPEG, sws_getCoefficients(SWS_CS_ITU601), 1, 0, 1 << 16, 1 << 16);
                uint8_t* dst[1] = {tile.data()};
                const int ds[1] = {tw * 3};
                sws_scale(sws, frame->data, frame->linesize, 0, frame->height, dst, ds);
                for (int r = 0; r < th; ++r) std::memcpy(page.at(x, y + r), tile.data() + size_t(r) * size_t(tw) * 3, size_t(tw) * 3);
            }
            av_frame_unref(frame.get());
        }
        const std::string clock = runtimeString(times[i]);
        drawText(page, x + (tw - textWidth(clock, kClockScale)) / 2, y + th + (kLabel - 8 * kClockScale) / 2, clock, kClockScale, kText);
        ++res.thumbs;
        if (++onPage == cols * rows) {
            if (!flush(rows)) {
                failed = true;
                break;
            }
            onPage = 0;
            newPage();
        }
    }
    sws_freeContext(sws);
    if (!failed && onPage > 0 && !flush((onPage + cols - 1) / cols)) failed = true;
    if (failed) return res;
    // Pages from an earlier, longer render of this name would be misleading.
    for (int k = pageNo + 1; k <= 99; ++k) {
        std::error_code ec;
        const std::string stale = contactPagePath(videoPath, k);
        if (!fs::exists(stale, ec)) break;
        fs::remove(stale, ec);
    }
    if (progress) progress(1.0);
    res.ok = true;
    return res;
}

}  // namespace ud

// SPDX-License-Identifier: GPL-3.0-or-later
// Small RAII wrappers around FFmpeg objects.
#pragma once

#include <memory>
#include <string>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/frame.h>
}

namespace ud {

struct FrameDeleter {
    void operator()(AVFrame* f) const { av_frame_free(&f); }
};
using FramePtr = std::unique_ptr<AVFrame, FrameDeleter>;
inline FramePtr makeFrame() { return FramePtr(av_frame_alloc()); }
// A new reference to the same buffers.
inline FramePtr refFrame(const AVFrame* f) {
    FramePtr r(av_frame_alloc());
    if (r && av_frame_ref(r.get(), f) < 0) r.reset();
    return r;
}
using SharedFrame = std::shared_ptr<const AVFrame>;
inline SharedFrame shareFrame(FramePtr f) {
    return SharedFrame(f.release(), [](const AVFrame* p) {
        AVFrame* q = const_cast<AVFrame*>(p);
        av_frame_free(&q);
    });
}

struct PacketDeleter {
    void operator()(AVPacket* p) const { av_packet_free(&p); }
};
using PacketPtr = std::unique_ptr<AVPacket, PacketDeleter>;
inline PacketPtr makePacket() { return PacketPtr(av_packet_alloc()); }

struct CodecDeleter {
    void operator()(AVCodecContext* c) const { avcodec_free_context(&c); }
};
using CodecPtr = std::unique_ptr<AVCodecContext, CodecDeleter>;

struct InputDeleter {
    void operator()(AVFormatContext* c) const { avformat_close_input(&c); }
};
using InputPtr = std::unique_ptr<AVFormatContext, InputDeleter>;

std::string avError(int code);
// FFmpeg's own log down to errors only (UINDUB_FFMPEG_LOG=1 keeps it all).
void quietFfmpegLog();

// Interlace flags moved into AVFrame.flags in FFmpeg 6.1; both spellings work here.
bool frameInterlaced(const AVFrame* f);
bool frameTopFirst(const AVFrame* f);
void setFrameInterlace(AVFrame* f, bool interlaced, bool topFirst);

}  // namespace ud

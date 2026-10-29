// SPDX-License-Identifier: GPL-3.0-or-later
#include "Output.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>

extern "C" {
#include <libavutil/channel_layout.h>
#include <libavutil/opt.h>
#include <libavutil/pixdesc.h>
}

namespace ud {

namespace fs = std::filesystem;

const char* videoCodecName(VideoCodec c) {
    switch (c) {
    case VideoCodec::X265: return "x265";
    case VideoCodec::X264: return "x264";
    case VideoCodec::SvtAv1: return "svt-av1";
    case VideoCodec::Ffv1: return "ffv1";
    }
    return "x265";
}

VideoCodec videoCodecFromName(const std::string& s) {
    if (s == "x264") return VideoCodec::X264;
    if (s == "svt-av1" || s == "av1") return VideoCodec::SvtAv1;
    if (s == "ffv1") return VideoCodec::Ffv1;
    return VideoCodec::X265;
}

const char* audioCodecName(AudioCodec c) {
    switch (c) {
    case AudioCodec::Flac: return "flac";
    case AudioCodec::Opus: return "opus";
    case AudioCodec::Aac: return "aac";
    case AudioCodec::Copy: return "copy";
    }
    return "flac";
}

AudioCodec audioCodecFromName(const std::string& s) {
    if (s == "opus") return AudioCodec::Opus;
    if (s == "aac") return AudioCodec::Aac;
    if (s == "copy") return AudioCodec::Copy;
    return AudioCodec::Flac;
}

int rfMax(VideoCodec c) { return c == VideoCodec::SvtAv1 ? 63 : (c == VideoCodec::Ffv1 ? 0 : 51); }
int rfDefault(VideoCodec c) {
    switch (c) {
    case VideoCodec::X265: return 15;
    case VideoCodec::X264: return 18;
    case VideoCodec::SvtAv1: return 28;
    default: return 0;
    }
}
std::string presetDefault(VideoCodec c) { return c == VideoCodec::SvtAv1 ? "6" : (c == VideoCodec::Ffv1 ? "" : "slow"); }
std::vector<std::string> presetsFor(VideoCodec c) {
    if (c == VideoCodec::SvtAv1) {
        std::vector<std::string> v;
        for (int i = 0; i <= 13; ++i) v.push_back(std::to_string(i));
        return v;
    }
    if (c == VideoCodec::Ffv1) return {};
    return {"ultrafast", "superfast", "veryfast", "faster", "fast", "medium", "slow", "slower", "veryslow", "placebo"};
}

bool OutputSettings::operator==(const OutputSettings& o) const {
    return codec == o.codec && rf == o.rf && preset == o.preset && chroma422 == o.chroma422 && audio == o.audio && opusKbps == o.opusKbps &&
           aacKbps == o.aacKbps && mp4 == o.mp4 && folder == o.folder && name == o.name && chapters == o.chapters && captions == o.captions;
}

OutputSettings effectiveOutput(const OutputSettings& in) {
    OutputSettings o = in;
    if (o.mp4) {
        // MP4 players handle FLAC and subtitle tracks badly.
        if (o.audio == AudioCodec::Flac || o.audio == AudioCodec::Copy) o.audio = AudioCodec::Aac;
        o.captions = false;
    }
    if (o.codec == VideoCodec::SvtAv1) o.chroma422 = false;
    return o;
}

AVPixelFormat encoderPixelFormat(const OutputSettings& o, const SourceInfo& s) {
    switch (o.codec) {
    case VideoCodec::X265: return s.gray ? AV_PIX_FMT_GRAY10LE : (o.chroma422 ? AV_PIX_FMT_YUV422P10LE : AV_PIX_FMT_YUV420P10LE);
    case VideoCodec::X264: return s.gray ? AV_PIX_FMT_GRAY8 : (o.chroma422 ? AV_PIX_FMT_YUV422P : AV_PIX_FMT_YUV420P);
    case VideoCodec::SvtAv1: return AV_PIX_FMT_YUV420P10LE;
    case VideoCodec::Ffv1: {
        // The source's own layout when FFV1 can store it, else 4:2:2 at its depth.
        const AVPixelFormat src = AVPixelFormat(s.pixfmt);
        const AVCodec* c = avcodec_find_encoder(AV_CODEC_ID_FFV1);
        const enum AVPixelFormat* fmts = nullptr;
#if LIBAVCODEC_VERSION_INT >= AV_VERSION_INT(61, 13, 100)
        int n = 0;
        avcodec_get_supported_config(nullptr, c, AV_CODEC_CONFIG_PIX_FORMAT, 0, reinterpret_cast<const void**>(&fmts), &n);
#else
        fmts = c ? c->pix_fmts : nullptr;
#endif
        for (const enum AVPixelFormat* p = fmts; p && *p != AV_PIX_FMT_NONE; ++p)
            if (*p == src) return src;
        if (s.gray) return s.bitDepth > 8 ? AV_PIX_FMT_GRAY10LE : AV_PIX_FMT_GRAY8;
        return s.bitDepth > 8 ? AV_PIX_FMT_YUV422P10LE : AV_PIX_FMT_YUV422P;
    }
    }
    return AV_PIX_FMT_YUV420P10LE;
}

std::string defaultOutputName(const SourceInfo& s) {
    // The capture's name without extension or segment number.
    const std::string stem = !s.basePath.empty() ? fs::path(s.basePath).filename().string() : fs::path(s.path).stem().string();
    return stem + ".uindub";
}

std::string outputPath(const OutputSettings& o, const SourceInfo& s) {
    std::string name = o.name;
    if (name.empty()) name = defaultOutputName(s);
    std::string folder = o.folder;
    if (folder.empty()) folder = fs::path(s.path).parent_path().string();
    return (fs::path(folder) / (name + (effectiveOutput(o).mp4 ? ".mp4" : ".mkv"))).string();
}

// ---------------------------------------------------------------------------

struct OutputWriter::Impl {
    AVFormatContext* oc = nullptr;
    CodecPtr vc, ac;
    AVStream* vs = nullptr;
    AVStream* as = nullptr;
    std::vector<AVStream*> subs;
    PacketPtr pkt = makePacket();
    int64_t vframes = 0, asamples = 0;
    std::vector<int32_t> fifo;  // pending interleaved samples
    int channels = 0, bits = 16;
    FieldOrder field = FieldOrder::Progressive;
    bool headerWritten = false;
    std::string path;

    ~Impl() {
        if (oc) {
            if (oc->pb && !(oc->oformat->flags & AVFMT_NOFILE)) avio_closep(&oc->pb);
            avformat_free_context(oc);
        }
    }

    bool drain(AVCodecContext* c, AVStream* st, std::string& err) {
        for (;;) {
            const int r = avcodec_receive_packet(c, pkt.get());
            if (r == AVERROR(EAGAIN) || r == AVERROR_EOF) return true;
            if (r < 0) {
                err = "encoder: " + avError(r);
                return false;
            }
            av_packet_rescale_ts(pkt.get(), c->time_base, st->time_base);
            pkt->stream_index = st->index;
            const int w = av_interleaved_write_frame(oc, pkt.get());
            if (w < 0) {
                err = "write: " + avError(w);
                return false;
            }
        }
    }

    bool encodeAudioFrame(int n, std::string& err) {
        FramePtr f = makeFrame();
        f->nb_samples = n;
        f->format = ac->sample_fmt;
        f->sample_rate = ac->sample_rate;
        av_channel_layout_copy(&f->ch_layout, &ac->ch_layout);
        if (av_frame_get_buffer(f.get(), 0) < 0) {
            err = "out of memory";
            return false;
        }
        const AVSampleFormat fmt = ac->sample_fmt;
        const bool planar = av_sample_fmt_is_planar(fmt);
        const AVSampleFormat packed = av_get_packed_sample_fmt(fmt);
        const int avail = int(std::min<size_t>(size_t(n), fifo.size() / size_t(channels)));
        for (int s = 0; s < n; ++s)
            for (int c = 0; c < channels; ++c) {
                const int32_t v = s < avail ? fifo[size_t(s) * size_t(channels) + size_t(c)] : 0;
                const int idx = planar ? s : s * channels + c;
                uint8_t* plane = f->extended_data[planar ? c : 0];
                switch (packed) {
                case AV_SAMPLE_FMT_S16: reinterpret_cast<int16_t*>(plane)[idx] = int16_t(v >> 16); break;
                case AV_SAMPLE_FMT_S32: reinterpret_cast<int32_t*>(plane)[idx] = v; break;
                case AV_SAMPLE_FMT_FLT: reinterpret_cast<float*>(plane)[idx] = float(double(v) / 2147483648.0); break;
                default: break;
                }
            }
        fifo.erase(fifo.begin(), fifo.begin() + long(size_t(avail) * size_t(channels)));
        f->pts = asamples;
        asamples += n;
        const int r = avcodec_send_frame(ac.get(), f.get());
        if (r < 0) {
            err = "audio encoder: " + avError(r);
            return false;
        }
        return drain(ac.get(), as, err);
    }
};

OutputWriter::OutputWriter() = default;
OutputWriter::~OutputWriter() { abort(); }

void OutputWriter::abort() {
    if (!m_i) return;
    const std::string p = m_i->path;
    const bool started = m_i->headerWritten;
    m_i.reset();
    if (started) std::remove(p.c_str());
}

int64_t OutputWriter::videoFrames() const { return m_i ? m_i->vframes : 0; }
int64_t OutputWriter::audioSamples() const { return m_i ? m_i->asamples : 0; }
uint64_t OutputWriter::bytes() const { return m_i && m_i->oc && m_i->oc->pb ? uint64_t(avio_tell(m_i->oc->pb)) : 0; }

bool OutputWriter::open(const std::string& path, const OutputSettings& oIn, const MuxVideo& v, const MuxAudio& a,
                        const std::vector<OutputChapter>& chapters, const std::vector<OutputCaptionTrack>& captions,
                        const std::map<std::string, std::string>& tags, std::string& err) {
    abort();
    const OutputSettings o = effectiveOutput(oIn);
    m_i = std::make_unique<Impl>();
    Impl& I = *m_i;
    I.path = path;
    I.field = v.field;
    if (avformat_alloc_output_context2(&I.oc, nullptr, o.mp4 ? "mp4" : "matroska", path.c_str()) < 0 || !I.oc) {
        err = "no muxer";
        return false;
    }

    // --- video
    const char* encName = nullptr;
    switch (o.codec) {
    case VideoCodec::X265: encName = "libx265"; break;
    case VideoCodec::X264: encName = "libx264"; break;
    case VideoCodec::SvtAv1: encName = "libsvtav1"; break;
    case VideoCodec::Ffv1: encName = "ffv1"; break;
    }
    const AVCodec* vcodec = avcodec_find_encoder_by_name(encName);
    if (!vcodec) {
        err = std::string("this build has no ") + encName + " encoder";
        return false;
    }
    I.vc.reset(avcodec_alloc_context3(vcodec));
    AVCodecContext* vc = I.vc.get();
    vc->width = v.width;
    vc->height = v.height;
    vc->pix_fmt = v.pixfmt;
    vc->time_base = AVRational{v.fps.den, v.fps.num};
    vc->framerate = AVRational{v.fps.num, v.fps.den};
    vc->sample_aspect_ratio = AVRational{v.sar.num, v.sar.den};
    vc->color_range = AVCOL_RANGE_MPEG;
    if (v.color == ColorStd::Bt470bg) {
        vc->color_primaries = AVCOL_PRI_BT470BG;
        vc->color_trc = AVCOL_TRC_GAMMA28;
        vc->colorspace = AVCOL_SPC_BT470BG;
    } else {
        vc->color_primaries = AVCOL_PRI_SMPTE170M;
        vc->color_trc = AVCOL_TRC_SMPTE170M;
        vc->colorspace = AVCOL_SPC_SMPTE170M;
    }
    vc->chroma_sample_location = AVCHROMA_LOC_LEFT;
    vc->field_order = v.field == FieldOrder::TFF ? AV_FIELD_TT : (v.field == FieldOrder::BFF ? AV_FIELD_BB : AV_FIELD_PROGRESSIVE);
    vc->thread_count = 0;
    if (I.oc->oformat->flags & AVFMT_GLOBALHEADER) vc->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    AVDictionary* vopts = nullptr;
    switch (o.codec) {
    case VideoCodec::X265:
        av_dict_set(&vopts, "crf", std::to_string(o.rf).c_str(), 0);
        av_dict_set(&vopts, "preset", o.preset.empty() ? "slow" : o.preset.c_str(), 0);
        av_dict_set(&vopts, "x265-params", "log-level=error", 0);
        break;
    case VideoCodec::X264:
        av_dict_set(&vopts, "crf", std::to_string(o.rf).c_str(), 0);
        av_dict_set(&vopts, "preset", o.preset.empty() ? "slow" : o.preset.c_str(), 0);
        if (v.field == FieldOrder::TFF || v.field == FieldOrder::BFF) vc->flags |= AV_CODEC_FLAG_INTERLACED_DCT | AV_CODEC_FLAG_INTERLACED_ME;
        break;
    case VideoCodec::SvtAv1:
        av_dict_set(&vopts, "crf", std::to_string(o.rf).c_str(), 0);
        av_dict_set(&vopts, "preset", o.preset.empty() ? "6" : o.preset.c_str(), 0);
        break;
    case VideoCodec::Ffv1:
        vc->level = 3;
        vc->gop_size = 1;
        av_dict_set(&vopts, "slicecrc", "1", 0);
        av_dict_set(&vopts, "coder", "range_def", 0);
        break;
    }
    int r = avcodec_open2(vc, vcodec, &vopts);
    av_dict_free(&vopts);
    if (r < 0) {
        err = std::string("cannot open ") + encName + ": " + avError(r);
        return false;
    }
    I.vs = avformat_new_stream(I.oc, nullptr);
    avcodec_parameters_from_context(I.vs->codecpar, vc);
    I.vs->time_base = vc->time_base;
    I.vs->avg_frame_rate = vc->framerate;
    I.vs->sample_aspect_ratio = vc->sample_aspect_ratio;

    // --- audio
    if (a.present) {
        I.channels = a.channels;
        I.bits = a.bits;
        const AVCodec* acodec = nullptr;
        AVSampleFormat sf = AV_SAMPLE_FMT_S16;
        switch (o.audio) {
        case AudioCodec::Flac:
            acodec = avcodec_find_encoder(AV_CODEC_ID_FLAC);
            sf = a.bits > 16 ? AV_SAMPLE_FMT_S32 : AV_SAMPLE_FMT_S16;
            break;
        case AudioCodec::Opus:
            acodec = avcodec_find_encoder_by_name("libopus");
            sf = AV_SAMPLE_FMT_FLT;
            if (a.rate != 48000 && a.rate != 24000 && a.rate != 16000 && a.rate != 12000 && a.rate != 8000) {
                err = "Opus needs 48 kHz audio and this source is " + std::to_string(a.rate) + " Hz; UinDub never resamples. Pick FLAC or AAC.";
                return false;
            }
            break;
        case AudioCodec::Aac:
            acodec = avcodec_find_encoder(AV_CODEC_ID_AAC);
            sf = AV_SAMPLE_FMT_FLTP;
            break;
        case AudioCodec::Copy:
            acodec = avcodec_find_encoder(a.bits > 24 ? AV_CODEC_ID_PCM_S32LE : (a.bits > 16 ? AV_CODEC_ID_PCM_S24LE : AV_CODEC_ID_PCM_S16LE));
            sf = a.bits > 16 ? AV_SAMPLE_FMT_S32 : AV_SAMPLE_FMT_S16;
            break;
        }
        if (!acodec) {
            err = std::string("this build has no ") + audioCodecName(o.audio) + " encoder";
            return false;
        }
        I.ac.reset(avcodec_alloc_context3(acodec));
        AVCodecContext* ac = I.ac.get();
        ac->sample_rate = a.rate;
        av_channel_layout_default(&ac->ch_layout, a.channels);
        ac->sample_fmt = sf;
        ac->time_base = AVRational{1, a.rate};
        if (sf == AV_SAMPLE_FMT_S32 && a.bits > 16) ac->bits_per_raw_sample = std::min(32, a.bits > 24 ? 32 : 24);
        if (o.audio == AudioCodec::Opus) ac->bit_rate = int64_t(o.opusKbps) * 1000;
        if (o.audio == AudioCodec::Aac) ac->bit_rate = int64_t(o.aacKbps) * 1000;
        if (o.audio == AudioCodec::Flac) ac->compression_level = 8;
        if (I.oc->oformat->flags & AVFMT_GLOBALHEADER) ac->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
        r = avcodec_open2(ac, acodec, nullptr);
        if (r < 0) {
            err = std::string("cannot open the ") + audioCodecName(o.audio) + " encoder: " + avError(r);
            return false;
        }
        I.as = avformat_new_stream(I.oc, nullptr);
        avcodec_parameters_from_context(I.as->codecpar, ac);
        I.as->time_base = ac->time_base;
    }

    // --- captions (MKV)
    if (o.captions && !o.mp4) {
        for (const OutputCaptionTrack& t : captions) {
            if (t.cues.empty()) continue;
            AVStream* s = avformat_new_stream(I.oc, nullptr);
            s->codecpar->codec_type = AVMEDIA_TYPE_SUBTITLE;
            s->codecpar->codec_id = AV_CODEC_ID_SUBRIP;
            s->time_base = AVRational{1, 1000};
            av_dict_set(&s->metadata, "title", t.name.c_str(), 0);
            av_dict_set(&s->metadata, "language", "eng", 0);
            I.subs.push_back(s);
        }
    }

    // --- chapters
    if (o.chapters) {
        int id = 1;
        for (const OutputChapter& c : chapters) {
            AVChapter* ch = static_cast<AVChapter*>(av_mallocz(sizeof(AVChapter)));
            if (!ch) break;
            ch->id = id++;
            ch->time_base = AVRational{1, 1000};
            ch->start = int64_t(std::llround(c.start * 1000.0));
            ch->end = int64_t(std::llround(c.end * 1000.0));
            av_dict_set(&ch->metadata, "title", c.title.c_str(), 0);
            AVChapter** grown = static_cast<AVChapter**>(av_realloc_array(I.oc->chapters, I.oc->nb_chapters + 1, sizeof(AVChapter*)));
            if (!grown) {
                av_dict_free(&ch->metadata);
                av_free(ch);
                break;
            }
            I.oc->chapters = grown;
            I.oc->chapters[I.oc->nb_chapters++] = ch;
        }
    }
    for (const auto& [k, val] : tags) av_dict_set(&I.oc->metadata, k.c_str(), val.c_str(), 0);

    if (!(I.oc->oformat->flags & AVFMT_NOFILE)) {
        r = avio_open(&I.oc->pb, path.c_str(), AVIO_FLAG_WRITE);
        if (r < 0) {
            err = "cannot write " + path + ": " + avError(r);
            return false;
        }
    }
    AVDictionary* mopts = nullptr;
    if (o.mp4) av_dict_set(&mopts, "movflags", "use_metadata_tags", 0);
    r = avformat_write_header(I.oc, &mopts);
    av_dict_free(&mopts);
    if (r < 0) {
        err = "cannot write the header: " + avError(r);
        return false;
    }
    I.headerWritten = true;

    // Caption packets go in up front; the interleaver sorts them in.
    size_t si = 0;
    for (const OutputCaptionTrack& t : captions) {
        if (t.cues.empty() || si >= I.subs.size()) continue;
        AVStream* s = I.subs[si++];
        for (const Caption& c : t.cues) {
            PacketPtr p = makePacket();
            if (av_new_packet(p.get(), int(c.text.size())) < 0) continue;
            std::memcpy(p->data, c.text.data(), c.text.size());
            p->pts = p->dts = av_rescale_q(int64_t(std::llround(c.start * 1000.0)), AVRational{1, 1000}, s->time_base);
            p->duration = av_rescale_q(int64_t(std::llround((c.end - c.start) * 1000.0)), AVRational{1, 1000}, s->time_base);
            p->stream_index = s->index;
            p->flags |= AV_PKT_FLAG_KEY;
            av_interleaved_write_frame(I.oc, p.get());
        }
    }
    return true;
}

bool OutputWriter::writeVideo(AVFrame* f, std::string& err) {
    if (!m_i) return false;
    Impl& I = *m_i;
    f->pts = I.vframes++;
    f->sample_aspect_ratio = I.vc->sample_aspect_ratio;
    f->color_range = AVCOL_RANGE_MPEG;
    f->color_primaries = I.vc->color_primaries;
    f->color_trc = I.vc->color_trc;
    f->colorspace = I.vc->colorspace;
    f->pict_type = AV_PICTURE_TYPE_NONE;
    const bool inter = I.field == FieldOrder::TFF || I.field == FieldOrder::BFF;
    setFrameInterlace(f, inter, I.field == FieldOrder::TFF);
    const int r = avcodec_send_frame(I.vc.get(), f);
    if (r < 0) {
        err = "video encoder: " + avError(r);
        return false;
    }
    return I.drain(I.vc.get(), I.vs, err);
}

bool OutputWriter::writeAudio(const std::vector<int32_t>& samples, std::string& err) {
    if (!m_i || !m_i->ac) return true;
    Impl& I = *m_i;
    I.fifo.insert(I.fifo.end(), samples.begin(), samples.end());
    const int fs = I.ac->frame_size > 0 ? I.ac->frame_size : 1024;
    while (I.fifo.size() >= size_t(fs) * size_t(I.channels))
        if (!I.encodeAudioFrame(fs, err)) return false;
    return true;
}

bool OutputWriter::finish(std::string& err) {
    if (!m_i) return false;
    Impl& I = *m_i;
    if (avcodec_send_frame(I.vc.get(), nullptr) < 0 || !I.drain(I.vc.get(), I.vs, err)) return false;
    if (I.ac) {
        if (!I.fifo.empty()) {
            const int left = int(I.fifo.size() / size_t(I.channels));
            const bool small = I.ac->codec->capabilities & (AV_CODEC_CAP_SMALL_LAST_FRAME | AV_CODEC_CAP_VARIABLE_FRAME_SIZE);
            const int fs = I.ac->frame_size > 0 ? I.ac->frame_size : left;
            if (!I.encodeAudioFrame(small ? left : fs, err)) return false;
            if (!small) I.asamples -= fs - left;  // padding is not audio
        }
        if (avcodec_send_frame(I.ac.get(), nullptr) < 0 || !I.drain(I.ac.get(), I.as, err)) return false;
    }
    const int r = av_write_trailer(I.oc);
    if (r < 0) {
        err = "cannot finish the file: " + avError(r);
        return false;
    }
    if (I.oc->pb) avio_closep(&I.oc->pb);
    I.headerWritten = false;  // done: keep the file
    return true;
}

}  // namespace ud

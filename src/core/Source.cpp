// SPDX-License-Identifier: GPL-3.0-or-later
#include "Source.h"

#include "shared/Checksums.h"
#include "shared/Json.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>

extern "C" {
#include <libavutil/imgutils.h>
#include <libavutil/pixdesc.h>
#include <libavutil/samplefmt.h>
}

namespace ud {

namespace fs = std::filesystem;

int64_t SourceInfo::sampleOfFrame(int64_t f) const {
    if (!sampleRate || !fps.valid()) return 0;
    // Exact rational: f * rate * den / num, rounded.
    const __int128 n = __int128(f) * sampleRate * fps.den;
    const __int128 d = fps.num;
    return int64_t((n * 2 + d) / (2 * d));
}

// ---------------------------------------------------------------------------
// sidecar helpers (the naming rules are PingDub's)

std::string captureBaseFor(const std::string& path) {
    std::string p = path;
    for (const char* ext : {".log.csv", ".json", ".mkv", ".avi", ".sha256", ".framemd5", ".chapters.txt", ".cc1.scc"}) {
        const std::string e = ext;
        if (p.size() > e.size() && p.compare(p.size() - e.size(), e.size(), e) == 0) {
            p.resize(p.size() - e.size());
            break;
        }
    }
    // name_part002 -> name
    const size_t u = p.rfind("_part");
    if (u != std::string::npos && p.size() - u == 8) {
        bool digits = true;
        for (size_t i = u + 5; i < p.size(); ++i) digits &= std::isdigit(static_cast<unsigned char>(p[i])) != 0;
        if (digits) p.resize(u);
    }
    return p;
}

bool parseCsvLine(const std::string& line, std::vector<std::string>& f) {
    f.clear();
    std::string cur;
    bool quoted = false;
    for (size_t i = 0; i < line.size(); ++i) {
        const char c = line[i];
        if (quoted) {
            if (c == '"') {
                if (i + 1 < line.size() && line[i + 1] == '"') {
                    cur += '"';
                    ++i;
                } else {
                    quoted = false;
                }
            } else {
                cur += c;
            }
        } else if (c == '"') {
            quoted = true;
        } else if (c == ',') {
            f.push_back(cur);
            cur.clear();
        } else if (c != '\r' && c != '\n') {
            cur += c;
        }
    }
    f.push_back(cur);
    return !quoted;
}

static double srtTime(const std::string& s) {
    int h = 0, m = 0, sec = 0, ms = 0;
    if (std::sscanf(s.c_str(), "%d:%d:%d,%d", &h, &m, &sec, &ms) == 4 || std::sscanf(s.c_str(), "%d:%d:%d.%d", &h, &m, &sec, &ms) == 4)
        return h * 3600.0 + m * 60.0 + sec + ms / 1000.0;
    return -1;
}

bool parseSrt(const std::string& path, std::vector<Caption>& out) {
    out.clear();
    std::ifstream f(path);
    if (!f) return false;
    std::string line;
    Caption cur;
    int state = 0;  // 0 index, 1 time, 2 text
    auto flush = [&] {
        if (state == 2 && cur.end > cur.start) {
            while (!cur.text.empty() && cur.text.back() == '\n') cur.text.pop_back();
            out.push_back(cur);
        }
        cur = Caption{};
        state = 0;
    };
    while (std::getline(f, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.size() >= 3 && (unsigned char)line[0] == 0xEF && (unsigned char)line[1] == 0xBB && (unsigned char)line[2] == 0xBF)
            line.erase(0, 3);
        if (line.empty()) {
            flush();
            continue;
        }
        if (state == 0) {
            if (line.find("-->") != std::string::npos) state = 1;  // index line missing
            else {
                state = 1;
                continue;
            }
        }
        if (state == 1) {
            const size_t a = line.find("-->");
            if (a == std::string::npos) {
                state = 0;
                continue;
            }
            std::string l = line.substr(0, a), r = line.substr(a + 3);
            auto trim = [](std::string s) {
                while (!s.empty() && s.front() == ' ') s.erase(0, 1);
                const size_t sp = s.find(' ');
                if (sp != std::string::npos) s.resize(sp);
                return s;
            };
            cur.start = srtTime(trim(l));
            cur.end = srtTime(trim(r));
            state = 2;
            continue;
        }
        cur.text += line;
        cur.text += '\n';
    }
    flush();
    return true;
}

bool parseFfmetadataChapters(const std::string& path, const Rational& fps, std::vector<Chapter>& out) {
    out.clear();
    std::ifstream f(path);
    if (!f) return false;
    std::string line;
    bool in = false;
    int64_t tbn = 1, tbd = 1000;
    int64_t start = 0, end = 0;
    std::string title;
    auto flush = [&] {
        if (!in) return;
        auto toFrame = [&](int64_t t) {
            const double sec = double(t) * double(tbn) / double(tbd);
            return int64_t(std::llround(sec * fps.toDouble()));
        };
        out.push_back({toFrame(start), toFrame(end), title});
        in = false;
    };
    while (std::getline(f, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line == "[CHAPTER]") {
            flush();
            in = true;
            tbn = 1;
            tbd = 1000;
            start = end = 0;
            title.clear();
            continue;
        }
        if (!in) continue;
        if (line.rfind("TIMEBASE=", 0) == 0) std::sscanf(line.c_str() + 9, "%lld/%lld", (long long*)&tbn, (long long*)&tbd);
        else if (line.rfind("START=", 0) == 0) start = std::atoll(line.c_str() + 6);
        else if (line.rfind("END=", 0) == 0) end = std::atoll(line.c_str() + 4);
        else if (line.rfind("title=", 0) == 0) title = line.substr(6);
        else if (!line.empty() && line[0] == '[') flush();
    }
    flush();
    return true;
}

bool gvusb2TagIsWrong(const std::string& driver, const std::string& captureStart) {
    std::string d = driver;
    for (char& c : d) c = char(std::tolower(static_cast<unsigned char>(c)));
    if (d.find("gvusb2") == std::string::npos && d.find("gv-usb2") == std::string::npos) return false;
    if (captureStart.size() < 10) return false;
    return captureStart.compare(0, 10, kGvusb2FixDate) < 0;
}

// A path from the JSON, or the file of the same name next to the JSON if the
// capture was moved since.
static std::string resolveNear(const std::string& p, const std::string& folder) {
    std::error_code ec;
    if (!p.empty() && fs::exists(p, ec)) return p;
    const std::string name = fs::path(p).filename().string();
    const std::string near = folder + "/" + name;
    if (!name.empty() && fs::exists(near, ec)) return near;
    return p;
}

static bool isMedia(const std::string& p) {
    std::string e = fs::path(p).extension().string();
    for (char& c : e) c = char(std::tolower(static_cast<unsigned char>(c)));
    return e != ".json" && e != ".csv" && e != ".sha256" && e != ".framemd5" && e != ".txt" && e != ".srt" && e != ".scc";
}

static FieldOrder fromAvField(AVFieldOrder f) {
    switch (f) {
    case AV_FIELD_PROGRESSIVE: return FieldOrder::Progressive;
    case AV_FIELD_TT:
    case AV_FIELD_TB: return FieldOrder::TFF;
    case AV_FIELD_BB:
    case AV_FIELD_BT: return FieldOrder::BFF;
    default: return FieldOrder::Unknown;
    }
}

static int64_t tagFrames(AVStream* st) {
    for (const char* k : {"NUMBER_OF_FRAMES", "NUMBER_OF_FRAMES-eng"})
        if (const AVDictionaryEntry* e = av_dict_get(st->metadata, k, nullptr, 0)) return std::atoll(e->value);
    return 0;
}

static SourceEvent::Kind eventKind(const std::string& e) {
    if (e == "insert") return SourceEvent::Kind::Insert;
    if (e == "drop" || e == "driver_drop" || e == "buffer_overflow") return SourceEvent::Kind::Drop;
    if (e == "signal_lost") return SourceEvent::Kind::SignalLost;
    if (e == "error_frame") return SourceEvent::Kind::Damaged;
    return SourceEvent::Kind::Other;
}

bool probeSource(const std::string& path, bool joinSegments, SourceInfo& s, std::string& err) {
    s = SourceInfo{};
    s.path = path;
    s.basePath = captureBaseFor(path);
    s.jsonPath = s.basePath + ".json";
    s.csvPath = s.basePath + ".log.csv";
    const std::string folder = fs::path(s.basePath).parent_path().string();
    std::error_code ec;

    JsonValue j;
    std::string jerr;
    s.havePingDub = fs::exists(s.jsonPath, ec) && readJsonFile(s.jsonPath, j, jerr) && j.isObject() && j.has("pingdub_version");
    std::vector<SourceSegment> listed;
    if (s.havePingDub) {
        s.pingdubVersion = j["pingdub_version"].str();
        s.driver = j["device"]["driver"].str();
        s.device = j["device"]["name"].str();
        s.input = j["device"]["input"].str();
        s.standard = j["video"]["standard"].str();
        s.captureStart = j["capture_start"].str();
        s.tapeLabel = j["tape_label"].str();
        s.jsonField = fieldOrderFromName(j["video"]["field_order"].str());
        if (j["segments"].isArray()) {
            for (const JsonValue& g : j["segments"].a) {
                SourceSegment seg;
                seg.file = resolveNear(g["file"].str(), folder);
                seg.startFrame = g["start_frame"].integer(0);
                seg.frames = g["frames"].integer(0);
                const std::string md5 = g["framemd5"].str();
                if (!md5.empty()) seg.framemd5 = resolveNear(md5, folder);
                listed.push_back(seg);
            }
        } else {
            for (const JsonValue& f : j["files"].a) {
                SourceSegment seg;
                seg.file = resolveNear(f.str(), folder);
                listed.push_back(seg);
            }
            if (listed.size() == 1) listed[0].frames = j["totals"]["frames"].integer(0);
        }
    }

    // Which files make the timeline.
    const bool openedMedia = isMedia(path);
    if (!listed.empty() && (joinSegments || !openedMedia)) {
        s.segments = listed;
    } else if (openedMedia) {
        SourceSegment seg;
        seg.file = path;
        for (const SourceSegment& l : listed) {
            if (fs::equivalent(l.file, path, ec)) seg = l;
            else s.otherSegments.push_back(l.file);
        }
        seg.file = path;
        s.segments = {seg};
    } else {
        err = "No media file found for " + path;
        return false;
    }
    // A lone segment of a split capture becomes its own timeline starting at 0.
    const int64_t rebase = s.segments.size() == 1 ? s.segments[0].startFrame : 0;
    if (listed.size() > 1 && s.segments.size() == 1) s.segments[0].startFrame = 0;

    // Probe the first file for the stream layout, then each for its length.
    int64_t timeline = 0;
    for (size_t i = 0; i < s.segments.size(); ++i) {
        SourceSegment& seg = s.segments[i];
        AVFormatContext* raw = nullptr;
        if (avformat_open_input(&raw, seg.file.c_str(), nullptr, nullptr) < 0) {
            err = "Cannot open " + seg.file;
            return false;
        }
        InputPtr in(raw);
        if (avformat_find_stream_info(in.get(), nullptr) < 0) {
            err = "Cannot read stream info from " + seg.file;
            return false;
        }
        const int vi = av_find_best_stream(in.get(), AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
        if (vi < 0) {
            err = "No video stream in " + seg.file;
            return false;
        }
        AVStream* st = in->streams[vi];
        AVCodecParameters* cp = st->codecpar;
        AVRational fr = st->avg_frame_rate.num ? st->avg_frame_rate : st->r_frame_rate;
        if (!fr.num || !fr.den) fr = AVRational{30000, 1001};
        if (i == 0) {
            s.width = cp->width;
            s.height = cp->height;
            s.pixfmt = cp->format;
            const AVCodecDescriptor* cd = avcodec_descriptor_get(cp->codec_id);
            s.codec = cd ? cd->name : "unknown";
            s.fps = {fr.num, fr.den};
            const AVRational sar = cp->sample_aspect_ratio.num ? cp->sample_aspect_ratio : st->sample_aspect_ratio;
            s.lines525 = std::abs(s.fps.toDouble() - 25.0) > 0.5 && s.height < 560;
            if (sar.num > 0 && sar.den > 0) s.sar = {sar.num, sar.den};
            else s.sar = s.lines525 ? Rational{10, 11} : Rational{12, 11};
            s.fieldTag = fromAvField(cp->field_order);
            if (cp->color_primaries == AVCOL_PRI_BT470BG || cp->color_space == AVCOL_SPC_BT470BG) s.color = ColorStd::Bt470bg;
            else if (cp->color_primaries == AVCOL_PRI_SMPTE170M || cp->color_space == AVCOL_SPC_SMPTE170M) s.color = ColorStd::Smpte170m;
            else s.color = s.lines525 ? ColorStd::Smpte170m : ColorStd::Bt470bg;
            if (const AVPixFmtDescriptor* d = av_pix_fmt_desc_get(AVPixelFormat(cp->format))) {
                s.bitDepth = d->comp[0].depth;
                s.log2ChromaW = d->log2_chroma_w;
                s.log2ChromaH = d->log2_chroma_h;
                s.gray = d->nb_components < 3;
            }
            const int ai = av_find_best_stream(in.get(), AVMEDIA_TYPE_AUDIO, -1, vi, nullptr, 0);
            if (ai >= 0) {
                AVCodecParameters* ap = in->streams[ai]->codecpar;
                s.hasAudio = true;
                s.sampleRate = ap->sample_rate;
                s.channels = ap->ch_layout.nb_channels;
                s.audioBits = ap->bits_per_raw_sample ? ap->bits_per_raw_sample : ap->bits_per_coded_sample;
                if (!s.audioBits) s.audioBits = av_get_bytes_per_sample(AVSampleFormat(ap->format)) * 8;
                const AVCodecDescriptor* ad = avcodec_descriptor_get(ap->codec_id);
                s.audioCodec = ad ? ad->name : "unknown";
            }
            // Container chapters, used when the JSON has none.
            if (!s.havePingDub || !j["chapters"].isArray()) {
                for (unsigned c = 0; c < in->nb_chapters; ++c) {
                    const AVChapter* ch = in->chapters[c];
                    const AVDictionaryEntry* t = av_dict_get(ch->metadata, "title", nullptr, 0);
                    Chapter x;
                    x.start = int64_t(std::llround(double(ch->start) * av_q2d(ch->time_base) * s.fps.toDouble()));
                    x.end = int64_t(std::llround(double(ch->end) * av_q2d(ch->time_base) * s.fps.toDouble()));
                    x.title = t ? t->value : "";
                    s.chapters.push_back(x);
                }
            }
        }
        if (seg.frames <= 0) {
            int64_t n = st->nb_frames > 0 ? st->nb_frames : tagFrames(st);
            if (n <= 0) {
                double dur = st->duration != AV_NOPTS_VALUE ? double(st->duration) * av_q2d(st->time_base)
                                                             : (in->duration != AV_NOPTS_VALUE ? double(in->duration) / AV_TIME_BASE : 0.0);
                n = int64_t(std::llround(dur * s.fps.toDouble()));
            }
            seg.frames = n;
        }
        if (s.segments.size() > 1) seg.startFrame = timeline;
        else seg.startFrame = 0;
        timeline += seg.frames;
        if (seg.framemd5.empty()) {
            const std::string m = sidecarFor(seg.file, ".framemd5");
            if (fs::exists(m, ec)) seg.framemd5 = m;
        }
        const std::string h = sidecarFor(seg.file, ".sha256");
        if (fs::exists(h, ec)) seg.sha256 = h;
    }
    s.frames = timeline;
    for (SourceSegment& seg : s.segments) {
        seg.startSample = s.sampleOfFrame(seg.startFrame);
        seg.samples = s.sampleOfFrame(seg.startFrame + seg.frames) - seg.startSample;
    }
    s.audioSamples = s.hasAudio ? s.sampleOfFrame(s.frames) : 0;

    auto inRange = [&](int64_t f) { return f - rebase; };
    if (s.havePingDub && j["chapters"].isArray()) {
        for (const JsonValue& c : j["chapters"].a) {
            Chapter x{inRange(c["start"].integer()), inRange(c["end"].integer()), c["title"].str()};
            if (x.end <= 0 || x.start >= s.frames) continue;
            x.start = std::max<int64_t>(0, x.start);
            x.end = std::min(s.frames, x.end);
            s.chapters.push_back(x);
        }
    }
    if (s.chapters.empty()) {
        const std::string txt = sidecarFor(s.segments[0].file, ".chapters.txt");
        if (fs::exists(txt, ec)) parseFfmetadataChapters(txt, s.fps, s.chapters);
    }
    std::sort(s.chapters.begin(), s.chapters.end(), [](const Chapter& a, const Chapter& b) { return a.start < b.start; });

    std::ifstream csv(s.csvPath);
    if (csv) {
        std::string line;
        std::vector<std::string> f;
        bool header = true;
        while (std::getline(csv, line)) {
            if (header) {
                header = false;
                if (line.rfind("wallclock,", 0) == 0) continue;
            }
            if (line.empty() || !parseCsvLine(line, f) || f.size() < 6) continue;
            if (f[1].empty()) continue;
            SourceEvent e;
            e.name = f[3];
            e.kind = eventKind(e.name);
            if (e.kind == SourceEvent::Kind::Other && e.name != "capture_stop") continue;
            e.frame = inRange(std::atoll(f[1].c_str()));
            const int64_t count = std::atoll(f[4].c_str());
            e.endFrame = e.kind == SourceEvent::Kind::Insert && count > 0 ? e.frame + count - 1 : e.frame;
            e.detail = f[5];
            if (e.endFrame < 0 || e.frame >= s.frames) continue;
            if (e.kind != SourceEvent::Kind::Other) s.events.push_back(e);
        }
    }

    for (int ch = 1; ch <= 4; ++ch) {
        const std::string srt = s.basePath + ".cc" + std::to_string(ch) + ".srt";
        if (!fs::exists(srt, ec)) continue;
        CaptionTrack t;
        t.path = srt;
        t.name = "CC" + std::to_string(ch);
        if (parseSrt(srt, t.cues) && !t.cues.empty()) {
            if (rebase) {
                const double off = frameSeconds(rebase, s.fps);
                for (Caption& c : t.cues) {
                    c.start -= off;
                    c.end -= off;
                }
                t.cues.erase(std::remove_if(t.cues.begin(), t.cues.end(), [&](const Caption& c) { return c.end <= 0 || c.start >= s.duration(); }),
                             t.cues.end());
            }
            s.captions.push_back(t);
        }
    }
    return true;
}

// ---------------------------------------------------------------------------
// SourceReader

SourceReader::~SourceReader() { close(); }

void SourceReader::close() {
    m_dec.reset();
    m_in.reset();
    m_pkt.reset();
    m_seg = SIZE_MAX;
}

bool SourceReader::open(const SourceInfo& info, std::string& err, int threads) {
    close();
    m_info = info;
    m_threads = threads;
    m_pkt = makePacket();
    if (info.segments.empty()) {
        err = "empty source";
        return false;
    }
    if (!openSegment(0)) {
        err = m_err;
        return false;
    }
    return true;
}

bool SourceReader::openSegment(size_t i) {
    m_dec.reset();
    m_in.reset();
    m_seg = SIZE_MAX;
    AVFormatContext* raw = nullptr;
    const std::string& file = m_info.segments[i].file;
    if (avformat_open_input(&raw, file.c_str(), nullptr, nullptr) < 0) {
        m_err = "cannot open " + file;
        return false;
    }
    m_in.reset(raw);
    avformat_find_stream_info(m_in.get(), nullptr);
    m_vi = av_find_best_stream(m_in.get(), AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (m_vi < 0) {
        m_err = "no video stream in " + file;
        return false;
    }
    for (unsigned k = 0; k < m_in->nb_streams; ++k)
        if (int(k) != m_vi) m_in->streams[k]->discard = AVDISCARD_ALL;
    AVStream* st = m_in->streams[m_vi];
    const AVCodec* dec = avcodec_find_decoder(st->codecpar->codec_id);
    if (!dec) {
        m_err = "no decoder for " + file;
        return false;
    }
    m_dec.reset(avcodec_alloc_context3(dec));
    avcodec_parameters_to_context(m_dec.get(), st->codecpar);
    m_dec->thread_count = m_threads;
    m_dec->pkt_timebase = st->time_base;
    if (avcodec_open2(m_dec.get(), dec, nullptr) < 0) {
        m_err = "cannot open the decoder for " + file;
        return false;
    }
    m_seg = i;
    m_next = 0;
    m_eof = false;
    return true;
}

FramePtr SourceReader::decodeNext() {
    AVStream* st = m_in->streams[m_vi];
    const double tb = av_q2d(st->time_base);
    const int64_t t0 = st->start_time != AV_NOPTS_VALUE ? st->start_time : 0;
    const double fps = m_info.fps.toDouble();
    FramePtr f = makeFrame();
    for (;;) {
        const int r = avcodec_receive_frame(m_dec.get(), f.get());
        if (r == 0) {
            int64_t idx = m_next;
            if (f->best_effort_timestamp != AV_NOPTS_VALUE) idx = int64_t(std::llround(double(f->best_effort_timestamp - t0) * tb * fps));
            if (idx < m_next) {
                av_frame_unref(f.get());
                continue;
            }
            m_next = idx + 1;
            return f;
        }
        if (r == AVERROR_EOF) {
            m_eof = true;
            return nullptr;
        }
        if (r != AVERROR(EAGAIN)) {
            m_err = "decode error: " + avError(r);
            return nullptr;
        }
        bool sent = false;
        while (!sent) {
            const int rr = av_read_frame(m_in.get(), m_pkt.get());
            if (rr < 0) {
                avcodec_send_packet(m_dec.get(), nullptr);
                sent = true;
                break;
            }
            if (m_pkt->stream_index == m_vi) {
                avcodec_send_packet(m_dec.get(), m_pkt.get());
                sent = true;
            }
            av_packet_unref(m_pkt.get());
        }
    }
}

bool SourceReader::seekTo(int64_t local) {
    AVStream* st = m_in->streams[m_vi];
    const int64_t t0 = st->start_time != AV_NOPTS_VALUE ? st->start_time : 0;
    const int64_t ts = t0 + av_rescale_q(local, AVRational{m_info.fps.den, m_info.fps.num}, st->time_base);
    if (av_seek_frame(m_in.get(), m_vi, ts, AVSEEK_FLAG_BACKWARD) < 0) {
        // Not seekable: start over and decode forward.
        if (!openSegment(m_seg)) return false;
        return true;
    }
    avcodec_flush_buffers(m_dec.get());
    m_eof = false;
    m_next = 0;  // the first decoded frame says where we landed
    return true;
}

FramePtr SourceReader::read(int64_t n) {
    if (n < 0 || n >= m_info.frames || m_info.segments.empty()) return nullptr;
    size_t s = 0;
    for (size_t i = 0; i < m_info.segments.size(); ++i)
        if (m_info.segments[i].startFrame <= n) s = i;
    const int64_t local = n - m_info.segments[s].startFrame;
    if (s != m_seg) {
        if (!openSegment(s)) return nullptr;
        if (local > 0 && !seekTo(local)) return nullptr;
    } else if (local < m_next || local > m_next + 24) {
        if (!seekTo(local)) return nullptr;
    }
    for (int guard = 0; guard < 100000; ++guard) {
        if (m_eof) return nullptr;
        FramePtr f = decodeNext();
        if (!f) return nullptr;
        const int64_t got = m_next - 1;
        if (got >= local) {
            // A file with a hole hands back the next frame it has.
            return f;
        }
    }
    return nullptr;
}

// ---------------------------------------------------------------------------
// AudioReader

AudioReader::~AudioReader() = default;

bool AudioReader::open(const SourceInfo& info, std::string& err) {
    m_info = info;
    m_pkt = makePacket();
    m_frame = makeFrame();
    if (!info.hasAudio) {
        err = "no audio";
        return false;
    }
    if (!openSegment(0)) {
        err = "cannot open the audio";
        return false;
    }
    return true;
}

bool AudioReader::openSegment(size_t i) {
    m_dec.reset();
    m_in.reset();
    m_seg = SIZE_MAX;
    AVFormatContext* raw = nullptr;
    if (avformat_open_input(&raw, m_info.segments[i].file.c_str(), nullptr, nullptr) < 0) return false;
    m_in.reset(raw);
    avformat_find_stream_info(m_in.get(), nullptr);
    m_ai = av_find_best_stream(m_in.get(), AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
    if (m_ai < 0) return false;
    // Video blocks are skipped over, not read, so counting samples from the
    // start of the file stays cheap and the count stays exact.
    for (unsigned k = 0; k < m_in->nb_streams; ++k)
        if (int(k) != m_ai) m_in->streams[k]->discard = AVDISCARD_ALL;
    const AVCodec* dec = avcodec_find_decoder(m_in->streams[m_ai]->codecpar->codec_id);
    if (!dec) return false;
    m_dec.reset(avcodec_alloc_context3(dec));
    avcodec_parameters_to_context(m_dec.get(), m_in->streams[m_ai]->codecpar);
    if (avcodec_open2(m_dec.get(), dec, nullptr) < 0) return false;
    m_seg = i;
    m_decodedTo = m_info.segments[i].startSample;
    m_bufStart = m_decodedTo;
    m_buf.clear();
    return true;
}

static void appendSamples(const AVFrame* f, int channels, std::vector<int32_t>& out) {
    const AVSampleFormat fmt = AVSampleFormat(f->format);
    const int n = f->nb_samples;
    const int fc = f->ch_layout.nb_channels;
    const bool planar = av_sample_fmt_is_planar(fmt);
    const size_t base = out.size();
    out.resize(base + size_t(n) * size_t(channels), 0);
    int32_t* o = out.data() + base;
    for (int s = 0; s < n; ++s) {
        for (int c = 0; c < channels; ++c) {
            if (c >= fc) continue;
            const int plane = planar ? c : 0;
            const int idx = planar ? s : s * fc + c;
            int32_t v = 0;
            switch (av_get_packed_sample_fmt(fmt)) {
            case AV_SAMPLE_FMT_U8: v = (int32_t(reinterpret_cast<const uint8_t*>(f->extended_data[plane])[idx]) - 128) << 24; break;
            case AV_SAMPLE_FMT_S16: v = int32_t(reinterpret_cast<const int16_t*>(f->extended_data[plane])[idx]) * 65536; break;
            case AV_SAMPLE_FMT_S32: v = reinterpret_cast<const int32_t*>(f->extended_data[plane])[idx]; break;
            case AV_SAMPLE_FMT_FLT: {
                const double x = reinterpret_cast<const float*>(f->extended_data[plane])[idx];
                v = int32_t(std::clamp(x * 2147483648.0, -2147483648.0, 2147483647.0));
                break;
            }
            case AV_SAMPLE_FMT_DBL: {
                const double x = reinterpret_cast<const double*>(f->extended_data[plane])[idx];
                v = int32_t(std::clamp(x * 2147483648.0, -2147483648.0, 2147483647.0));
                break;
            }
            default: break;
            }
            o[size_t(s) * size_t(channels) + size_t(c)] = v;
        }
    }
}

bool AudioReader::fill(int64_t upTo) {
    while (m_decodedTo < upTo && !m_eofAll) {
        const SourceSegment& seg = m_info.segments[m_seg];
        const int64_t segEnd = seg.startSample + seg.samples;
        if (m_decodedTo >= segEnd) {
            if (m_seg + 1 >= m_info.segments.size()) {
                m_eofAll = true;
                break;
            }
            // Next segment: samples continue exactly where this one stopped.
            std::vector<int32_t> keep;
            keep.swap(m_buf);
            const int64_t keepStart = m_bufStart;
            if (!openSegment(m_seg + 1)) {
                m_eofAll = true;
                break;
            }
            m_buf.swap(keep);
            m_bufStart = keepStart;
            continue;
        }
        const int r = avcodec_receive_frame(m_dec.get(), m_frame.get());
        if (r == 0) {
            std::vector<int32_t> tmp;
            appendSamples(m_frame.get(), m_info.channels, tmp);
            int64_t n = int64_t(tmp.size()) / std::max(1, m_info.channels);
            // A segment never hands more than its share to the timeline.
            if (m_decodedTo + n > segEnd) n = segEnd - m_decodedTo;
            m_buf.insert(m_buf.end(), tmp.begin(), tmp.begin() + n * m_info.channels);
            m_decodedTo += n;
            av_frame_unref(m_frame.get());
            continue;
        }
        if (r == AVERROR_EOF) {
            // Short segment: pad it with silence to its length so later ones stay in place.
            const int64_t pad = segEnd - m_decodedTo;
            if (pad > 0) {
                m_buf.resize(m_buf.size() + size_t(pad) * size_t(m_info.channels), 0);
                m_decodedTo = segEnd;
            }
            continue;
        }
        if (r != AVERROR(EAGAIN)) return false;
        bool sent = false;
        while (!sent) {
            const int rr = av_read_frame(m_in.get(), m_pkt.get());
            if (rr < 0) {
                avcodec_send_packet(m_dec.get(), nullptr);
                sent = true;
                break;
            }
            if (m_pkt->stream_index == m_ai) {
                avcodec_send_packet(m_dec.get(), m_pkt.get());
                sent = true;
            }
            av_packet_unref(m_pkt.get());
        }
    }
    return true;
}

bool AudioReader::read(int64_t start, int64_t count, std::vector<int32_t>& out) {
    const int ch = std::max(1, m_info.channels);
    out.assign(size_t(std::max<int64_t>(0, count)) * size_t(ch), 0);
    if (count <= 0 || m_info.segments.empty()) return true;
    if (start < m_bufStart) {
        // Going back: start the segment again.
        size_t s = 0;
        for (size_t i = 0; i < m_info.segments.size(); ++i)
            if (m_info.segments[i].startSample <= start) s = i;
        m_eofAll = false;
        if (!openSegment(s)) return false;
    } else if (m_seg != SIZE_MAX && start >= m_info.segments[m_seg].startSample + m_info.segments[m_seg].samples) {
        size_t s = m_seg;
        for (size_t i = 0; i < m_info.segments.size(); ++i)
            if (m_info.segments[i].startSample <= start) s = i;
        if (s != m_seg) {
            m_eofAll = false;
            if (!openSegment(s)) return false;
        }
    }
    if (!fill(start + count)) return false;
    // Drop what is no longer needed, keeping a second behind for crossfades.
    const int64_t keepFrom = std::max(m_bufStart, start - m_info.sampleRate);
    if (keepFrom > m_bufStart) {
        const size_t drop = size_t(keepFrom - m_bufStart) * size_t(ch);
        m_buf.erase(m_buf.begin(), m_buf.begin() + std::min(drop, m_buf.size()));
        m_bufStart = keepFrom;
    }
    const int64_t have = m_bufStart + int64_t(m_buf.size()) / ch;
    for (int64_t s = start; s < start + count && s < have; ++s) {
        if (s < m_bufStart) continue;
        std::memcpy(out.data() + size_t(s - start) * size_t(ch), m_buf.data() + size_t(s - m_bufStart) * size_t(ch),
                    sizeof(int32_t) * size_t(ch));
    }
    return true;
}

// ---------------------------------------------------------------------------
// field order

namespace {

// Luma rows of any decoded frame as 16-bit values on an 8-bit scale x 256.
struct LumaFrame {
    int w = 0, h = 0;
    std::vector<float> y;
};

bool lumaOf(const AVFrame* f, LumaFrame& out) {
    const AVPixFmtDescriptor* d = av_pix_fmt_desc_get(AVPixelFormat(f->format));
    if (!d || (d->flags & AV_PIX_FMT_FLAG_RGB)) return false;
    out.w = f->width;
    out.h = f->height;
    out.y.resize(size_t(out.w) * size_t(out.h));
    std::vector<uint16_t> line(size_t(out.w));
    const double scale = 1.0 / double(1 << std::max(0, d->comp[0].depth - 8));
    const uint8_t* data[4] = {f->data[0], f->data[1], f->data[2], f->data[3]};
    for (int y = 0; y < out.h; ++y) {
        av_read_image_line2(line.data(), data, f->linesize, d, 0, y, 0, out.w, 0, 2);
        float* o = out.y.data() + size_t(y) * size_t(out.w);
        for (int x = 0; x < out.w; ++x) o[x] = float(line[size_t(x)] * scale);
    }
    return true;
}

// Combing of a frame woven from the top field of `t` and the bottom of `b`.
double combOf(const LumaFrame& t, const LumaFrame& b) {
    const int w = std::min(t.w, b.w), h = std::min(t.h, b.h);
    auto px = [&](int x, int y) { return (y & 1) ? b.y[size_t(y) * size_t(b.w) + size_t(x)] : t.y[size_t(y) * size_t(t.w) + size_t(x)]; };
    double sum = 0;
    for (int y = 1; y + 1 < h; ++y)
        for (int x = 16; x < w - 16; x += 2) {
            const double c = px(x, y), a = px(x, y - 1), e = px(x, y + 1);
            const double v = (a - c) * (e - c);
            if (v > 0) sum += v;
        }
    return sum;
}

}  // namespace

double combScorePair(const AVFrame* fa, const AVFrame* fb, double* motion) {
    LumaFrame a, b;
    if (!lumaOf(fa, a) || !lumaOf(fb, b) || a.w != b.w || a.h != b.h) return 0;
    if (motion) {
        double m = 0;
        for (size_t i = 0; i < a.y.size(); i += 3) m += std::fabs(a.y[i] - b.y[i]);
        *motion = m / double(a.y.size() / 3 + 1);
    }
    // TFF: fields run T(n) B(n) T(n+1), so B(n) sits next to T(n+1).
    const double tff = combOf(b, a);
    // BFF: B(n) T(n) B(n+1), so T(n) sits next to B(n+1).
    const double bff = combOf(a, b);
    return (tff - bff) / (tff + bff + 1e-9);
}

FieldOrder combTest(SourceReader& r, const SourceInfo& info, int64_t start, int maxFrames, double* confidence) {
    double votes = 0, weight = 0;
    int used = 0;
    FramePtr prev = r.read(start);
    for (int64_t n = start + 1; prev && n < info.frames && n < start + 900 && used < maxFrames; ++n) {
        FramePtr cur = r.read(n);
        if (!cur) break;
        double motion = 0;
        const double s = combScorePair(prev.get(), cur.get(), &motion);
        if (motion > 1.5) {
            votes += s;
            weight += 1;
            ++used;
        }
        prev = std::move(cur);
    }
    if (confidence) *confidence = weight > 0 ? std::min(1.0, std::fabs(votes / weight) * 2.0) : 0.0;
    if (weight < 3) return FieldOrder::Unknown;
    const double mean = votes / weight;
    if (std::fabs(mean) < 0.03) return FieldOrder::Unknown;
    return mean < 0 ? FieldOrder::TFF : FieldOrder::BFF;
}

FieldOrderCheck checkFieldOrder(SourceReader& r, const SourceInfo& info) {
    FieldOrderCheck c;
    c.tag = info.fieldTag;
    c.json = info.jsonField;
    c.gvusb2Trap = gvusb2TagIsWrong(info.driver, info.captureStart);
    c.comb = combTest(r, info, std::min<int64_t>(30, std::max<int64_t>(0, info.frames - 60)), 40, &c.combConfidence);
    if (c.gvusb2Trap) {
        c.chosen = FieldOrder::TFF;
        c.notes.push_back("GV-USB2 capture made before the " + std::string(kGvusb2FixDate) +
                          " driver build: it is tagged bottom field first but the tape is top field first. Using TFF.");
    } else if (c.json == FieldOrder::TFF || c.json == FieldOrder::BFF) {
        c.chosen = c.json;
    } else if (c.tag == FieldOrder::TFF || c.tag == FieldOrder::BFF) {
        c.chosen = c.tag;
    } else if (c.comb != FieldOrder::Unknown) {
        c.chosen = c.comb;
        c.notes.push_back("The file has no field order tag; the comb test says " + std::string(fieldOrderName(c.comb)) + ".");
    } else {
        c.chosen = FieldOrder::TFF;
        c.notes.push_back("No field order tag and the comb test found too little motion. Assuming TFF; check it in the Source tab.");
    }
    if (c.comb != FieldOrder::Unknown && c.comb != c.chosen && c.combConfidence > 0.25)
        c.notes.push_back("The comb test on the first moving frames says " + std::string(fieldOrderName(c.comb)) + ", but " +
                          fieldOrderName(c.chosen) + " is set. Motion will judder if the order is wrong.");
    return c;
}

}  // namespace ud

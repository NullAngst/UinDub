// SPDX-License-Identifier: GPL-3.0-or-later
// A source: a PingDub master (one file or several segments joined into one
// timeline) or any interlaced SD file FFmpeg decodes. Opened read-only.
//
// Geometry, field order, pixel aspect and color tags come from the file,
// never from constants, which is what makes PAL work with no special cases.
// The sidecars next to a PingDub capture add chapters, events, captions,
// checksums and the segment list.
#pragma once

#include "Av.h"
#include "Types.h"

#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace ud {

struct SourceSegment {
    std::string file;
    int64_t startFrame = 0;
    int64_t frames = 0;
    int64_t startSample = 0;
    int64_t samples = 0;
    std::string framemd5;  // sidecar paths, "" when absent
    std::string sha256;
};

struct Chapter {
    int64_t start = 0;  // source frames
    int64_t end = 0;    // exclusive
    std::string title;
};

// Events PingDub logged on the timeline that matter here.
struct SourceEvent {
    enum class Kind { Insert, Drop, SignalLost, Damaged, Other };
    Kind kind = Kind::Other;
    int64_t frame = 0;
    int64_t endFrame = 0;  // inclusive
    std::string name, detail;
};

struct Caption {
    double start = 0, end = 0;  // seconds on the source timeline
    std::string text;
};

struct CaptionTrack {
    std::string path;
    std::string name;  // "CC1", "CC3" ...
    std::vector<Caption> cues;
};

struct SourceInfo {
    std::string path;      // what the user opened
    std::string basePath;  // folder/stem shared by PingDub's sidecars
    std::vector<SourceSegment> segments;
    int64_t frames = 0;

    // video
    int width = 0, height = 0;
    int pixfmt = -1;       // AVPixelFormat as decoded
    int bitDepth = 8;
    int log2ChromaW = 1, log2ChromaH = 0;
    bool gray = false;
    std::string codec;
    Rational fps{30000, 1001};
    Rational sar{10, 11};
    FieldOrder fieldTag = FieldOrder::Unknown;  // from the container
    bool lines525 = true;                       // NTSC geometry; false for 625-line
    ColorStd color = ColorStd::Smpte170m;

    // audio
    bool hasAudio = false;
    int sampleRate = 0, channels = 0, audioBits = 0;
    std::string audioCodec;
    int64_t audioSamples = 0;

    // PingDub sidecars
    bool havePingDub = false;
    std::string jsonPath, csvPath;
    std::string pingdubVersion, driver, device, input, standard, captureStart, tapeLabel;
    FieldOrder jsonField = FieldOrder::Unknown;
    std::vector<Chapter> chapters;
    std::vector<SourceEvent> events;
    std::vector<CaptionTrack> captions;
    std::vector<std::string> otherSegments;  // segment files not opened (a single segment was asked for)

    bool interlacedSd() const { return height >= 480 && height <= 608; }
    double duration() const { return frameSeconds(frames, fps); }
    int64_t sampleOfFrame(int64_t f) const;
};

// Opens and describes a source. With joinSegments, a segment of a split
// capture brings the whole segment list along.
bool probeSource(const std::string& path, bool joinSegments, SourceInfo& out, std::string& err);

// The capture's base path for any of its files or sidecars.
std::string captureBaseFor(const std::string& path);
bool parseCsvLine(const std::string& line, std::vector<std::string>& fields);
bool parseSrt(const std::string& path, std::vector<Caption>& out);
// FFmpeg's ";FFMETADATA1" chapter text, as PingDub writes for AVI.
bool parseFfmetadataChapters(const std::string& path, const Rational& fps, std::vector<Chapter>& out);

// Decodes frames by timeline index across segments. Sequential reads are
// cheap; a jump seeks. One reader per thread.
class SourceReader {
public:
    SourceReader() = default;
    ~SourceReader();
    SourceReader(const SourceReader&) = delete;
    SourceReader& operator=(const SourceReader&) = delete;
    bool open(const SourceInfo& info, std::string& err, int threads = 0);
    void close();
    // Frame n of the timeline, decoded as-is. Null past the end or on error.
    FramePtr read(int64_t n);
    std::string lastError() const { return m_err; }

private:
    bool openSegment(size_t i);
    bool seekTo(int64_t local);
    FramePtr decodeNext();

    SourceInfo m_info;
    size_t m_seg = SIZE_MAX;
    InputPtr m_in;
    CodecPtr m_dec;
    PacketPtr m_pkt;
    int m_vi = -1;
    int m_threads = 0;
    int64_t m_next = -1;    // local index the next decoded frame should have
    bool m_eof = false;
    std::string m_err;
};

// Audio as interleaved 32-bit integers, left-justified (a 16-bit sample is
// value << 16), read by timeline sample position across segments.
class AudioReader {
public:
    ~AudioReader();
    bool open(const SourceInfo& info, std::string& err);
    // `count` samples per channel starting at `start`; samples past the end are silence.
    bool read(int64_t start, int64_t count, std::vector<int32_t>& out);

private:
    bool openSegment(size_t i);
    bool fill(int64_t upTo);

    SourceInfo m_info;
    size_t m_seg = SIZE_MAX;
    InputPtr m_in;
    CodecPtr m_dec;
    PacketPtr m_pkt;
    FramePtr m_frame;
    int m_ai = -1;
    std::vector<int32_t> m_buf;  // decoded samples from m_bufStart
    int64_t m_bufStart = 0;
    int64_t m_decodedTo = 0;     // timeline sample after the last decoded one
    bool m_eofAll = false;
};

// Field order checks.
struct FieldOrderCheck {
    FieldOrder tag = FieldOrder::Unknown;       // container tag
    FieldOrder json = FieldOrder::Unknown;      // PingDub JSON
    FieldOrder comb = FieldOrder::Unknown;      // comb test on moving frames
    double combConfidence = 0;                  // 0..1
    bool gvusb2Trap = false;                    // GV-USB2 capture before the fixed driver
    FieldOrder chosen = FieldOrder::TFF;
    std::vector<std::string> notes;
};

// The GV-USB2 driver build that fixed the field tag.
constexpr const char* kGvusb2FixDate = "2026-10-28";
bool gvusb2TagIsWrong(const std::string& driver, const std::string& captureStart);

// Weaves consecutive frames both ways and compares the combing. Looks at up
// to `maxFrames` frames from `start`, keeping the ones with motion.
FieldOrder combTest(SourceReader& r, const SourceInfo& info, int64_t start, int maxFrames, double* confidence);
// Same test on two decoded frames, for synthetic checks: returns
// comb(TFF pairing) - comb(BFF pairing) normalized; negative means TFF.
double combScorePair(const AVFrame* a, const AVFrame* b, double* motion);

FieldOrderCheck checkFieldOrder(SourceReader& r, const SourceInfo& info);

}  // namespace ud

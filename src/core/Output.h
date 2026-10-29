// SPDX-License-Identifier: GPL-3.0-or-later
// Encoding and muxing: x265, x264 or SVT-AV1 at the RF you pick (FFV1 for
// lossless work), FLAC, Opus, AAC or untouched PCM, chapters, caption tracks
// and the tags that describe the render, into MKV (or MP4).
#pragma once

#include "Av.h"
#include "Source.h"
#include "Types.h"

#include <map>
#include <memory>
#include <string>
#include <vector>

namespace ud {

enum class VideoCodec { X265, X264, SvtAv1, Ffv1 };
enum class AudioCodec { Flac, Opus, Aac, Copy };
const char* videoCodecName(VideoCodec c);
VideoCodec videoCodecFromName(const std::string& s);
const char* audioCodecName(AudioCodec c);
AudioCodec audioCodecFromName(const std::string& s);

int rfMax(VideoCodec c);
int rfDefault(VideoCodec c);
std::string presetDefault(VideoCodec c);
std::vector<std::string> presetsFor(VideoCodec c);

struct OutputSettings {
    VideoCodec codec = VideoCodec::X265;
    int rf = 15;
    std::string preset = "slow";
    bool chroma422 = false;          // x265 and x264
    AudioCodec audio = AudioCodec::Flac;
    int opusKbps = 192;
    int aacKbps = 256;
    bool mp4 = false;
    std::string folder;              // "" asks
    std::string name;                // "" means "<source name>.uindub"
    bool chapters = true;
    bool captions = true;
    bool operator==(const OutputSettings& o) const;
};

// The pixel format the encoder gets.
AVPixelFormat encoderPixelFormat(const OutputSettings& o, const SourceInfo& s);
// What MP4 can carry: FLAC and captions are switched off there.
OutputSettings effectiveOutput(const OutputSettings& o);
// "<folder>/<name>.mkv"
// "<source name>.uindub": the output name when none is set.
std::string defaultOutputName(const SourceInfo& s);
std::string outputPath(const OutputSettings& o, const SourceInfo& s);

struct MuxVideo {
    int width = 0, height = 0;
    AVPixelFormat pixfmt = AV_PIX_FMT_YUV420P10LE;
    Rational fps{60000, 1001};
    Rational sar{10, 11};
    FieldOrder field = FieldOrder::Progressive;
    ColorStd color = ColorStd::Smpte170m;
};

struct MuxAudio {
    bool present = false;
    int rate = 48000, channels = 2, bits = 16;
};

struct OutputChapter {
    double start = 0, end = 0;  // seconds
    std::string title;
};

struct OutputCaptionTrack {
    std::string name;
    std::vector<Caption> cues;  // output seconds
};

class OutputWriter {
public:
    OutputWriter();
    ~OutputWriter();
    OutputWriter(const OutputWriter&) = delete;
    OutputWriter& operator=(const OutputWriter&) = delete;

    bool open(const std::string& path, const OutputSettings& o, const MuxVideo& v, const MuxAudio& a,
              const std::vector<OutputChapter>& chapters, const std::vector<OutputCaptionTrack>& captions,
              const std::map<std::string, std::string>& tags, std::string& err);
    // Video frames in output order; pts is assigned here.
    bool writeVideo(AVFrame* f, std::string& err);
    // Interleaved int32 left-justified samples.
    bool writeAudio(const std::vector<int32_t>& samples, std::string& err);
    bool finish(std::string& err);
    void abort();
    int64_t videoFrames() const;
    int64_t audioSamples() const;
    uint64_t bytes() const;

private:
    struct Impl;
    std::unique_ptr<Impl> m_i;
};

}  // namespace ud

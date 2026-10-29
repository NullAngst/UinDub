// SPDX-License-Identifier: GPL-3.0-or-later
// Every encoder and container: ffprobe confirms size, SAR, color tags, the
// progressive flag, chapters shifted by the trim, subtitle tracks and the
// settings tag.
#include "core/Synth.h"
#include "testing.h"

#include "core/Render.h"

#include <cstdio>

using namespace ud;

namespace {

struct Case {
    VideoCodec codec;
    bool mp4;
    bool square;
};

}  // namespace

TEST_CASE("encoders and containers") {
    const std::string dir = tempDir("output");
    SynthScene scene;
    DamageModel d;
    SynthFile f;
    f.path = dir + "/tape.mkv";
    f.frames = 30;
    f.chapters = true;   // chapters at 0 and 15
    f.captions = true;
    std::string err;
    CHECK_MSG(writeSynthCapture(f, scene, d, err), "%s", err.c_str());
    SourceInfo src;
    CHECK(probeSource(f.path, true, src, err));
    CHECK_EQ(int(src.chapters.size()), 2);
    CHECK_EQ(int(src.captions.size()), 1);

    const Case cases[] = {{VideoCodec::X265, false, false}, {VideoCodec::X264, false, true}, {VideoCodec::SvtAv1, false, false},
                          {VideoCodec::Ffv1, false, false}, {VideoCodec::X265, true, false}, {VideoCodec::X264, true, false}};
    for (const Case& c : cases) {
        RenderJob job;
        job.source = src;
        Project& p = job.project;
        p.folder = dir + "/project";
        p.sourcePath = f.path;
        p.fieldOrder = FieldOrder::TFF;
        p.deint.mode = DeintMode::Bwdif;
        p.deint.doubleRate = false;
        p.crop = {8, 7, 4, 9, c.square};  // odd sizes get snapped for 4:2:0
        p.edit.in = 6;
        p.out.codec = c.codec;
        p.out.rf = rfDefault(c.codec);
        p.out.preset = c.codec == VideoCodec::SvtAv1 ? "12" : (c.codec == VideoCodec::Ffv1 ? "" : "ultrafast");
        p.out.mp4 = c.mp4;
        p.out.audio = c.mp4 ? AudioCodec::Aac : AudioCodec::Flac;
        p.tapeLabel = "Synthetic tape";
        job.outPath = dir + "/out_" + videoCodecName(c.codec) + (c.mp4 ? ".mp4" : ".mkv");
        const RenderOutcome o = runRender(job);
        CHECK_MSG(o.ok, "%s: %s", job.outPath.c_str(), o.error.c_str());
        if (!o.ok) continue;
        CHECK_EQ(o.videoFrames, int64_t(24));
        JsonValue j;
        CHECK_MSG(ffprobeJson(job.outPath, "-count_frames -count_packets -show_streams -show_chapters -show_format", j, err), "%s", err.c_str());
        const JsonValue& streams = j["streams"];
        const JsonValue* v = nullptr;
        int subs = 0, audio = 0;
        for (const JsonValue& s : streams.a) {
            if (s["codec_type"].str() == "video") v = &s;
            if (s["codec_type"].str() == "subtitle") ++subs;
            if (s["codec_type"].str() == "audio") ++audio;
        }
        CHECK(v != nullptr);
        if (!v) continue;
        const bool is420 = c.codec != VideoCodec::Ffv1;
        // 720 - 15 = 705 -> 704 (chroma halved across); 486 - 13 = 473 -> 472 for 4:2:0
        const int w = 704, h = is420 ? 472 : 473;  // 4:2:2 FFV1 keeps the odd height
        const int fw = c.square ? 640 : w;
        std::printf("  %s %s: %lldx%lld sar %s, %s/%s/%s, field %s, %d chapters, %d subs, %d audio, %s frames\n", videoCodecName(c.codec),
                    c.mp4 ? "mp4" : "mkv", (*v)["width"].integer(), (*v)["height"].integer(), (*v)["sample_aspect_ratio"].str().c_str(),
                    (*v)["color_primaries"].str().c_str(), (*v)["color_transfer"].str().c_str(), (*v)["color_space"].str().c_str(),
                    (*v)["field_order"].str().c_str(), int(j["chapters"].size()), subs, audio,
                    ((*v)["nb_read_frames"].str().empty() ? (*v)["nb_read_packets"] : (*v)["nb_read_frames"]).str().c_str());
        CHECK_EQ((*v)["width"].integer(), (long long)fw);
        CHECK_EQ((*v)["height"].integer(), (long long)h);
        CHECK((*v)["sample_aspect_ratio"].str() == (c.square ? "1:1" : "10:11"));
        CHECK((*v)["color_primaries"].str() == "smpte170m");
        CHECK((*v)["color_space"].str() == "smpte170m");
        // Decoded frames when this ffprobe can decode the codec (an FFmpeg
        // without dav1d cannot decode AV1), packets otherwise: one per frame here.
        const std::string counted = (*v)["nb_read_frames"].str().empty() ? (*v)["nb_read_packets"].str() : (*v)["nb_read_frames"].str();
        CHECK_MSG(counted == "24", "%s frames", counted.c_str());
        if (!c.mp4) CHECK((*v)["field_order"].str() == "progressive");
        CHECK_EQ(audio, 1);
        CHECK_EQ(subs, c.mp4 ? 0 : 1);
        // Chapters at source frames 0 and 15, trimmed by 6: the first starts
        // the clip, the second moves to frame 9.
        CHECK_EQ(int(j["chapters"].size()), 2);
        if (j["chapters"].size() == 2) {
            const double second = std::atof(j["chapters"][1]["start_time"].str().c_str());
            CHECK_MSG(std::fabs(second - 9 * 1001.0 / 30000.0) < 0.002, "second chapter at %.4f", second);
        }
        const JsonValue& tags = j["format"]["tags"];
        bool haveSettings = false, haveHash = false;
        for (const auto& [k, val] : tags.o) {
            std::string up = k;
            for (char& ch : up) ch = char(std::toupper(static_cast<unsigned char>(ch)));
            if (up == "UINDUB_SETTINGS") haveSettings = val.str().find("\"edge_lock\"") != std::string::npos;
            if (up == "UINDUB_SOURCE_SHA256") haveHash = val.str().size() >= 64;
        }
        CHECK(haveSettings);
        CHECK(haveHash);
    }
}

TEST_MAIN()

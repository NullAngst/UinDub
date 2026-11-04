// SPDX-License-Identifier: GPL-3.0-or-later
// The upload path and what came with it: the final scale (size, square
// pixels, BT.709 tags), the contact sheet from the finished file, and audio
// and video staying interleaved in the file when caption tracks are on.
#include "core/Synth.h"
#include "testing.h"

#include "core/ContactSheet.h"
#include "core/Pipeline.h"
#include "core/Render.h"

#include <cmath>
#include <cstdio>
#include <filesystem>

using namespace ud;

namespace {

bool makeSource(const std::string& dir, SourceInfo& src, bool captions, int64_t frames = 60) {
    SynthScene scene;
    SynthFile f;
    f.path = dir + "/tape.mkv";
    f.frames = frames;
    f.captions = captions;
    f.chapters = true;
    std::string err;
    if (!writeSynthCapture(f, scene, DamageModel::none(), err)) {
        std::printf("  synth: %s\n", err.c_str());
        return false;
    }
    return probeSource(f.path, true, src, err);
}

Project baseProject(const std::string& dir, const SourceInfo& src) {
    Project p;
    p.folder = dir + "/project";
    p.sourcePath = src.path;
    p.fieldOrder = FieldOrder::TFF;
    p.deint.mode = DeintMode::Bwdif;
    p.deint.doubleRate = false;
    return p;
}

}  // namespace

TEST_CASE("scale geometry") {
    SourceInfo s;
    s.width = 720;
    s.height = 486;
    s.sar = {10, 11};
    s.color = ColorStd::Smpte170m;
    Project p;
    p.deint.mode = DeintMode::Bwdif;
    p.crop = {8, 8, 0, 6, false};  // 704 x 480 left
    p.out.scaleHeight = 1080;
    const OutputGeometry g = outputGeometry(p, s, AV_PIX_FMT_YUV420P);
    CHECK(g.scale);
    CHECK_EQ(g.preWidth, 704);
    CHECK_EQ(g.preHeight, 480);
    CHECK_EQ(g.height, 1080);
    // 704 x 10/11 = 640 display wide on 480: 4:3, so 1440 at 1080.
    CHECK_EQ(g.width, 1440);
    CHECK(g.sar.num == 1 && g.sar.den == 1);
    CHECK(g.color == ColorStd::Bt709);
    CHECK(!g.resize);

    // Interlaced output is never scaled.
    p.deint.mode = DeintMode::None;
    const OutputGeometry gi = outputGeometry(p, s, AV_PIX_FMT_YUV420P);
    CHECK(!gi.scale);
    CHECK_EQ(gi.height, 480);
    CHECK(gi.color == ColorStd::Smpte170m);
}

TEST_CASE("contact sheet text") {
    CHECK(runtimeString(0) == "0:00:00");
    CHECK(runtimeString(3725.9) == "1:02:05");
    CHECK(contactHeader("Christmas 1994", 7083) == "Christmas 1994 - 1:58:03");
    CHECK(contactPagePath("/a/b/tape.uindub.mkv", 1) == "/a/b/tape.uindub.contact01.jpg");
    CHECK(contactPagePath("/a/b/tape.uindub.mkv", 12) == "/a/b/tape.uindub.contact12.jpg");
}

TEST_CASE("1080p upload render with contact sheet") {
    const std::string dir = tempDir("upload");
    SourceInfo src;
    CHECK(makeSource(dir, src, false));
    RenderJob job;
    job.source = src;
    job.project = baseProject(dir, src);
    job.project.tapeLabel = "Test tape";
    applyUploadPreset(job.project.out, UploadPreset::YouTube1080);
    job.project.out.preset = "ultrafast";
    job.project.out.contactInterval = 1;
    job.outPath = dir + "/upload.mp4";
    const RenderOutcome o = runRender(job);
    CHECK_MSG(o.ok, "%s", o.error.c_str());
    if (!o.ok) return;
    std::string err;
    JsonValue j;
    CHECK_MSG(ffprobeJson(o.outPath, "-show_streams", j, err), "%s", err.c_str());
    const JsonValue* v = nullptr;
    const JsonValue* a = nullptr;
    for (const JsonValue& s : j["streams"].a) {
        if (s["codec_type"].str() == "video") v = &s;
        if (s["codec_type"].str() == "audio") a = &s;
    }
    CHECK(v && a);
    if (v) {
        CHECK_EQ((*v)["height"].integer(), 1080LL);
        CHECK((*v)["width"].integer() % 2 == 0);
        CHECK((*v)["sample_aspect_ratio"].str() == "1:1" || (*v)["sample_aspect_ratio"].str().empty());
        CHECK((*v)["codec_name"].str() == "h264");
        CHECK((*v)["color_space"].str() == "bt709");
        CHECK((*v)["color_primaries"].str() == "bt709");
    }
    if (a) CHECK((*a)["codec_name"].str() == "aac");

    // 60 frames at 29.97 is 2 seconds: thumbnails at 0, 1 and 2 would be
    // three, but 2.0 is past the end, so two, on one page.
    CHECK_EQ(int(o.contactSheets.size()), 1);
    if (!o.contactSheets.empty()) {
        CHECK(std::filesystem::exists(o.contactSheets.front()));
        JsonValue jj;
        CHECK(ffprobeJson(o.contactSheets.front(), "-show_streams", jj, err));
        if (jj["streams"].size() == 1) {
            // 6 tiles of 320 plus 7 gaps of 8.
            CHECK_EQ(jj["streams"][0]["width"].integer(), 6LL * 320 + 7 * 8);
            CHECK(jj["streams"][0]["codec_name"].str() == "mjpeg");
        }
    }

    // A second, shorter render of the same name removes the pages it no longer has.
    const std::string stale = contactPagePath(o.outPath, 2);
    std::FILE* f = std::fopen(stale.c_str(), "wb");
    if (f) std::fclose(f);
    ContactSheetOptions co;
    co.interval = 10;
    const ContactSheetResult cs = writeContactSheet(o.outPath, co);
    CHECK_MSG(cs.ok, "%s", cs.error.c_str());
    CHECK(!std::filesystem::exists(stale));
}

TEST_CASE("captions keep audio and video interleaved") {
    const std::string dir = tempDir("interleave");
    SourceInfo src;
    CHECK(makeSource(dir, src, true, 240));
    CHECK(!src.captions.empty());
    for (AudioCodec ac : {AudioCodec::Flac, AudioCodec::Copy}) {
        RenderJob job;
        job.source = src;
        job.project = baseProject(dir, src);
        job.project.deint.doubleRate = true;
        job.project.out.codec = VideoCodec::X265;
        job.project.out.rf = 28;
        job.project.out.preset = "medium";  // real lookahead, so the encoder holds frames back
        job.project.out.audio = ac;
        job.project.out.contactSheet = false;
        job.outPath = dir + "/out_" + audioCodecName(ac) + ".mkv";
        const RenderOutcome o = runRender(job);
        CHECK_MSG(o.ok, "%s", o.error.c_str());
        if (!o.ok) continue;
        std::string err;
        JsonValue j;
        CHECK_MSG(ffprobeJson(o.outPath, "-show_packets -show_streams", j, err), "%s", err.c_str());
        int vIndex = -1, aIndex = -1, subs = 0;
        for (const JsonValue& s : j["streams"].a) {
            if (s["codec_type"].str() == "video") vIndex = int(s["index"].integer());
            if (s["codec_type"].str() == "audio") aIndex = int(s["index"].integer());
            if (s["codec_type"].str() == "subtitle") ++subs;
        }
        CHECK_EQ(subs, 1);
        // In file order, no audio or video packet may come more than a few
        // milliseconds before one of the other that should be ahead of it.
        double newest = -1e9, worst = 0;
        for (const JsonValue& p : j["packets"].a) {
            const int si = int(p["stream_index"].integer());
            if (si != vIndex && si != aIndex) continue;
            const std::string d = p["dts_time"].str();
            if (d.empty() || d == "N/A") continue;
            const double t = std::atof(d.c_str());
            worst = std::max(worst, newest - t);
            newest = std::max(newest, t);
        }
        std::printf("  %s: worst a/v order slip %.3f s\n", audioCodecName(ac), worst);
        CHECK_MSG(worst < 0.05, "%s: packets out of order by %.3f s", audioCodecName(ac), worst);
    }
}

TEST_MAIN()

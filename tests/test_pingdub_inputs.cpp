// SPDX-License-Identifier: GPL-3.0-or-later
// Every PingDub codec and pixel format opens; segments join with no missing
// or repeated frame; sidecars parse, including ones from older PingDub versions.
#include "core/Synth.h"
#include "testing.h"

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>

extern "C" {
#include <libavutil/pixdesc.h>
}

using namespace ud;

namespace {

// Position of the moving bar on a line of a frame, which says which frame it is.
double barAt(const Picture& p, int y) {
    const float* row = p.p[0].row(y);
    for (int x = 150; x < p.width - 1; ++x)
        if (row[x] < 180 && row[x + 1] >= 180) return x + (180 - row[x]) / (row[x + 1] - row[x]);
    return -1;
}

}  // namespace

TEST_CASE("codecs and pixel formats") {
    struct C {
        const char* codec;
        const char* pix;
        const char* container;
        double tol;
    };
    const C cases[] = {{"ffv1", "yuv422p", "mkv", 0.6},      {"ffv1", "yuv422p10le", "mkv", 0.2}, {"ffv1", "yuv420p", "mkv", 0.6},
                       {"ffv1", "gray", "mkv", 0.6},          {"ffv1", "gray10le", "mkv", 0.2},    {"utvideo", "yuv422p", "mkv", 0.6},
                       {"ffvhuff", "yuv422p", "mkv", 0.6},    {"huffyuv", "yuv422p", "avi", 0.6},  {"rawvideo", "yuyv422", "avi", 0.6},
                       {"rawvideo", "yuyv422", "mkv", 0.6},   {"mjpeg", "yuvj422p", "avi", 12.0}};
    SynthScene scene;
    for (const C& c : cases) {
        const std::string dir = tempDir(std::string("inputs_") + c.codec + "_" + c.pix + "_" + c.container);
        SynthFile f;
        f.path = dir + "/tape." + c.container;
        f.codec = c.codec;
        f.pixfmt = av_get_pix_fmt(c.pix);
        f.container = c.container;
        f.frames = 6;
        std::string err;
        const bool wrote = writeSynthCapture(f, scene, DamageModel::none(), err);
        CHECK_MSG(wrote, "%s %s: %s", c.codec, c.pix, err.c_str());
        if (!wrote) continue;
        SourceInfo s;
        CHECK_MSG(probeSource(f.path, true, s, err), "%s", err.c_str());
        CHECK_EQ(s.width, 720);
        CHECK_EQ(s.height, 486);
        CHECK_EQ(s.frames, int64_t(6));
        CHECK(s.havePingDub);
        CHECK(s.hasAudio);
        SourceReader r;
        CHECK(r.open(s, err));
        double worst = 0;
        for (int64_t n = 0; n < 6; ++n) {
            FramePtr fr = r.read(n);
            CHECK(fr != nullptr);
            if (!fr) break;
            Picture got, want;
            CHECK(toPicture(fr.get(), got, true));
            SynthScene sc = scene;
            sc.panBar = true;
            sc.panPerField = 3.0;
            sc.panX = 200;
            renderFrame(sc, DamageModel::none(), n, FieldOrder::TFF, want);
            for (int y = 20; y < 480; y += 37)
                for (int x = 20; x < 700; x += 3) worst = std::max(worst, double(std::fabs(got.p[0].row(y)[x] - want.p[0].row(y)[x])));
        }
        std::printf("  %-8s %-12s %s: %s, worst luma error %.2f\n", c.codec, c.pix, c.container,
                    av_get_pix_fmt_name(AVPixelFormat(s.pixfmt)), worst);
        CHECK_MSG(worst <= c.tol, "%s %s off by %.2f", c.codec, c.pix, worst);
    }
}

TEST_CASE("segments join into one source") {
    const std::string dir = tempDir("inputs_segments");
    SynthScene scene;
    SynthFile f;
    f.path = dir + "/tape.mkv";
    f.frames = 30;
    f.segments = 3;
    std::string err;
    CHECK(writeSynthCapture(f, scene, DamageModel::none(), err));
    // Opening the middle segment brings the rest along, or just itself.
    const std::string mid = dir + "/tape_part002.mkv";
    SourceInfo joined, alone;
    CHECK(probeSource(mid, true, joined, err));
    CHECK(probeSource(mid, false, alone, err));
    CHECK_EQ(int(joined.segments.size()), 3);
    CHECK_EQ(joined.frames, int64_t(30));
    CHECK_EQ(int(alone.segments.size()), 1);
    CHECK_EQ(alone.frames, int64_t(10));
    CHECK_EQ(int(alone.otherSegments.size()), 2);
    // Every frame once, in order, across both joins.
    SourceReader r;
    CHECK(r.open(joined, err));
    SynthScene sc = scene;
    sc.panBar = true;
    sc.panPerField = 3.0;
    sc.panX = 200;
    int wrong = 0;
    for (int64_t n = 0; n < 30; ++n) {
        FramePtr fr = r.read(n);
        Picture got, want;
        toPicture(fr.get(), got, true);
        renderFrame(sc, DamageModel::none(), n, FieldOrder::TFF, want);
        if (std::fabs(barAt(got, 100) - barAt(want, 100)) > 0.5) ++wrong;
    }
    CHECK_EQ(wrong, 0);
    // Random access backwards across a join.
    for (int64_t n : {25, 3, 17, 9, 10, 29, 0}) {
        FramePtr fr = r.read(n);
        Picture got, want;
        toPicture(fr.get(), got, true);
        renderFrame(sc, DamageModel::none(), n, FieldOrder::TFF, want);
        CHECK_MSG(std::fabs(barAt(got, 100) - barAt(want, 100)) < 0.5, "frame %lld", (long long)n);
    }
    // Audio is sample-continuous across segments.
    AudioReader a;
    CHECK(a.open(joined, err));
    std::vector<int32_t> buf;
    const int64_t total = joined.audioSamples;
    CHECK(a.read(0, total, buf));
    int bad = 0;
    for (int64_t k = 0; k < total; ++k) {
        const int16_t want = int16_t(std::lround(9000.0 * std::sin(2 * M_PI * 440.0 * double(k) / 48000.0)));
        bad += (buf[size_t(k * 2)] >> 16) != want;
    }
    CHECK_EQ(bad, 0);
}

TEST_CASE("sidecars, including older PingDub versions") {
    const std::string dir = tempDir("inputs_sidecars");
    SynthScene scene;
    SynthFile f;
    f.path = dir + "/tape.mkv";
    f.frames = 30;
    f.chapters = true;
    f.captions = true;
    f.events = true;
    std::string err;
    CHECK(writeSynthCapture(f, scene, DamageModel::none(), err));
    SourceInfo s;
    CHECK(probeSource(f.path, true, s, err));
    CHECK_EQ(int(s.chapters.size()), 2);
    CHECK_EQ(int(s.captions.size()), 1);
    CHECK_EQ(int(s.captions[0].cues.size()), 3);
    int inserts = 0, losses = 0, damaged = 0;
    for (const SourceEvent& e : s.events) {
        inserts += e.kind == SourceEvent::Kind::Insert;
        losses += e.kind == SourceEvent::Kind::SignalLost;
        damaged += e.kind == SourceEvent::Kind::Damaged;
    }
    CHECK_EQ(inserts, 1);
    CHECK_EQ(losses, 1);
    CHECK_EQ(damaged, 1);
    CHECK(s.driver == "gvusb2");
    // An older JSON: a "files" list and no "segments", chapters in a
    // .chapters.txt next to an AVI.
    const std::string od = tempDir("inputs_old");
    SynthFile g;
    g.path = od + "/old.avi";
    g.container = "avi";
    g.codec = "huffyuv";
    g.frames = 12;
    g.pingdub = false;
    CHECK(writeSynthCapture(g, scene, DamageModel::none(), err));
    {
        std::ofstream j(od + "/old.json");
        j << "{\"pingdub_version\": \"0.9.0\", \"capture_start\": \"2025-05-01T10:00:00\", \"tape_label\": \"Old\", "
             "\"device\": {\"name\": \"EasyCAP\", \"driver\": \"usbtv\"}, \"video\": {\"field_order\": \"TFF\", \"frame_rate\": \"30000/1001\"}, "
             "\"files\": [\"" << g.path << "\"], \"totals\": {\"frames\": 12}}\n";
        std::ofstream c(od + "/old.chapters.txt");
        c << ";FFMETADATA1\n[CHAPTER]\nTIMEBASE=1001/30000\nSTART=0\nEND=6\ntitle=Recording 1\n[CHAPTER]\nTIMEBASE=1001/30000\nSTART=6\nEND=12\ntitle=Recording 2\n";
        std::ofstream l(od + "/old.log.csv");
        l << "wallclock,slot,timecode,event,count,detail\n2025-05-01T10:00:00,4,,driver_drop,1,\"x\"\n";
    }
    SourceInfo o;
    CHECK_MSG(probeSource(g.path, true, o, err), "%s", err.c_str());
    CHECK(o.havePingDub);
    CHECK_EQ(o.frames, int64_t(12));
    CHECK_EQ(int(o.chapters.size()), 2);
    if (o.chapters.size() == 2) CHECK_EQ(o.chapters[1].start, int64_t(6));
    CHECK_EQ(int(o.events.size()), 1);
    CHECK(o.tapeLabel == "Old");
}

TEST_MAIN()

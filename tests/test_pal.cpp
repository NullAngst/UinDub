// SPDX-License-Identifier: GPL-3.0-or-later
// The jitter, field order, deinterlace and output tests again on 720x576 25i
// synthetic fields: 50p and 25p out, 12:11 aspect and bt470bg tags carried.
#include "core/Synth.h"
#include "testing.h"

#include "core/Render.h"

#include <cstdio>

using namespace ud;

TEST_CASE("PAL jitter comes back clean") {
    SynthScene s;
    s.height = 576;
    DamageModel d;
    TrackerConfig c = borderConfig(s, d, 30);
    CHECK(!c.refs.empty());
    const SynthRun r = runTracker(s, d, c, 30);
    CHECK_EQ(int(r.fields.size()), 60);
    CHECK_EQ(int(r.fields[0].off.size()), 288);
    const Residual res = residualOf(r, d);
    std::printf("  PAL: rms %.4f px over %d lines\n", res.rmsMeasured, res.measured);
    CHECK(res.rmsMeasured < 0.15);
}

TEST_CASE("PAL field order, deinterlace and tags") {
    const std::string dir = tempDir("pal");
    SynthScene scene;
    for (FieldOrder truth : {FieldOrder::TFF, FieldOrder::BFF}) {
        SynthFile f;
        f.path = dir + "/pal_" + fieldOrderName(truth) + ".mkv";
        f.pal = true;
        f.frames = 20;
        f.truth = truth;
        f.tag = truth;
        f.driver = "saa7134";
        std::string err;
        CHECK_MSG(writeSynthCapture(f, scene, DamageModel::none(), err), "%s", err.c_str());
        SourceInfo s;
        CHECK(probeSource(f.path, true, s, err));
        CHECK_EQ(s.height, 576);
        CHECK(!s.lines525);
        CHECK(s.fps == Rational(25, 1));
        CHECK(s.sar == Rational(12, 11));
        CHECK(s.color == ColorStd::Bt470bg);
        SourceReader r;
        r.open(s, err);
        const FieldOrder comb = combTest(r, s, 0, 15, nullptr);
        CHECK(comb == truth);
        for (bool dbl : {true, false}) {
            RenderJob job;
            job.source = s;
            job.project.folder = dir + "/project";
            job.project.sourcePath = f.path;
            job.project.fieldOrder = truth;
            job.project.deint.mode = DeintMode::Bwdif;
            job.project.deint.doubleRate = dbl;
            job.project.out.codec = VideoCodec::X264;
            job.project.out.preset = "ultrafast";
            job.outPath = dir + "/out_" + fieldOrderName(truth) + (dbl ? "_50p.mkv" : "_25p.mkv");
            const RenderOutcome o = runRender(job);
            CHECK_MSG(o.ok, "%s", o.error.c_str());
            CHECK_EQ(o.videoFrames, int64_t(dbl ? 40 : 20));
            JsonValue j;
            CHECK(ffprobeJson(job.outPath, "-show_streams -select_streams v", j, err));
            const JsonValue& v = j["streams"][size_t(0)];
            std::printf("  %s %s: %s, sar %s, %s/%s\n", fieldOrderName(truth), dbl ? "50p" : "25p", v["r_frame_rate"].str().c_str(),
                        v["sample_aspect_ratio"].str().c_str(), v["color_primaries"].str().c_str(), v["color_space"].str().c_str());
            CHECK(v["r_frame_rate"].str() == (dbl ? "50/1" : "25/1"));
            CHECK(v["sample_aspect_ratio"].str() == "12:11");
            CHECK(v["color_primaries"].str() == "bt470bg");
            CHECK(v["color_space"].str() == "bt470bg");
            CHECK_EQ(v["height"].integer(), 576LL);
        }
    }
}

TEST_MAIN()

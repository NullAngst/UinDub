// SPDX-License-Identifier: GPL-3.0-or-later
// A short clean FFV1 capture, damaged with known offsets by the warble
// generator, then analyzed: residual under 0.3 px RMS on lines with a
// measurement. The clean clip is generated here; set UINDUB_WARBLE_CLIP to a
// real clean PingDub capture to run the same check on real footage.
#include "core/Synth.h"
#include "testing.h"

#include "core/Analyze.h"
#include "core/Warble.h"

#include <cstdio>
#include <cstdlib>

using namespace ud;

TEST_CASE("warble copy, analyze, score") {
    const std::string dir = tempDir("warble");
    std::string clean;
    std::string err;
    if (const char* real = std::getenv("UINDUB_WARBLE_CLIP")) {
        clean = real;
        std::printf("  using %s\n", real);
    } else {
        // A clean capture: border, bars, a little noise, nothing wobbling.
        SynthScene scene;
        scene.noise = 1.5;
        scene.sigma = 1.6;
        SynthFile f;
        f.path = dir + "/clean.mkv";
        f.frames = 24;
        f.moving = true;
        CHECK_MSG(writeSynthCapture(f, scene, DamageModel::none(), err), "%s", err.c_str());
        clean = f.path;
    }
    WarbleJob job;
    CHECK(probeSource(clean, true, job.clean, err));
    job.frames = std::min<int64_t>(job.clean.frames, 48);
    job.order = FieldOrder::TFF;
    job.outPath = dir + "/clean.warble.mkv";
    job.projectFolder = dir + "/project";
    const WarbleResult w = makeWarbleCopy(job);
    CHECK_MSG(w.ok, "%s", w.error.c_str());
    if (!w.ok) return;
    CHECK(w.project.hasTruth);
    CHECK_EQ(int(w.project.refs.size()), 1);
    // The copy keeps its audio and length.
    SourceInfo copy;
    CHECK(probeSource(job.outPath, false, copy, err));
    CHECK_EQ(copy.frames, job.frames);
    CHECK(copy.hasAudio == job.clean.hasAudio);
    AnalyzeJob a;
    a.source = copy;
    a.order = FieldOrder::TFF;
    a.refs = w.project.refs;
    a.offsetsPath = w.project.offsetsPath();
    a.tracksPath = w.project.tracksPath();
    const AnalyzeOutcome o = runAnalyze(a);
    CHECK_MSG(o.ok, "%s", o.error.c_str());
    OffsetStore st;
    CHECK(st.open(a.offsetsPath, err));
    const WarbleScore sc = scoreWarble(st, w.project.truth, FieldOrder::TFF, copy.frames);
    std::printf("  residual %.4f px RMS on %d measured lines of %d (all lines %.4f, clip constant %.3f)\n", sc.rmsMeasured, sc.measured,
                sc.lines, sc.rmsAll, sc.bias);
    CHECK(sc.ok);
    CHECK_MSG(sc.rmsMeasured < 0.3, "%.4f px", sc.rmsMeasured);
    CHECK(sc.measured > sc.lines / 2);
    CHECK_EQ(int(sc.perLine.size()), copy.height);
    CHECK_EQ(int(sc.perField.size()), int(copy.frames * 2));
    // The project reloads with its truth.
    Project back;
    CHECK(Project::load(w.project.folder, back, err));
    CHECK(back.hasTruth);
    CHECK(back.truth.jitter == w.project.truth.jitter);
}

TEST_MAIN()

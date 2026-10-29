// SPDX-License-Identifier: GPL-3.0-or-later
#include "SelfTest.h"

#include "Analyze.h"
#include "Av.h"
#include "Edit.h"
#include "Qtgmc.h"
#include "Render.h"
#include "Synth.h"
#include "Warble.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>

#include <unistd.h>

namespace ud {

namespace {

struct Checker {
    bool verbose;
    int failed = 0;
    void step(const char* what) const { std::printf("self-test: %s\n", what); std::fflush(stdout); }
    bool check(bool ok, const std::string& what) {
        if (!ok) {
            ++failed;
            std::fprintf(stderr, "self-test: FAIL %s\n", what.c_str());
        } else if (verbose) {
            std::string w = what;
            if (w.size() >= 2 && w.compare(w.size() - 2, 2, ": ") == 0) w.resize(w.size() - 2);
            std::printf("self-test:   ok %s\n", w.c_str());
        }
        return ok;
    }
};

std::string fmt(const char* f, double a, double b = 0) {
    char buf[256];
    std::snprintf(buf, sizeof buf, f, a, b);
    return buf;
}

}  // namespace

int runSelfTest(bool verbose) {
    Checker c{verbose};
    const auto t0 = std::chrono::steady_clock::now();
    namespace fs = std::filesystem;
    std::error_code ec;
    const fs::path dir = fs::temp_directory_path(ec) / ("uindub-selftest-" + std::to_string(::getpid()));
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);
    struct Cleanup {
        fs::path d;
        ~Cleanup() {
            std::error_code e;
            if (!std::getenv("UINDUB_SELFTEST_KEEP")) fs::remove_all(d, e);
        }
    } cleanup{dir};
    std::string err;

    c.step("writing a 10 second synthetic capture (FFV1, PCM)");
    SynthScene scene;
    scene.noise = 1.5;
    scene.sigma = 1.6;
    SynthFile f;
    f.path = (dir / "clean.mkv").string();
    f.frames = 300;
    f.moving = true;
    if (!c.check(writeSynthCapture(f, scene, DamageModel::none(), err), "synthetic capture: " + err)) return 1;

    c.step("damaging a copy with known offsets (warble generator)");
    WarbleJob wj;
    if (!c.check(probeSource(f.path, true, wj.clean, err), "probe clean: " + err)) return 1;
    wj.order = FieldOrder::TFF;
    wj.outPath = (dir / "clean.warble.mkv").string();
    wj.projectFolder = (dir / "project").string();
    const WarbleResult w = makeWarbleCopy(wj);
    if (!c.check(w.ok, "warble copy: " + w.error)) return 1;
    SourceInfo src;
    if (!c.check(probeSource(wj.outPath, true, src, err), "probe copy: " + err)) return 1;
    c.check(src.frames == 300, fmt("copy has %.0f frames, want 300", double(src.frames)));

    c.step("Edge Lock analyze");
    AnalyzeJob aj;
    aj.source = src;
    aj.order = FieldOrder::TFF;
    aj.settings = w.project.edge;
    aj.refs = w.project.refs;
    aj.offsetsPath = w.project.offsetsPath();
    aj.tracksPath = w.project.tracksPath();
    const AnalyzeOutcome ao = runAnalyze(aj);
    if (!c.check(ao.ok, "analyze: " + ao.error)) return 1;
    OffsetStore store;
    if (c.check(store.open(aj.offsetsPath, err), "offsets.zst opens: " + err)) {
        const WarbleScore sc = scoreWarble(store, w.project.truth, FieldOrder::TFF, src.frames);
        std::printf("self-test: Edge Lock residual %.4f px RMS on measured lines\n", sc.rmsMeasured);
        c.check(sc.ok && sc.rmsMeasured < 0.3, fmt("Edge Lock residual %.4f px, want under 0.3", sc.rmsMeasured));
    }

    Project base = w.project;
    base.edgeEnabled = true;
    base.analysisKey = base.currentAnalysisKey();
    base.fieldOrder = FieldOrder::TFF;
    base.out.audio = AudioCodec::Flac;
    const EditList el = EditList::build(src.frames, base.edit);
    const int64_t wantSamples = planAudio(el, src).totalSamples();

    auto renderOnce = [&](const char* label, DeintMode mode, const char* preset, VideoCodec vc, const char* encPreset) {
        c.step(label);
        RenderJob rj;
        rj.project = base;
        rj.project.deint.mode = mode;
        rj.project.deint.doubleRate = true;
        rj.project.deint.qtgmcPreset = preset;
        rj.project.out.codec = vc;
        rj.project.out.rf = rfDefault(vc);
        rj.project.out.preset = encPreset;
        rj.source = src;
        rj.outPath = (dir / (std::string(label).substr(7) + ".mkv")).string();
        for (char& ch : rj.outPath)
            if (ch == ' ') ch = '_';
        const RenderOutcome o = runRender(rj);
        if (!c.check(o.ok, std::string(label) + ": " + o.error)) return;
        const int64_t wantFrames = expectedOutputFrames(rj.project, el.outputFrames());
        c.check(o.videoFrames == wantFrames, fmt("video frames %.0f, want %.0f", double(o.videoFrames), double(wantFrames)));
        c.check(o.audioSamples == wantSamples, fmt("audio samples %.0f, want %.0f", double(o.audioSamples), double(wantSamples)));
        SourceInfo back;
        if (c.check(probeSource(o.outPath, false, back, err), "output probes: " + err)) {
            c.check(back.frames == wantFrames, fmt("output file has %.0f frames, want %.0f", double(back.frames), double(wantFrames)));
            c.check(fs::exists(o.sidecar), "sidecar written: " + o.sidecar);
        }
    };

    renderOnce("render bwdif x265", DeintMode::Bwdif, "Draft", VideoCodec::X265, "fast");

    const VsRuntimeInfo& rt = vapoursynthRuntime();
    if (!rt.ok) {
        if (std::getenv("UINDUB_SELFTEST_NO_QTGMC")) {
            std::printf("self-test: QTGMC skipped (UINDUB_SELFTEST_NO_QTGMC is set): %s\n", rt.error.c_str());
        } else {
            c.check(false, "QTGMC runtime: " + rt.error);
        }
    } else {
        std::printf("self-test: VapourSynth %s\n", rt.version.c_str());
        renderOnce("render QTGMC x264", DeintMode::Qtgmc, "Draft", VideoCodec::X264, "ultrafast");
    }

    const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    if (c.failed) {
        std::fprintf(stderr, "self-test: %d check(s) failed (%.1f s)\n", c.failed, secs);
        return 1;
    }
    std::printf("self-test: passed (%.1f s)\n", secs);
    return 0;
}

}  // namespace ud

// SPDX-License-Identifier: GPL-3.0-or-later
// The Analyze tab's FFmpeg measurements on synthetic tape with a known
// answer: idet finds telecined film and the field order, cropdetect finds
// the blanking borders, ebur128 and the peak count read the test tone.
#include "core/Synth.h"
#include "testing.h"

#include "core/Detect.h"

#include <cmath>
#include <cstdio>

using namespace ud;

namespace {

bool synth(const std::string& name, const SynthScene& scene, int64_t frames, SourceInfo& src) {
    SynthFile f;
    f.path = tempDir(name) + "/tape.mkv";
    f.frames = frames;
    std::string err;
    if (!writeSynthCapture(f, scene, DamageModel::none(), err)) {
        std::printf("  synth: %s\n", err.c_str());
        return false;
    }
    return probeSource(f.path, true, src, err);
}

}  // namespace

TEST_CASE("idet finds telecined film, top field first") {
    SynthScene scene;
    scene.telecine = true;
    scene.panPerFilm = 6;
    SourceInfo src;
    CHECK(synth("detect_film", scene, 150, src));
    const IdetResult r = runIdet(src, 0, src.frames, 2, 60);
    CHECK_MSG(r.ok, "%s", r.error.c_str());
    std::printf("  tff %lld bff %lld prog %lld und %lld, repeated top %lld bottom %lld neither %lld\n", (long long)r.tff, (long long)r.bff,
                (long long)r.progressive, (long long)r.undetermined, (long long)r.repeatedTop, (long long)r.repeatedBottom, (long long)r.repeatedNeither);
    CHECK(r.film);
    CHECK(r.order == FieldOrder::TFF);
}

TEST_CASE("idet on plain interlaced motion is not film") {
    SynthScene scene;
    scene.panBar = true;
    scene.panPerField = 3;
    SourceInfo src;
    CHECK(synth("detect_video", scene, 90, src));
    const IdetResult r = runIdet(src, 0, src.frames, 1, 90);
    CHECK_MSG(r.ok, "%s", r.error.c_str());
    std::printf("  repeated top %lld bottom %lld neither %lld\n", (long long)r.repeatedTop, (long long)r.repeatedBottom, (long long)r.repeatedNeither);
    CHECK(!r.film);
}

TEST_CASE("cropdetect finds the blanking borders") {
    SynthScene scene;  // left border at 12.5 px, right at 706.5 px
    SourceInfo src;
    CHECK(synth("detect_crop", scene, 20, src));
    const CropDetectResult c = runCropDetect(src, 0, src.frames, 10);
    CHECK_MSG(c.ok, "%s", c.error.c_str());
    std::printf("  left %d right %d top %d bottom %d\n", c.left, c.right, c.top, c.bottom);
    CHECK(c.left >= 10 && c.left <= 15);
    CHECK(c.right >= 10 && c.right <= 16);
    CHECK(c.top <= 2);
}

TEST_CASE("loudness and peak of the test tone") {
    SynthScene scene;
    SourceInfo src;
    CHECK(synth("detect_audio", scene, 90, src));
    EditSettings e;
    const LoudnessResult l = runLoudness(src, EditList::build(src.frames, e));
    CHECK_MSG(l.ok, "%s", l.error.c_str());
    std::printf("  %.2f LUFS, peak %.2f dBFS, %lld clipped\n", l.integrated, l.peakDb, (long long)l.clipped);
    // 9000 of 32767 is -11.2 dBFS.
    CHECK(std::fabs(l.peakDb - 20.0 * std::log10(9000.0 / 32767.0)) < 0.1);
    CHECK(l.integrated < -5 && l.integrated > -25);
    CHECK_EQ(l.clipped, int64_t(0));
}

TEST_MAIN()

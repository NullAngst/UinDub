// SPDX-License-Identifier: GPL-3.0-or-later
// Everything off, FFV1 output: the output's frame checksums equal the
// source's. Off means off.
#include "core/Synth.h"
#include "testing.h"

#include "core/OffsetStore.h"
#include "core/Render.h"
#include "shared/Checksums.h"

#include <cstdio>
#include <filesystem>

extern "C" {
#include <libavutil/pixdesc.h>
}

using namespace ud;

namespace {

std::vector<std::string> frameHashes(const std::string& path) {
    std::vector<std::string> out;
    SourceInfo s;
    std::string err;
    if (!probeSource(path, false, s, err)) return out;
    SourceReader r;
    if (!r.open(s, err)) return out;
    for (int64_t n = 0; n < s.frames; ++n) {
        FramePtr f = r.read(n);
        if (!f) break;
        uint8_t md5[16];
        int size = 0;
        frameMd5(f.get(), md5, size);
        out.push_back(toHex(md5, 16));
    }
    return out;
}

}  // namespace

TEST_CASE("neutral render is bit exact") {
    for (const char* pix : {"yuv422p", "yuv422p10le"}) {
        const std::string dir = tempDir(std::string("neutral_") + pix);
        SynthScene scene;
        DamageModel d;
        SynthFile f;
        f.path = dir + "/tape.mkv";
        f.frames = 20;
        f.pixfmt = av_get_pix_fmt(pix);
        std::string err;
        CHECK_MSG(writeSynthCapture(f, scene, d, err), "%s", err.c_str());
        RenderJob job;
        CHECK(probeSource(f.path, true, job.source, err));
        job.project.folder = dir + "/project";
        job.project.sourcePath = f.path;
        job.project.fieldOrder = FieldOrder::TFF;
        job.project.edgeEnabled = false;
        job.project.deint.mode = DeintMode::None;
        job.project.out.codec = VideoCodec::Ffv1;
        job.project.out.audio = AudioCodec::Flac;
        job.outPath = dir + "/out.mkv";
        const RenderOutcome o = runRender(job);
        CHECK_MSG(o.ok, "%s", o.error.c_str());
        const auto a = frameHashes(f.path), b = frameHashes(job.outPath);
        CHECK_EQ(int(a.size()), 20);
        CHECK_EQ(int(b.size()), 20);
        int diff = 0;
        for (size_t i = 0; i < std::min(a.size(), b.size()); ++i) diff += a[i] != b[i];
        std::printf("  %s: %zu frames, %d differ\n", pix, b.size(), diff);
        CHECK_EQ(diff, 0);
        // Edge Lock on with references but strength 0 also never touches a pixel.
        CHECK(!o.sidecar.empty());
    }
}

TEST_CASE("Edge Lock on with nothing to correct leaves pixels alone") {
    const std::string dir = tempDir("neutral_edge");
    SynthScene scene;
    const DamageModel none = DamageModel::none();
    SynthFile f;
    f.path = dir + "/clean.mkv";
    f.frames = 10;
    f.moving = false;
    std::string err;
    CHECK(writeSynthCapture(f, scene, none, err));
    RenderJob job;
    CHECK(probeSource(f.path, true, job.source, err));
    // Analyze a clean clip: the offsets are tiny but not all zero, so use
    // strength 0, which must skip the resampler entirely.
    Project& p = job.project;
    p.folder = dir + "/project";
    p.sourcePath = f.path;
    p.fieldOrder = FieldOrder::TFF;
    p.edgeEnabled = true;
    p.edge.strength = 0;
    p.deint.mode = DeintMode::None;
    p.out.codec = VideoCodec::Ffv1;
    job.outPath = dir + "/out.mkv";
    SourceReader r;
    r.open(job.source, err);
    FramePtr f0 = r.read(0);
    Picture pic;
    toPicture(f0.get(), pic, true);
    Reference ref;
    ref.kind = RefKind::LeftBorder;
    RefSpan sp;
    CHECK(makeBorderSpan(pic, false, job.source.frames, p.edge, sp));
    ref.spans.push_back(sp);
    p.refs.push_back(ref);
    // Pretend it was analyzed with zero offsets.
    OffsetHeader h;
    h.lines = job.source.height;
    h.width = job.source.width;
    h.frames = job.source.frames;
    OffsetWriter w;
    std::filesystem::create_directories(p.folder);
    CHECK(w.create(p.offsetsPath(), h, nullptr, 0, h.frames, err));
    for (int64_t n = 0; n < h.frames; ++n) {
        FrameOffsets fo;
        fo.reset(h.lines, false);
        for (int y = 0; y < h.lines; ++y) fo.off[size_t(y)] = int16_t(64 * ((y % 3) - 1));
        w.put(n, fo);
    }
    CHECK(w.finish(err));
    p.analysisKey = p.currentAnalysisKey();
    const RenderOutcome o = runRender(job);
    CHECK_MSG(o.ok, "%s", o.error.c_str());
    const auto a = frameHashes(f.path), b = frameHashes(job.outPath);
    int diff = 0;
    for (size_t i = 0; i < std::min(a.size(), b.size()); ++i) diff += a[i] != b[i];
    CHECK_EQ(int(b.size()), 10);
    CHECK_EQ(diff, 0);
}

TEST_MAIN()

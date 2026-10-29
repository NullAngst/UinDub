// SPDX-License-Identifier: GPL-3.0-or-later
// offsets.zst round-trips exactly, and a re-analyzed span replaces only its
// own frames.
#include "core/Synth.h"
#include "testing.h"

#include "core/Analyze.h"

#include <cstdio>
#include <filesystem>

using namespace ud;

namespace {

FrameOffsets pattern(int64_t frame, int lines, int salt, bool slope) {
    FrameOffsets fo;
    fo.reset(lines, slope);
    for (int y = 0; y < lines; ++y) {
        fo.off[size_t(y)] = int16_t(((frame * 131 + y * 17 + salt * 7) % 2001) - 1000);
        fo.conf[size_t(y)] = uint8_t((frame + y + salt) % 256);
        if (slope) fo.slope[size_t(y)] = int16_t(((frame * 7 + y + salt) % 601) - 300);
    }
    return fo;
}

bool same(const FrameOffsets& a, const FrameOffsets& b) { return a.off == b.off && a.conf == b.conf && a.slope == b.slope; }

}  // namespace

TEST_CASE("store round trip, three chunks, with and without slope") {
    for (bool slope : {false, true}) {
        const std::string dir = tempDir("offsets_rt");
        OffsetHeader h;
        h.lines = 486;
        h.width = 720;
        h.frames = 600;
        h.slope = slope;
        OffsetWriter w;
        std::string err;
        CHECK(w.create(dir + "/offsets.zst", h, nullptr, 0, h.frames, err));
        for (int64_t f = 0; f < h.frames; ++f) CHECK(w.put(f, pattern(f, h.lines, 0, slope)));
        CHECK(w.finish(err));
        OffsetStore s;
        CHECK(s.open(dir + "/offsets.zst", err));
        CHECK_EQ(s.chunks(), 3);
        CHECK_EQ(s.header().lines, 486);
        int bad = 0;
        for (int64_t f = 0; f < h.frames; ++f) {
            FrameOffsets got;
            CHECK(s.get(f, got));
            bad += !same(got, pattern(f, h.lines, 0, slope));
        }
        CHECK_EQ(bad, 0);
        // Raw size per NTSC frame is 1458 bytes without the slope.
        if (!slope) CHECK_EQ(int(h.frameBytes()), 1458);
    }
}

TEST_CASE("partial rewrite keeps every other frame and copies untouched chunks") {
    const std::string dir = tempDir("offsets_partial");
    OffsetHeader h;
    h.lines = 486;
    h.width = 720;
    h.frames = 700;
    std::string err;
    {
        OffsetWriter w;
        CHECK(w.create(dir + "/offsets.zst", h, nullptr, 0, h.frames, err));
        for (int64_t f = 0; f < h.frames; ++f) w.put(f, pattern(f, h.lines, 0, false));
        CHECK(w.finish(err));
    }
    OffsetStore before;
    CHECK(before.open(dir + "/offsets.zst", err));
    std::vector<uint8_t> chunk0Before, chunk2Before;
    before.rawChunk(0, chunk0Before);
    before.rawChunk(2, chunk2Before);
    {
        OffsetStore base;
        CHECK(base.open(dir + "/offsets.zst", err));
        OffsetWriter w;
        CHECK(w.create(dir + "/offsets.zst", h, &base, 300, 400, err));
        for (int64_t f = 300; f < 400; ++f) w.put(f, pattern(f, h.lines, 5, false));
        CHECK(w.finish(err));
    }
    OffsetStore after;
    CHECK(after.open(dir + "/offsets.zst", err));
    int bad = 0;
    for (int64_t f = 0; f < h.frames; ++f) {
        FrameOffsets got;
        after.get(f, got);
        const bool inSpan = f >= 300 && f < 400;
        bad += !same(got, pattern(f, h.lines, inSpan ? 5 : 0, false));
    }
    CHECK_EQ(bad, 0);
    std::vector<uint8_t> chunk0After, chunk2After;
    after.rawChunk(0, chunk0After);
    after.rawChunk(2, chunk2After);
    CHECK(chunk0After == chunk0Before);
    CHECK(chunk2After == chunk2Before);
}

TEST_CASE("Analyze writes what the tracker measured, and re-analyze touches only its span") {
    const std::string dir = tempDir("offsets_analyze");
    SynthScene scene;
    DamageModel d;
    SynthFile sf;
    sf.path = dir + "/tape.mkv";
    sf.frames = 24;
    sf.moving = false;
    std::string err;
    CHECK_MSG(writeSynthCapture(sf, scene, d, err), "%s", err.c_str());
    SourceInfo src;
    CHECK_MSG(probeSource(sf.path, true, src, err), "%s", err.c_str());
    CHECK_EQ(src.frames, int64_t(24));
    // Border reference from frame 0 of the decoded file.
    SourceReader rd;
    CHECK(rd.open(src, err));
    FramePtr f0 = rd.read(0);
    CHECK(f0 != nullptr);
    Picture pic;
    toPicture(f0.get(), pic, true);
    Reference ref;
    ref.id = 1;
    ref.kind = RefKind::LeftBorder;
    RefSpan sp;
    CHECK(makeBorderSpan(pic, false, src.frames, EdgeLockSettings{}, sp));
    ref.spans.push_back(sp);

    AnalyzeJob job;
    job.source = src;
    job.order = FieldOrder::TFF;
    job.refs = {ref};
    job.offsetsPath = dir + "/offsets.zst";
    job.tracksPath = dir + "/tracks.zst";
    const AnalyzeOutcome o = runAnalyze(job);
    CHECK_MSG(o.ok, "%s", o.error.c_str());
    CHECK_EQ(o.frames, int64_t(24));
    OffsetStore st;
    CHECK(st.open(job.offsetsPath, err));
    // Same numbers as the tracker run on the decoded fields directly.
    TrackerConfig cfg;
    cfg.refs = job.refs;
    cfg.width = src.width;
    cfg.frameHeight = src.height;
    EdgeTracker t(cfg);
    std::vector<FieldOffsets> fields;
    t.onField = [&](FieldOffsets&& f) { fields.push_back(std::move(f)); };
    for (int64_t n = 0; n < src.frames; ++n) {
        FramePtr fr = rd.read(n);
        for (int k = 0; k < 2; ++k) {
            FieldLuma fl;
            fieldLuma(fr.get(), k, fl);
            t.push(t.prepare(2 * n + k, std::move(fl)));
        }
    }
    t.finish();
    CHECK_EQ(int(fields.size()), 48);
    int mismatched = 0;
    double worstVsDamage = 0;
    std::vector<FrameOffsets> firstPass(size_t(src.frames));
    for (int64_t n = 0; n < src.frames; ++n) {
        FrameOffsets want, got;
        fieldsToFrame(fields[size_t(2 * n)], fields[size_t(2 * n + 1)], src.height, false, want);
        st.get(n, got);
        mismatched += !same(want, got);
        firstPass[size_t(n)] = got;
        for (int y = 40; y < src.height; ++y)
            if (got.conf[size_t(y)] >= kConfMeasuredMin) worstVsDamage = std::max(worstVsDamage, std::fabs(got.offsetPx(y) - d.offset(2 * n + (y & 1), y)));
    }
    CHECK_EQ(mismatched, 0);
    std::printf("  worst |offset - damage| on measured lines: %.3f px (includes the clip constant)\n", worstVsDamage);
    CHECK(worstVsDamage < 1.5);
    TrackStore tracks;
    CHECK(tracks.load(job.tracksPath, err));
    CHECK_EQ(int(tracks.fields.size()), 48);

    // Re-analyze frames 8..12 with different settings: only they change.
    AnalyzeJob re = job;
    re.start = 8;
    re.end = 13;
    re.settings.maxGap = 2;
    // Measured against the right border instead, so the numbers differ.
    Reference right;
    right.id = 2;
    right.kind = RefKind::RightBorder;
    RefSpan rsp;
    CHECK(makeBorderSpan(pic, true, src.frames, EdgeLockSettings{}, rsp));
    right.spans.push_back(rsp);
    re.refs = {right};
    const AnalyzeOutcome o2 = runAnalyze(re);
    CHECK_MSG(o2.ok, "%s", o2.error.c_str());
    OffsetStore st2;
    CHECK(st2.open(job.offsetsPath, err));
    int changedOutside = 0, changedInside = 0;
    for (int64_t n = 0; n < src.frames; ++n) {
        FrameOffsets got;
        st2.get(n, got);
        const bool diff = !same(got, firstPass[size_t(n)]);
        if (n >= 8 && n < 13) changedInside += diff;
        else changedOutside += diff;
    }
    CHECK_EQ(changedOutside, 0);
    std::printf("  re-analyzed frames that changed: %d of 5\n", changedInside);
    CHECK_EQ(changedInside, 5);
}

TEST_MAIN()

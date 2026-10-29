// SPDX-License-Identifier: GPL-3.0-or-later
// Three cut spans, one splitting a chapter: frame count, audio sample count
// and A/V offset exact at every join, chapters and captions shifted per the
// cut rules, no click at the crossfades, IVTC cuts on cadence.
#include "core/Synth.h"
#include "testing.h"

#include "core/Deinterlace.h"
#include "core/Edit.h"
#include "core/Render.h"
#include "shared/Checksums.h"

#include <cmath>
#include <cstdio>
#include <cstring>

extern "C" {
#include <libavutil/channel_layout.h>
}

using namespace ud;

namespace {

// Every audio sample of a file, interleaved s16.
std::vector<int16_t> decodeAudio(const std::string& path, int& channels) {
    std::vector<int16_t> out;
    SourceInfo s;
    std::string err;
    if (!probeSource(path, false, s, err) || !s.hasAudio) return out;
    channels = s.channels;
    AudioReader r;
    if (!r.open(s, err)) return out;
    // Read past the end; the reader pads with silence, so trim by the count.
    AVFormatContext* fc = nullptr;
    avformat_open_input(&fc, path.c_str(), nullptr, nullptr);
    avformat_find_stream_info(fc, nullptr);
    const int ai = av_find_best_stream(fc, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
    const AVCodec* dec = avcodec_find_decoder(fc->streams[ai]->codecpar->codec_id);
    AVCodecContext* dc = avcodec_alloc_context3(dec);
    avcodec_parameters_to_context(dc, fc->streams[ai]->codecpar);
    avcodec_open2(dc, dec, nullptr);
    AVPacket* p = av_packet_alloc();
    AVFrame* f = av_frame_alloc();
    auto take = [&] {
        while (avcodec_receive_frame(dc, f) == 0) {
            for (int i = 0; i < f->nb_samples; ++i)
                for (int c = 0; c < channels; ++c) {
                    int32_t v = 0;
                    if (f->format == AV_SAMPLE_FMT_S16) v = reinterpret_cast<int16_t*>(f->data[0])[i * channels + c];
                    else if (f->format == AV_SAMPLE_FMT_S16P) v = reinterpret_cast<int16_t*>(f->extended_data[c])[i];
                    else if (f->format == AV_SAMPLE_FMT_S32) v = reinterpret_cast<int32_t*>(f->data[0])[i * channels + c] >> 16;
                    out.push_back(int16_t(v));
                }
            av_frame_unref(f);
        }
    };
    while (av_read_frame(fc, p) >= 0) {
        if (p->stream_index == ai) {
            avcodec_send_packet(dc, p);
            take();
        }
        av_packet_unref(p);
    }
    avcodec_send_packet(dc, nullptr);
    take();
    av_frame_free(&f);
    av_packet_free(&p);
    avcodec_free_context(&dc);
    avformat_close_input(&fc);
    return out;
}

int16_t sourceSample(int64_t k, int c, int rate) { return int16_t(std::lround(9000.0 * std::sin(2 * M_PI * 440.0 * double(k) / rate + c))); }

}  // namespace

TEST_CASE("three cuts, one splitting a chapter") {
    const std::string dir = tempDir("cuts");
    SynthScene scene;
    const DamageModel d = DamageModel::none();
    SynthFile f;
    f.path = dir + "/tape.mkv";
    f.frames = 60;  // chapters at 0 and 30
    f.chapters = true;
    f.captions = true;
    std::string err;
    CHECK_MSG(writeSynthCapture(f, scene, d, err), "%s", err.c_str());
    RenderJob job;
    CHECK(probeSource(f.path, true, job.source, err));
    const SourceInfo& src = job.source;
    Project& p = job.project;
    p.folder = dir + "/project";
    p.sourcePath = f.path;
    p.fieldOrder = FieldOrder::TFF;
    p.deint.mode = DeintMode::None;
    p.out.codec = VideoCodec::Ffv1;
    p.out.audio = AudioCodec::Flac;
    p.edit.cuts = {{5, 10}, {20, 35}, {50, 55}};
    job.outPath = dir + "/out.mkv";
    const RenderOutcome o = runRender(job);
    CHECK_MSG(o.ok, "%s", o.error.c_str());
    if (!o.ok) return;
    // Kept: [0,5) [10,20) [35,50) [55,60) = 35 frames.
    CHECK_EQ(o.videoFrames, int64_t(35));
    const EditList el = EditList::build(src.frames, p.edit);
    const AudioPlan plan = planAudio(el, src);
    CHECK_EQ(o.audioSamples, plan.totalSamples());

    // Frames are the right ones.
    {
        SourceInfo os;
        CHECK(probeSource(job.outPath, false, os, err));
        CHECK_EQ(os.frames, int64_t(35));
        SourceReader a, b;
        a.open(src, err);
        b.open(os, err);
        int wrong = 0;
        for (int64_t k = 0; k < 35; ++k) {
            FramePtr fa = a.read(el.sourceOf(k)), fb = b.read(k);
            uint8_t ma[16], mb[16];
            int sa, sb;
            frameMd5(fa.get(), ma, sa);
            frameMd5(fb.get(), mb, sb);
            wrong += std::memcmp(ma, mb, 16) != 0;
        }
        CHECK_EQ(wrong, 0);
    }

    // Audio: exact count, every sample in place away from the joins, no click at them.
    int ch = 2;
    const std::vector<int16_t> au = decodeAudio(job.outPath, ch);
    CHECK_EQ(int64_t(au.size()) / ch, plan.totalSamples());
    int64_t outPos = 0, misplaced = 0;
    std::vector<int64_t> joins;
    for (size_t k = 0; k < plan.parts.size(); ++k) {
        const auto& part = plan.parts[k];
        if (k) joins.push_back(outPos);
        for (int64_t s = part.start; s < part.end; ++s, ++outPos) {
            bool nearJoin = false;
            for (int64_t j : joins) nearJoin |= std::llabs(outPos - j) <= plan.half;
            if (k + 1 < plan.parts.size()) nearJoin |= (part.end - s) <= plan.half;
            if (nearJoin || size_t(outPos * ch + 1) >= au.size()) continue;
            for (int c = 0; c < ch; ++c) misplaced += au[size_t(outPos * ch + c)] != sourceSample(s, c, src.sampleRate);
        }
    }
    CHECK_EQ(misplaced, int64_t(0));
    int maxStep = 0;
    for (size_t i = size_t(ch); i < au.size(); ++i) maxStep = std::max(maxStep, std::abs(int(au[i]) - int(au[i - size_t(ch)])));
    const int sineStep = int(std::ceil(9000.0 * 2 * M_PI * 440.0 / src.sampleRate));
    std::printf("  largest sample step %d (the sine alone steps up to %d)\n", maxStep, sineStep);
    CHECK_MSG(maxStep < sineStep * 2, "click at a join: step %d", maxStep);

    // Chapters: Recording 1 at 0 and again at 5 (split by the first cut),
    // Recording 2 at 15 (it began inside the second cut) and again at 30.
    JsonValue j;
    CHECK(ffprobeJson(job.outPath, "-show_chapters", j, err));
    const double fr = 1001.0 / 30000.0;
    const double want[] = {0, 5 * fr, 15 * fr, 30 * fr};
    CHECK_EQ(int(j["chapters"].size()), 4);
    for (size_t i = 0; i < 4 && i < j["chapters"].size(); ++i) {
        const double at = std::atof(j["chapters"][i]["start_time"].str().c_str());
        CHECK_MSG(std::fabs(at - want[i]) < 0.002, "chapter %zu at %.4f, want %.4f", i, at, want[i]);
    }
    // Captions: the first cue (0.1-0.4 s) is split by the first cut.
    const std::vector<Caption> cues = mapCaptions(src.captions[0].cues, el, src.fps);
    CHECK(cues.size() >= 3);
    if (cues.size() >= 2) {
        CHECK(std::fabs(cues[0].start - 0.1) < 1e-6);
        CHECK(std::fabs(cues[0].end - 5 * fr) < 1e-6);
        CHECK(std::fabs(cues[1].start - 5 * fr) < 1e-6);
        CHECK(std::fabs(cues[1].end - (0.4 - 5 * fr)) < 1e-6);
    }
    JsonValue pk;
    CHECK(ffprobeJson(job.outPath, "-select_streams s -show_packets", pk, err));
    CHECK_EQ(int(pk["packets"].size()), int(cues.size()));
}

TEST_CASE("IVTC: cuts land on the film cadence") {
    const std::string dir = tempDir("cuts_ivtc");
    SynthScene scene;
    scene.telecine = true;
    scene.telecineOffset = 2;
    scene.bars = {};
    scene.panX = 120;
    scene.panPerFilm = 8;
    const DamageModel d = DamageModel::none();
    SynthFile f;
    f.path = dir + "/film.mkv";
    f.frames = 60;
    f.moving = false;
    std::string err;
    CHECK_MSG(writeSynthCapture(f, scene, d, err), "%s", err.c_str());
    RenderJob job;
    CHECK(probeSource(f.path, true, job.source, err));
    // The cadence as the render sees it.
    SourceReader r;
    r.open(job.source, err);
    std::vector<double> comb;
    for (int64_t n = 0; n < 60; ++n) comb.push_back(combMetric(r.read(n).get()));
    const int phase = cadencePhase(comb, 0);
    std::printf("  cadence phase %d\n", phase);
    CHECK(phase >= 0);
    // Combed frames are video frames 2 and 3 of each cycle, offset by 2: 0, 1, 5, 6...
    CHECK_EQ(phase, 0);
    Project& p = job.project;
    p.folder = dir + "/project";
    p.sourcePath = f.path;
    p.fieldOrder = FieldOrder::TFF;
    p.deint.mode = DeintMode::Ivtc;
    p.out.codec = VideoCodec::Ffv1;
    p.edit.cuts = {{17, 31}};
    job.outPath = dir + "/out.mkv";
    const RenderOutcome o = runRender(job);
    CHECK_MSG(o.ok, "%s", o.error.c_str());
    if (!o.ok) return;
    // Clean cycle starts are 3, 8, 13 ...: 17 snaps to 18, 31 to 33, the
    // clip's start moves in to 3 and its end to 58, so both ranges hold whole cycles.
    const EditList el = EditList::build(60, p.edit, [&](int64_t x) { return snapToCadenceInside(x, phase, 60); });
    CHECK_EQ(int(el.ranges().size()), 2);
    if (el.ranges().size() == 2) {
        CHECK_EQ(el.ranges()[0].start, int64_t(3));
        CHECK_EQ(el.ranges()[0].end, int64_t(18));
        CHECK_EQ(el.ranges()[1].start, int64_t(33));
        CHECK_EQ(el.ranges()[1].end, int64_t(58));
    }
    CHECK_EQ(o.videoFrames, el.outputFrames() * 4 / 5);
    std::printf("  %lld frames out of %lld kept\n", (long long)o.videoFrames, (long long)el.outputFrames());
    // Every output frame is one whole film frame, each a new one.
    SourceInfo os;
    CHECK(probeSource(job.outPath, false, os, err));
    SourceReader ro;
    ro.open(os, err);
    int combed = 0, repeats = 0;
    double prev = -1;
    for (int64_t k = 0; k < os.frames; ++k) {
        FramePtr fr = ro.read(k);
        if (!fr) break;
        if (combMetric(fr.get()) > 5.0) ++combed;
        // Bar position on two lines of opposite fields must agree.
        FieldLuma a, b;
        fieldLuma(fr.get(), 0, a);
        fieldLuma(fr.get(), 1, b);
        auto edgeAt = [](const FieldLuma& fl) {
            const float* row = fl.row(100);
            for (int x = 20; x < fl.width - 1; ++x)
                if (row[x] < 167 && row[x + 1] >= 167) return x + (167 - row[x]) / (row[x + 1] - row[x]);
            return -1.0f;
        };
        const double ea = edgeAt(a), eb = edgeAt(b);
        if (std::fabs(ea - eb) > 1.0) ++combed;
        if (prev >= 0 && std::fabs(ea - prev) < 1.0) ++repeats;
        prev = ea;
    }
    CHECK_EQ(combed, 0);
    // The join itself may repeat nothing either: the cut removed whole film frames.
    CHECK_EQ(repeats, 0);
}

TEST_MAIN()

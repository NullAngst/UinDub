// SPDX-License-Identifier: GPL-3.0-or-later
#include "Warble.h"

#include "Output.h"
#include "Simd.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>

namespace ud {

namespace fs = std::filesystem;

double warbleTruth(const DamageModel& d, FieldOrder order, int64_t frame, int y) {
    // Temporal field 2n is the first field: the top one for TFF.
    const int k = (y & 1) == firstParity(order) ? 0 : 1;
    return d.offset(2 * frame + k, y);
}

WarbleResult makeWarbleCopy(const WarbleJob& job, const std::function<bool(double)>& progress) {
    WarbleResult res;
    const SourceInfo& s = job.clean;
    const int64_t start = std::clamp<int64_t>(job.start, 0, s.frames);
    const int64_t count = job.frames < 0 ? s.frames - start : std::clamp<int64_t>(job.frames, 0, s.frames - start);
    if (count <= 0) {
        res.error = "nothing to copy";
        return res;
    }
    std::string err;
    SourceReader reader;
    if (!reader.open(s, err)) {
        res.error = err;
        return res;
    }
    std::error_code ec;
    fs::create_directories(fs::path(job.outPath).parent_path(), ec);
    // FFV1 at the source's own layout, lossless, with the audio copied.
    OutputSettings o;
    o.codec = VideoCodec::Ffv1;
    o.audio = AudioCodec::Copy;
    MuxVideo mv;
    mv.width = s.width;
    mv.height = s.height;
    mv.pixfmt = encoderPixelFormat(o, s);
    mv.fps = s.fps;
    mv.sar = s.sar;
    mv.field = job.order;
    mv.color = s.color;
    MuxAudio ma;
    ma.present = s.hasAudio;
    ma.rate = s.sampleRate;
    ma.channels = s.channels;
    ma.bits = s.audioBits <= 16 ? 16 : 24;
    std::map<std::string, std::string> tags;
    tags["TITLE"] = "Warble test copy of " + fs::path(s.path).filename().string();
    tags["UINDUB_WARBLE"] = "1";
    OutputWriter w;
    if (!w.open(job.outPath, o, mv, ma, {}, {}, tags, err)) {
        res.error = err;
        return res;
    }
    AudioReader audio;
    if (s.hasAudio) audio.open(s, err);
    std::vector<float> tmp(static_cast<size_t>(s.width));
    for (int64_t i = 0; i < count; ++i) {
        const int64_t n = start + i;
        FramePtr f = reader.read(n);
        Picture pic;
        if (!f || !toPicture(f.get(), pic, true)) {
            w.abort();
            res.error = "cannot decode frame " + std::to_string(n);
            return res;
        }
        for (int y = 0; y < pic.height; ++y) {
            // The picture on this line sits d px right of where it belongs.
            const double d = warbleTruth(job.damage, job.order, i, y);
            if (d == 0) continue;
            for (int c = 0; c < pic.planes(); ++c) {
                shiftRow(pic.p[c].row(y), tmp.data(), pic.width, -d, c == 0 ? 16.0f : 128.0f, false);
                std::memcpy(pic.p[c].row(y), tmp.data(), sizeof(float) * size_t(pic.width));
            }
        }
        FramePtr out = fromPicture(pic, mv.pixfmt, true);
        if (!out || !w.writeVideo(out.get(), err)) {
            w.abort();
            res.error = err.empty() ? "cannot encode" : err;
            return res;
        }
        if (s.hasAudio) {
            const int64_t a = s.sampleOfFrame(n), b = s.sampleOfFrame(n + 1);
            std::vector<int32_t> buf;
            audio.read(a, b - a, buf);
            w.writeAudio(buf, err);
        }
        if (progress && i % 8 == 0 && !progress(double(i) / double(count))) {
            w.abort();
            res.error = "cancelled";
            return res;
        }
    }
    if (!w.finish(err)) {
        res.error = err;
        return res;
    }

    // The project: the truth, and a left border reference when there is one.
    Project p;
    p.folder = job.projectFolder.empty() ? defaultProjectFolder(job.outPath) : job.projectFolder;
    p.sourcePath = job.outPath;
    p.fieldOrder = job.order;
    p.edgeEnabled = true;
    p.hasTruth = true;
    p.truth = job.damage;
    p.tapeLabel = "Warble test";
    SourceInfo copy;
    if (probeSource(job.outPath, false, copy, err)) {
        SourceReader r2;
        if (r2.open(copy, err)) {
            FramePtr f0 = r2.read(0);
            Picture pic;
            if (f0 && toPicture(f0.get(), pic, true)) {
                RefSpan sp;
                if (makeBorderSpan(pic, false, copy.frames, p.edge, sp)) {
                    Reference ref;
                    ref.id = 1;
                    ref.kind = RefKind::LeftBorder;
                    ref.name = "Left border";
                    ref.spans.push_back(sp);
                    p.refs.push_back(ref);
                }
            }
        }
    }
    if (!p.save(err)) {
        res.error = err;
        return res;
    }
    res.ok = true;
    res.project = p;
    return res;
}

WarbleScore scoreWarble(const OffsetStore& store, const DamageModel& truth, FieldOrder order, int64_t frames) {
    WarbleScore sc;
    if (!store.isOpen()) return sc;
    const int L = store.header().lines;
    frames = std::min(frames, store.header().frames);
    double bias = 0;
    int nb = 0;
    FrameOffsets fo;
    for (int64_t n = 0; n < frames; ++n) {
        store.get(n, fo);
        for (int y = 0; y < L; ++y)
            if (fo.conf[size_t(y)] >= kConfMeasuredMin) {
                bias += fo.offsetPx(y) - warbleTruth(truth, order, n, y);
                ++nb;
            }
    }
    sc.bias = nb ? bias / nb : 0;
    std::vector<double> lineSum(static_cast<size_t>(L), 0.0);
    std::vector<int> lineN(static_cast<size_t>(L), 0);
    sc.perField.assign(size_t(frames) * 2, 0.0);
    std::vector<int> fieldN(size_t(frames) * 2, 0);
    double sm = 0, sa = 0;
    for (int64_t n = 0; n < frames; ++n) {
        store.get(n, fo);
        for (int y = 0; y < L; ++y) {
            const double e = fo.offsetPx(y) - warbleTruth(truth, order, n, y) - sc.bias;
            sa += e * e;
            ++sc.lines;
            if (fo.conf[size_t(y)] < kConfMeasuredMin) continue;
            sm += e * e;
            ++sc.measured;
            lineSum[size_t(y)] += e * e;
            ++lineN[size_t(y)];
            const int k = (y & 1) == firstParity(order) ? 0 : 1;
            sc.perField[size_t(2 * n + k)] += e * e;
            ++fieldN[size_t(2 * n + k)];
        }
    }
    sc.rmsMeasured = sc.measured ? std::sqrt(sm / sc.measured) : 0;
    sc.rmsAll = sc.lines ? std::sqrt(sa / sc.lines) : 0;
    sc.perLine.resize(size_t(L));
    for (int y = 0; y < L; ++y) sc.perLine[size_t(y)] = lineN[size_t(y)] ? std::sqrt(lineSum[size_t(y)] / lineN[size_t(y)]) : 0;
    for (size_t i = 0; i < sc.perField.size(); ++i) sc.perField[i] = fieldN[i] ? std::sqrt(sc.perField[i] / fieldN[i]) : 0;
    sc.ok = sc.measured > 0;
    return sc;
}

}  // namespace ud

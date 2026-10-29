// SPDX-License-Identifier: GPL-3.0-or-later
#include "Analyze.h"

#include "ThreadPool.h"

#include <chrono>
#include <condition_variable>
#include <deque>
#include <future>
#include <mutex>
#include <thread>

namespace ud {

std::set<int64_t> resetFramesOf(const SourceInfo& s) {
    std::set<int64_t> r;
    for (const SourceEvent& e : s.events) {
        if (e.kind != SourceEvent::Kind::SignalLost && e.kind != SourceEvent::Kind::Insert) continue;
        r.insert(e.frame);
        r.insert(e.endFrame + 1);
    }
    return r;
}

AnalyzeOutcome runAnalyze(const AnalyzeJob& job, const AnalyzeProgress& progress) {
    AnalyzeOutcome out;
    const auto t0 = std::chrono::steady_clock::now();
    const SourceInfo& src = job.source;
    const int64_t start = std::clamp<int64_t>(job.start, 0, src.frames);
    const int64_t end = job.end < 0 ? src.frames : std::clamp<int64_t>(job.end, start, src.frames);
    if (end <= start) {
        out.error = "Nothing to analyze.";
        return out;
    }
    if (job.refs.empty()) {
        out.error = "Set at least one reference edge first.";
        return out;
    }

    TrackerConfig cfg;
    cfg.s = job.settings;
    cfg.refs = job.refs;
    cfg.width = src.width;
    cfg.frameHeight = src.height;
    cfg.order = job.order;
    cfg.resetFrames = job.resetFrames.empty() ? resetFramesOf(src) : job.resetFrames;
    EdgeTracker tracker(cfg);

    // Re-analysis writes on top of what is there.
    OffsetStore base;
    std::string err;
    const bool partial = start > 0 || end < src.frames;
    if (partial) base.open(job.offsetsPath, err);
    TrackStore tracks;
    if (partial) tracks.load(job.tracksPath, err);
    tracks.fields.resize(size_t(src.frames) * 2);

    OffsetHeader h;
    h.lines = src.height;
    h.width = src.width;
    h.frames = src.frames;
    h.slope = job.settings.lengthCorrection;
    h.xref = float(0.5 * src.width);
    h.firstParity = firstParity(job.order);
    OffsetWriter writer;
    if (!writer.create(job.offsetsPath, h, partial ? &base : nullptr, start, end, err)) {
        out.error = err;
        return out;
    }

    // Finished fields pair up into frames for the store.
    std::deque<FieldOffsets> ready;
    bool writeFailed = false;
    tracker.onField = [&](FieldOffsets&& f) {
        if (f.field >= 0 && size_t(f.field) < tracks.fields.size()) tracks.fields[size_t(f.field)] = f.track;
        ready.push_back(std::move(f));
        while (ready.size() >= 2) {
            if (ready[0].field / 2 != ready[1].field / 2) {
                ready.pop_front();  // a lone field (never happens with whole frames)
                continue;
            }
            FrameOffsets fo;
            fieldsToFrame(ready[0], ready[1], h.lines, h.slope, fo);
            if (!writer.put(ready[0].field / 2, fo)) writeFailed = true;
            ready.pop_front();
            ready.pop_front();
        }
    };

    SourceReader reader;
    if (!reader.open(src, err, 0)) {
        out.error = err;
        return out;
    }
    ThreadPool pool(job.threads);
    const int p0 = firstParity(job.order);
    // Decode on this thread, prepare fields on the pool, push in order. A
    // small window keeps every core busy without holding many frames.
    const int window = std::max(4, pool.size() * 2);
    std::deque<std::future<std::pair<PreparedField, PreparedField>>> inflight;
    int64_t next = start, pushed = start;
    bool cancelled = false, failed = false;
    auto pushOne = [&] {
        auto pr = inflight.front().get();
        inflight.pop_front();
        tracker.push(std::move(pr.first));
        tracker.push(std::move(pr.second));
        ++pushed;
        if (progress && (pushed % 8 == 0 || pushed == end) && !progress(double(pushed - start) / double(end - start), pushed)) cancelled = true;
    };
    int64_t last = end;  // shrinks if the file turns out shorter than it said
    while ((next < last || !inflight.empty()) && !cancelled && !failed) {
        while (next < last && int(inflight.size()) < window) {
            FramePtr fr = reader.read(next);
            if (!fr) {
                if (next == start) {
                    out.error = "Cannot decode frame " + std::to_string(next) + ": " + reader.lastError();
                    failed = true;
                }
                last = next;  // a short file: analyze what is there
                break;
            }
            const int64_t n = next++;
            SharedFrame sf = shareFrame(std::move(fr));
            inflight.push_back(pool.submit([&tracker, sf, n, p0] {
                FieldLuma a, b;
                fieldLuma(sf.get(), p0, a);
                fieldLuma(sf.get(), 1 - p0, b);
                return std::make_pair(tracker.prepare(2 * n, std::move(a)), tracker.prepare(2 * n + 1, std::move(b)));
            }));
        }
        if (inflight.empty()) break;
        pushOne();
    }
    while (!inflight.empty()) {
        inflight.front().wait();
        inflight.pop_front();
    }
    if (cancelled || failed) {
        writer.abort();
        out.cancelled = cancelled;
        return out;
    }
    tracker.finish();
    if (writeFailed || !writer.finish(err)) {
        out.error = writeFailed ? "Cannot write the offsets file." : err;
        return out;
    }
    if (!tracks.save(job.tracksPath, err)) {
        out.error = err;
        return out;
    }
    out.ok = true;
    out.tracker = tracker.result();
    out.frames = pushed - start;
    out.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    return out;
}

}  // namespace ud

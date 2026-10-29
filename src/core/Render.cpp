// SPDX-License-Identifier: GPL-3.0-or-later
#include "Render.h"

#include "Deinterlace.h"
#include "Edit.h"
#include "Output.h"
#include "Pipeline.h"
#include "Qtgmc.h"
#include "ThreadPool.h"
#include "shared/Checksums.h"
#include "shared/Json.h"

#include <chrono>
#include <cmath>
#include <ctime>
#include <filesystem>
#include <list>
#include <map>
#include <mutex>

extern "C" {
#include <libavutil/pixdesc.h>
}

namespace ud {

namespace fs = std::filesystem;

int64_t expectedOutputFrames(const Project& p, int64_t kept) {
    switch (p.deint.mode) {
    case DeintMode::None: return kept;
    case DeintMode::Ivtc: return kept * 4 / 5;
    default: return p.deint.doubleRate ? kept * 2 : kept;
    }
}

namespace {

std::string isoNow() {
    const std::time_t t = std::time(nullptr);
    char buf[32];
    std::strftime(buf, sizeof buf, "%Y-%m-%dT%H:%M:%S", std::localtime(&t));
    return buf;
}

// Processed input frames for QTGMC, which asks for neighbors more than once.
class FrameCache {
public:
    explicit FrameCache(size_t cap) : m_cap(cap) {}
    FramePtr get(int64_t n) {
        auto it = m_map.find(n);
        if (it == m_map.end()) return nullptr;
        return refFrame(it->second.get());
    }
    void put(int64_t n, const AVFrame* f) {
        if (m_map.count(n)) return;
        m_map[n] = refFrame(f);
        m_order.push_back(n);
        while (m_order.size() > m_cap) {
            m_map.erase(m_order.front());
            m_order.pop_front();
        }
    }

private:
    size_t m_cap;
    std::map<int64_t, FramePtr> m_map;
    std::list<int64_t> m_order;
};

}  // namespace

RenderOutcome runRender(const RenderJob& job, const RenderCallback& progress) {
    RenderOutcome out;
    const auto t0 = std::chrono::steady_clock::now();
    const Project& P = job.project;
    const SourceInfo& S = job.source;
    std::string err;
    auto report = [&](const std::string& stage, double frac, int64_t frames, int64_t total) {
        if (!progress) return true;
        RenderProgress rp;
        rp.stage = stage;
        rp.fraction = frac;
        rp.frames = frames;
        rp.total = total;
        const double sec = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        rp.fps = sec > 0 ? frames / sec : 0;
        return progress(rp);
    };

    // --- optional source check
    if (job.verifySource) {
        for (size_t i = 0; i < S.segments.size(); ++i) {
            const VerifyResult v = verifyCapture(S.segments[i].file, true, [&](double f, const std::string&) {
                return report("Verifying source", (double(i) + f) / double(S.segments.size()), 0, 0);
            });
            if (v.cancelled) {
                out.cancelled = true;
                return out;
            }
            if (!v.error.empty() || ((v.haveSha || v.haveFrameMd5) && !v.ok)) {
                out.error = "Verify source: " + S.segments[i].file + ": " + v.summary();
                return out;
            }
            out.notes.push_back("Verified " + fs::path(S.segments[i].file).filename().string() + ": " + v.summary());
        }
    }

    const OutputSettings O = effectiveOutput(P.out);
    if (O.audio != P.out.audio) out.notes.push_back("MP4 cannot carry FLAC or untouched PCM well: audio is AAC.");
    if (P.out.captions && !O.captions && !S.captions.empty()) out.notes.push_back("MP4: caption tracks left out.");
    const AVPixelFormat enc = encoderPixelFormat(O, S);
    const FieldOrder order = P.renderFieldOrder();
    const bool tff = order == FieldOrder::TFF;
    const Rational outFps = deintOutputRate(P.deint, S.fps);
    const OutputGeometry G = outputGeometry(P, S, enc);
    for (const std::string& n : G.notes) out.notes.push_back(n);

    InputStage in;
    if (!in.open(P, S, err)) {
        out.error = err;
        return out;
    }
    SourceReader reader;
    if (!reader.open(S, err)) {
        out.error = err;
        return out;
    }

    // Cuts snap to the film cadence with IVTC, so no stray field survives.
    std::function<int64_t(int64_t)> snap;
    if (P.deint.mode == DeintMode::Ivtc) {
        snap = [&](int64_t f) -> int64_t {
            const int64_t a = std::max<int64_t>(0, f - 30), b = std::min<int64_t>(S.frames, f + 30);
            std::vector<double> comb;
            for (int64_t n = a; n < b; ++n) {
                FramePtr fr = reader.read(n);
                comb.push_back(fr ? combMetric(fr.get()) : 0.0);
            }
            const int phase = cadencePhase(comb, a);
            return snapToCadenceInside(f, phase, S.frames);
        };
    }
    const EditList el = EditList::build(S.frames, P.edit, snap);
    if (el.empty()) {
        out.error = "Nothing left to render: the trim and cuts remove every frame.";
        return out;
    }

    const bool colorNeutral = P.color.neutral() && (!P.colorPerChapter || [&] {
                                  for (const auto& [k, v] : P.chapterColors)
                                      if (!v.neutral()) return false;
                                  return true;
                              }());
    const bool passthrough = !in.edgeActive() && P.deint.mode == DeintMode::None && colorNeutral && G.cropLeft == 0 && G.cropRight == 0 &&
                             G.cropTop == 0 && G.cropBottom == 0 && !G.resize && enc == AVPixelFormat(S.pixfmt);

    // --- container, tags, chapters, captions
    MuxVideo mv;
    mv.width = G.width;
    mv.height = G.height;
    mv.pixfmt = enc;
    mv.fps = outFps;
    mv.sar = G.sar;
    mv.field = P.deint.mode == DeintMode::None ? order : FieldOrder::Progressive;
    mv.color = S.color;
    MuxAudio ma;
    ma.present = S.hasAudio;
    ma.rate = S.sampleRate;
    ma.channels = S.channels;
    ma.bits = S.audioBits <= 16 ? 16 : (S.audioBits <= 24 ? 24 : 32);
    std::vector<OutputChapter> chapters;
    for (const Chapter& c : mapChapters(S.chapters, el))
        chapters.push_back({frameSeconds(c.start, S.fps), frameSeconds(c.end, S.fps), c.title});
    std::vector<OutputCaptionTrack> captions;
    for (const CaptionTrack& t : S.captions) captions.push_back({t.name, mapCaptions(t.cues, el, S.fps)});

    std::map<std::string, std::string> tags;
    const std::string label = !P.tapeLabel.empty() ? P.tapeLabel : S.tapeLabel;
    if (!label.empty()) tags["TITLE"] = label;
    tags["UINDUB_VERSION"] = UINDUB_VERSION;
    tags["UINDUB_SOURCE"] = fs::path(S.path).filename().string();
    std::vector<std::string> hashes;
    for (size_t i = 0; i < S.segments.size(); ++i) {
        const SourceSegment& seg = S.segments[i];
        std::string hex = seg.sha256.empty() ? std::string() : readSha256Sidecar(seg.sha256);
        if (hex.empty()) {
            if (!sha256File(seg.file, hex, err, [&](uint64_t d, uint64_t t) {
                    return report("Checksumming source", t ? (double(i) + double(d) / double(t)) / double(S.segments.size()) : 0, 0, 0);
                })) {
                if (err == "cancelled") {
                    out.cancelled = true;
                    return out;
                }
                hex = "unavailable";
            }
        }
        hashes.push_back(hex);
    }
    {
        std::string all;
        for (size_t i = 0; i < hashes.size(); ++i) {
            if (i) all += ' ';
            all += hashes[i];
        }
        tags["UINDUB_SOURCE_SHA256"] = all;
    }
    tags["UINDUB_SETTINGS"] = P.settingsJson().dump(false);

    const std::string path = job.outPath.empty() ? outputPath(P.out, S) : job.outPath;
    {
        std::error_code ec;
        fs::create_directories(fs::path(path).parent_path(), ec);
    }
    OutputWriter writer;
    if (!writer.open(path, O, mv, ma, chapters, captions, tags, err)) {
        out.error = err;
        return out;
    }
    AudioCutter audio;
    if (S.hasAudio && !audio.open(S, planAudio(el, S), err)) {
        writer.abort();
        out.error = "audio: " + err;
        return out;
    }
    auto syncAudio = [&](std::string& e) -> bool {
        if (!S.hasAudio) return true;
        const int64_t want = std::min<int64_t>(audio.total(), int64_t(std::llround(double(writer.videoFrames()) * S.sampleRate * outFps.den / double(outFps.num))));
        if (want <= audio.produced()) return true;
        std::vector<int32_t> buf;
        if (!audio.next(want - audio.produced(), buf)) {
            e = "cannot read the audio";
            return false;
        }
        return writer.writeAudio(buf, e);
    };

    ThreadPool pool(job.threads);
    const int batch = std::max(4, pool.size() * 2);
    int64_t total = 0;
    for (const Range& r : el.ranges()) total += expectedOutputFrames(P, r.length());
    const AVPixelFormat work = S.gray ? AV_PIX_FMT_GRAY16 : AV_PIX_FMT_YUV444P16;
    bool failed = false, cancelled = false;
    std::mutex errMutex;
    auto fail = [&](const std::string& e) {
        std::lock_guard<std::mutex> lk(errMutex);
        if (!failed) out.error = e;
        failed = true;
    };

    // Writes progressive (or interlaced for None) frames after color and crop.
    // `srcOf` gives the source frame each one came from, for per-chapter color.
    auto postAndWrite = [&](std::vector<FramePtr>& frames, const std::vector<int64_t>& srcOf, bool interlaced) {
        std::vector<FramePtr> ready(frames.size());
        pool.parallelFor(int(frames.size()), [&](int i) {
            Picture pic;
            if (!toPicture(frames[size_t(i)].get(), pic, interlaced)) {
                fail("cannot read a frame for color and crop");
                return;
            }
            postProcess(pic, P, S, G, srcOf[size_t(i)]);
            ready[size_t(i)] = fromPicture(pic, enc, interlaced);
            if (!ready[size_t(i)]) fail("cannot convert a frame for the encoder");
        });
        for (FramePtr& f : ready) {
            if (failed) return;
            std::string e;
            if (!writer.writeVideo(f.get(), e) || !syncAudio(e)) {
                fail(e);
                return;
            }
        }
        if (!report("Rendering", total ? double(writer.videoFrames()) / double(total) : 0, writer.videoFrames(), total)) cancelled = true;
    };

    for (const Range& range : el.ranges()) {
        if (failed || cancelled) break;
        const int64_t len = range.length();
        if (passthrough) {
            for (int64_t n = range.start; n < range.end && !failed && !cancelled; ++n) {
                FramePtr f = reader.read(n);
                if (!f) {
                    fail("cannot decode frame " + std::to_string(n));
                    break;
                }
                std::string e;
                if (!writer.writeVideo(f.get(), e) || !syncAudio(e)) fail(e);
                if (n % 16 == 0 && !report("Rendering", double(writer.videoFrames()) / double(total), writer.videoFrames(), total)) cancelled = true;
            }
            continue;
        }
        if (P.deint.mode == DeintMode::None) {
            for (int64_t n = range.start; n < range.end && !failed && !cancelled; n += batch) {
                const int64_t m = std::min<int64_t>(batch, range.end - n);
                std::vector<FramePtr> dec(static_cast<size_t>(m));
                std::vector<int64_t> src(static_cast<size_t>(m));
                for (int64_t i = 0; i < m; ++i) {
                    dec[size_t(i)] = reader.read(n + i);
                    src[size_t(i)] = n + i;
                    if (!dec[size_t(i)]) fail("cannot decode frame " + std::to_string(n + i));
                }
                if (failed) break;
                std::vector<FramePtr> inter(static_cast<size_t>(m));
                pool.parallelFor(int(m), [&](int i) {
                    Picture pic;
                    if (!in.fromDecoded(dec[size_t(i)].get(), src[size_t(i)], pic)) {
                        fail("cannot read frame " + std::to_string(src[size_t(i)]));
                        return;
                    }
                    postProcess(pic, P, S, G, src[size_t(i)]);
                    inter[size_t(i)] = fromPicture(pic, enc, true);
                });
                for (FramePtr& f : inter) {
                    if (failed) break;
                    std::string e;
                    if (!writer.writeVideo(f.get(), e) || !syncAudio(e)) fail(e);
                }
                if (!report("Rendering", double(writer.videoFrames()) / double(total), writer.videoFrames(), total)) cancelled = true;
            }
            continue;
        }
        if (P.deint.mode == DeintMode::Qtgmc) {
            QtgmcConfig qc;
            qc.preset = P.deint.qtgmcPreset;
            qc.doubleRate = P.deint.doubleRate;
            qc.tff = tff;
            qc.width = S.width;
            qc.height = S.height;
            qc.gray = S.gray;
            qc.fps = S.fps;
            qc.sourceMatch = P.deint.sourceMatch;
            qc.sharpness = P.deint.sharpness;
            qc.threads = job.threads;
            SourceReader qreader;
            if (!qreader.open(S, err)) {
                fail(err);
                break;
            }
            FrameCache cache(48);
            QtgmcRunner q;
            const int64_t base = range.start;
            if (!q.open(qc, len, [&](int64_t i) -> FramePtr {
                    if (FramePtr hit = cache.get(i)) return hit;
                    Picture pic;
                    if (!in.frame(qreader, base + i, pic)) return nullptr;
                    FramePtr f = fromPicture(pic, work, true);
                    if (f) cache.put(i, f.get());
                    return f;
                }, err)) {
                fail(err);
                break;
            }
            const int64_t outN = q.outputFrames();
            for (int64_t k = 0; k < outN && !failed && !cancelled; k += batch) {
                const int64_t m = std::min<int64_t>(batch, outN - k);
                std::vector<FramePtr> frames;
                std::vector<int64_t> src;
                for (int64_t i = 0; i < m; ++i) {
                    FramePtr f = q.get(k + i, err);
                    if (!f) {
                        fail(err);
                        break;
                    }
                    frames.push_back(std::move(f));
                    src.push_back(base + (P.deint.doubleRate ? (k + i) / 2 : (k + i)));
                }
                if (!failed) postAndWrite(frames, src, false);
            }
            q.close();
            continue;
        }
        // bwdif, yadif, IVTC through FFmpeg's filters.
        FilterDeinterlacer d;
        if (!d.open(P.deint.mode, P.deint.doubleRate, tff, S.width, S.height, work, S.fps, S.sar, err)) {
            fail(err);
            break;
        }
        int64_t produced = 0;
        auto srcOfOut = [&](int64_t j) -> int64_t {
            if (P.deint.mode == DeintMode::Ivtc) return std::min(range.end - 1, range.start + j * 5 / 4);
            return std::min(range.end - 1, range.start + (P.deint.doubleRate ? j / 2 : j));
        };
        auto drainFilter = [&] {
            std::vector<FramePtr> frames;
            std::vector<int64_t> src;
            while (FramePtr f = d.pull()) {
                frames.push_back(std::move(f));
                src.push_back(srcOfOut(produced++));
            }
            if (!frames.empty()) postAndWrite(frames, src, false);
        };
        for (int64_t n = range.start; n < range.end && !failed && !cancelled; n += batch) {
            const int64_t m = std::min<int64_t>(batch, range.end - n);
            std::vector<FramePtr> dec(static_cast<size_t>(m));
            for (int64_t i = 0; i < m; ++i) {
                dec[size_t(i)] = reader.read(n + i);
                if (!dec[size_t(i)]) fail("cannot decode frame " + std::to_string(n + i));
            }
            if (failed) break;
            std::vector<FramePtr> work16(static_cast<size_t>(m));
            pool.parallelFor(int(m), [&](int i) {
                Picture pic;
                if (!in.fromDecoded(dec[size_t(i)].get(), n + i, pic)) {
                    fail("cannot read frame " + std::to_string(n + i));
                    return;
                }
                work16[size_t(i)] = fromPicture(pic, work, true);
            });
            for (FramePtr& f : work16) {
                if (failed) break;
                if (!d.push(std::move(f), err)) fail(err);
            }
            drainFilter();
        }
        if (!failed && !cancelled) {
            if (!d.push(nullptr, err)) fail(err);
            drainFilter();
        }
    }

    if (cancelled || failed) {
        writer.abort();
        out.cancelled = cancelled && !failed;
        return out;
    }
    // The last of the audio, so both end together.
    if (S.hasAudio) {
        std::vector<int32_t> buf;
        while (audio.produced() < audio.total()) {
            if (!audio.next(std::min<int64_t>(65536, audio.total() - audio.produced()), buf) || !writer.writeAudio(buf, err)) {
                writer.abort();
                out.error = "audio: " + err;
                return out;
            }
        }
    }
    if (!writer.finish(err)) {
        out.error = err;
        return out;
    }
    out.videoFrames = writer.videoFrames();
    out.audioSamples = writer.audioSamples();
    out.outPath = path;

    // The sidecar: every setting, the version and the source hash.
    JsonValue sc = JsonValue::object();
    sc.set("uindub_version", UINDUB_VERSION).set("rendered", isoNow());
    JsonValue src = JsonValue::object();
    src.set("path", S.path).set("frames", (long long)S.frames).set("frame_rate", std::to_string(S.fps.num) + "/" + std::to_string(S.fps.den));
    JsonValue segs = JsonValue::array();
    for (size_t i = 0; i < S.segments.size(); ++i) {
        JsonValue g = JsonValue::object();
        g.set("file", S.segments[i].file).set("sha256", hashes[i]);
        segs.push(g);
    }
    src.set("segments", segs);
    sc.set("source", src);
    JsonValue o = JsonValue::object();
    o.set("path", path).set("frames", (long long)out.videoFrames).set("audio_samples", (long long)out.audioSamples);
    o.set("frame_rate", std::to_string(outFps.num) + "/" + std::to_string(outFps.den)).set("width", G.width).set("height", G.height);
    o.set("sar", std::to_string(G.sar.num) + ":" + std::to_string(G.sar.den)).set("pixel_format", av_get_pix_fmt_name(enc));
    JsonValue ranges = JsonValue::array();
    for (const Range& r : el.ranges()) {
        JsonValue e = JsonValue::array();
        e.push((long long)r.start).push((long long)r.end);
        ranges.push(e);
    }
    o.set("kept_ranges", ranges);
    JsonValue notes = JsonValue::array();
    for (const std::string& n : out.notes) notes.push(n);
    o.set("notes", notes);
    sc.set("output", o);
    sc.set("settings", P.settingsJson());
    // tape.uindub.mkv gets tape.uindub.json next to it.
    std::string stem = fs::path(path).stem().string();
    if (stem.size() < 7 || stem.compare(stem.size() - 7, 7, ".uindub") != 0) stem += ".uindub";
    const std::string side = (fs::path(path).parent_path() / (stem + ".json")).string();
    if (writeJsonFile(side, sc, err)) out.sidecar = side;
    out.ok = true;
    out.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    return out;
}

}  // namespace ud

// SPDX-License-Identifier: GPL-3.0-or-later
// Trim and cut: in and out points plus any number of spans cut from the
// middle. The kept ranges are what the render reads. Cuts land on source
// frame boundaries (on the film cadence with IVTC); audio is cut at the
// matching sample with a 5 ms crossfade at each join, and chapters and
// captions move with the picture.
#pragma once

#include "Source.h"
#include "Types.h"

#include <cstdint>
#include <functional>
#include <utility>
#include <vector>

namespace ud {

struct CutSpan {
    int64_t start = 0, end = 0;  // source frames, end exclusive
};

struct EditSettings {
    int64_t in = 0;
    int64_t out = -1;  // exclusive; -1 the end
    std::vector<CutSpan> cuts;
};

struct Range {
    int64_t start = 0, end = 0;  // source frames, end exclusive
    int64_t length() const { return end - start; }
};

class EditList {
public:
    // Kept ranges in order. `snap` moves each boundary (in, out, cut edges),
    // used for the IVTC cadence; identity when empty.
    static EditList build(int64_t frames, const EditSettings& e, const std::function<int64_t(int64_t)>& snap = {});
    const std::vector<Range>& ranges() const { return m_ranges; }
    int64_t outputFrames() const;  // source frames kept
    // Output position (in source frames) of a source frame; -1 if cut.
    int64_t mapFrame(int64_t src) const;
    // Output seconds of a source time; false if it falls in a cut.
    bool mapTime(double srcSec, const Rational& fps, double& outSec) const;
    // The source frame shown at an output position.
    int64_t sourceOf(int64_t outPos) const;
    bool empty() const { return m_ranges.empty(); }

private:
    std::vector<Range> m_ranges;
};

// Chapters on the output timeline (in source-frame units of output time).
// Inside a cut: dropped. After one: earlier. Split by one: starts again
// right after it, same title.
std::vector<Chapter> mapChapters(const std::vector<Chapter>& src, const EditList& el);
// Caption cues on the output timeline, in seconds; cut parts removed.
std::vector<Caption> mapCaptions(const std::vector<Caption>& src, const EditList& el, const Rational& fps);

// Audio sample spans for each kept range, and the crossfade at joins.
constexpr double kCrossfadeSeconds = 0.005;

struct AudioPlan {
    struct Part {
        int64_t start = 0, end = 0;  // source samples, end exclusive
    };
    std::vector<Part> parts;
    int64_t half = 0;  // samples each side of a join that crossfade
    int64_t totalSamples() const;
};
AudioPlan planAudio(const EditList& el, const SourceInfo& src);

// Renders the planned audio in blocks, joins crossfaded. Interleaved int32
// left-justified, like AudioReader.
class AudioCutter {
public:
    bool open(const SourceInfo& src, const AudioPlan& plan, std::string& err);
    // Next `count` samples per channel (fewer at the end; empty when done).
    bool next(int64_t count, std::vector<int32_t>& out);
    int64_t produced() const { return m_out; }
    int64_t total() const { return m_plan.totalSamples(); }

private:
    AudioReader m_reader;
    AudioPlan m_plan;
    int m_channels = 2;
    int64_t m_out = 0;  // output samples produced
};

}  // namespace ud

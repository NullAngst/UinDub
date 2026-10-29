// SPDX-License-Identifier: GPL-3.0-or-later
// Deinterlacing with FFmpeg's filters: bwdif, yadif and IVTC (fieldmatch +
// decimate). QTGMC lives in Qtgmc.h; None keeps the frames interlaced.
#pragma once

#include "Av.h"
#include "Types.h"

#include <memory>
#include <string>
#include <vector>

namespace ud {

enum class DeintMode { Qtgmc, Bwdif, Yadif, Ivtc, None };
const char* deintModeName(DeintMode m);
DeintMode deintModeFromName(const std::string& s);

struct DeintSettings {
    DeintMode mode = DeintMode::Qtgmc;
    bool doubleRate = true;          // QTGMC, bwdif, yadif
    std::string qtgmcPreset = "Slower";
    int sourceMatch = 0;             // QTGMC Advanced
    double sharpness = -1;           // QTGMC Advanced, -1 the preset's own
    bool operator==(const DeintSettings& o) const {
        return mode == o.mode && doubleRate == o.doubleRate && qtgmcPreset == o.qtgmcPreset && sourceMatch == o.sourceMatch &&
               sharpness == o.sharpness;
    }
};

// Output rate for an input rate.
Rational deintOutputRate(const DeintSettings& d, const Rational& in);

// A libavfilter graph fed interlaced frames, giving progressive ones back.
class FilterDeinterlacer {
public:
    FilterDeinterlacer();
    ~FilterDeinterlacer();
    FilterDeinterlacer(const FilterDeinterlacer&) = delete;
    FilterDeinterlacer& operator=(const FilterDeinterlacer&) = delete;

    // `fmt` is the pixel format frames will arrive in.
    bool open(DeintMode mode, bool doubleRate, bool tff, int width, int height, AVPixelFormat fmt, const Rational& fps,
              const Rational& sar, std::string& err);
    // nullptr flushes.
    bool push(FramePtr f, std::string& err);
    // Next output frame, or nullptr when none is ready.
    FramePtr pull();
    void close();

private:
    struct Impl;
    std::unique_ptr<Impl> m_i;
};

// --- telecine cadence ---------------------------------------------------------

// Combing of one frame (how much its two fields disagree), luma only.
double combMetric(const AVFrame* f);
// The phase of a 3:2 cadence from combing per frame: frames n with
// (n - phase) % 5 in {0, 1} are the two combed ones. -1 when no cadence.
int cadencePhase(const std::vector<double>& comb, int64_t firstFrame);
// The frame where a cadence cycle starts cleanly (no film frame's fields on
// both sides), nearest to `frame`.
int64_t snapToCadence(int64_t frame, int phase);
// The same, kept inside [0, frames]: the clip's own start and end move inward
// to a clean cycle, so every kept range holds whole cycles.
int64_t snapToCadenceInside(int64_t frame, int phase, int64_t frames);

}  // namespace ud

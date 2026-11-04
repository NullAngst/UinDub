// SPDX-License-Identifier: GPL-3.0-or-later
// Measurements for the Analyze tab that FFmpeg's own filters already make:
// idet (interlaced, progressive or telecined film, and which field leads),
// cropdetect (black borders) and ebur128 (loudness), plus a plain sample
// peak and clip count. Each reads the source on its own; none changes the
// project.
#pragma once

#include "Edit.h"
#include "Source.h"

#include <functional>
#include <string>

namespace ud {

using DetectProgress = std::function<bool(double fraction)>;  // false stops

struct IdetResult {
    bool ok = false;
    std::string error;
    int64_t tff = 0, bff = 0, progressive = 0, undetermined = 0;  // multi-frame detection
    int64_t repeatedTop = 0, repeatedBottom = 0, repeatedNeither = 0;
    int64_t frames = 0;
    // The reading of those counts.
    FieldOrder order = FieldOrder::Unknown;  // Progressive when most frames are
    bool film = false;                       // repeated fields in a 3:2 pattern's share
    double confidence = 0;                   // share of decided frames that agree
};
// Up to `windows` runs of `windowFrames` consecutive frames spread over [a, b).
IdetResult runIdet(const SourceInfo& s, int64_t a, int64_t b, int windows = 8, int windowFrames = 240, const DetectProgress& progress = {});

struct CropDetectResult {
    bool ok = false;
    std::string error;
    int left = 0, right = 0, top = 0, bottom = 0;  // black to remove, px
    int frames = 0;
};
// `samples` frames spread over [a, b); the borders are the ones black in all of them.
CropDetectResult runCropDetect(const SourceInfo& s, int64_t a, int64_t b, int samples = 120, const DetectProgress& progress = {});

struct LoudnessResult {
    bool ok = false;
    std::string error;
    double integrated = 0;  // LUFS
    double range = 0;       // LU
    double peakDb = -144;   // sample peak, dBFS
    int64_t clipped = 0;    // samples at full scale
    int64_t samples = 0;    // per channel, measured
};
// The audio the render would write (the kept ranges, joins crossfaded).
LoudnessResult runLoudness(const SourceInfo& s, const EditList& el, const DetectProgress& progress = {});

}  // namespace ud

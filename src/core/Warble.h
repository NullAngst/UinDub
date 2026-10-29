// SPDX-License-Identifier: GPL-3.0-or-later
// Tools > Warble test. There is no warbly capture from this chain to tune
// on, so UinDub brings its own: any clean clip, damaged with the same model
// as the synthetic tests (jitter, a crawling wobble, a top hook and a field
// offset), written as an FFV1 copy whose project carries the true offsets.
// Analyze the copy and UinDub scores itself against them.
#pragma once

#include "Damage.h"
#include "OffsetStore.h"
#include "Project.h"
#include "Source.h"

#include <functional>
#include <string>
#include <vector>

namespace ud {

struct WarbleJob {
    SourceInfo clean;
    FieldOrder order = FieldOrder::TFF;
    DamageModel damage;
    int64_t start = 0, frames = -1;  // the stretch to copy; -1 to the end
    std::string outPath;             // the damaged .mkv
    std::string projectFolder;       // its project, with the truth inside
};

struct WarbleResult {
    bool ok = false;
    std::string error;
    Project project;  // saved, ready to open
};

WarbleResult makeWarbleCopy(const WarbleJob& job, const std::function<bool(double)>& progress = {});

// How close Analyze came to the truth.
struct WarbleScore {
    bool ok = false;
    double rmsMeasured = 0;  // px, over lines with a measurement
    double rmsAll = 0;       // px, every line (interpolated ones included)
    double bias = 0;         // the clip constant taken out first
    int measured = 0, lines = 0;
    std::vector<double> perLine;   // RMS per frame line, measured lines only
    std::vector<double> perField;  // RMS per temporal field
};
WarbleScore scoreWarble(const OffsetStore& store, const DamageModel& truth, FieldOrder order, int64_t frames);

// The true offset of one frame line, for drawing next to the measured one.
double warbleTruth(const DamageModel& d, FieldOrder order, int64_t frame, int frameLine);

}  // namespace ud

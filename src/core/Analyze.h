// SPDX-License-Identifier: GPL-3.0-or-later
// Analyze: the first Edge Lock pass. Decodes the source, measures every
// field (in parallel), runs the tracker in order and writes offsets.zst and
// tracks.zst into the project folder. Render reads them back.
#pragma once

#include "EdgeTracker.h"
#include "OffsetStore.h"
#include "Source.h"

#include <functional>
#include <set>
#include <string>

namespace ud {

struct AnalyzeJob {
    SourceInfo source;
    FieldOrder order = FieldOrder::TFF;
    EdgeLockSettings settings;
    std::vector<Reference> refs;
    int64_t start = 0, end = -1;  // frames to (re-)analyze, end exclusive; -1 the whole clip
    std::string offsetsPath, tracksPath;
    std::set<int64_t> resetFrames;  // empty: taken from the source's events
    int threads = 0;
};

struct AnalyzeOutcome {
    bool ok = false;
    bool cancelled = false;
    std::string error;
    TrackerResult tracker;
    int64_t frames = 0;
    double seconds = 0;
};

using AnalyzeProgress = std::function<bool(double fraction, int64_t frame)>;  // false cancels

AnalyzeOutcome runAnalyze(const AnalyzeJob& job, const AnalyzeProgress& progress = {});

// Frames where tracking resets: signal loss and inserted frames from PingDub's log.
std::set<int64_t> resetFramesOf(const SourceInfo& s);

}  // namespace ud

// SPDX-License-Identifier: GPL-3.0-or-later
// Render: every kept range of the source through Edge Lock, the
// deinterlacer, color, crop and the encoder, with the audio cut to match,
// chapters and captions moved, and a .uindub.json sidecar written next to
// the output so a render can be redone exactly.
#pragma once

#include "Project.h"
#include "Source.h"

#include <functional>
#include <string>
#include <vector>

namespace ud {

struct RenderJob {
    Project project;  // a snapshot; the GUI can keep editing
    SourceInfo source;
    std::string outPath;  // "" for the project's output setting
    bool verifySource = false;
    int threads = 0;
};

struct RenderProgress {
    std::string stage;
    double fraction = 0;
    int64_t frames = 0, total = 0;
    double fps = 0;
};

struct RenderOutcome {
    bool ok = false;
    bool cancelled = false;
    std::string error;
    std::string outPath, sidecar;
    int64_t videoFrames = 0, audioSamples = 0;
    double seconds = 0;
    std::vector<std::string> notes;
};

using RenderCallback = std::function<bool(const RenderProgress&)>;  // false cancels

RenderOutcome runRender(const RenderJob& job, const RenderCallback& progress = {});

// The frames a render will produce for these settings, for the progress bar.
int64_t expectedOutputFrames(const Project& p, int64_t keptFrames);

}  // namespace ud

// SPDX-License-Identifier: GPL-3.0-or-later
// The damage model shared by the synthetic tests and Tools > Warble test:
// per-line jitter, a crawling sine wobble, a top hook and a field offset,
// each with its own strength. Deterministic for a seed, so a damaged copy
// always carries its exact answer.
#pragma once

#include <cstdint>

namespace ud {

struct DamageModel {
    double jitter = 3.0;        // +- px, new every line of every field
    double wobble = 4.0;        // +- px amplitude
    double wobblePeriod = 60;   // frame lines
    double wobbleCrawl = 0.12;  // radians per field the wobble moves
    double hook = 15.0;         // px at the top line
    double hookLines = 30;      // frame lines it takes to fade out
    double fieldOffset = 1.5;   // px the second field of each frame sits off the first
    uint32_t seed = 1;

    // Offset of a frame line in a temporal field: the picture there sits this
    // many pixels right of where it belongs.
    double offset(int64_t field, int frameLine) const;
    static DamageModel none() {
        DamageModel d;
        d.jitter = d.wobble = d.hook = d.fieldOffset = 0;
        return d;
    }
};

}  // namespace ud

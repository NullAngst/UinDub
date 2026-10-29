// SPDX-License-Identifier: GPL-3.0-or-later
// The Edge Lock tracker: takes fields in temporal order and hands back
// finished per-line offsets, a few fields behind, since real-motion
// smoothing looks ahead.
//
// prepare() is const and thread safe (gradients, histogram, border
// measurements), so Analyze runs it for many fields at once; push() runs in
// order (drawn edges track motion field to field).
#pragma once

#include "EdgeLock.h"

#include <array>
#include <deque>
#include <functional>
#include <memory>
#include <set>

namespace ud {

struct TrackerConfig {
    EdgeLockSettings s;
    std::vector<Reference> refs;  // at most kMaxReferences, slot = index
    int width = 0, frameHeight = 0;
    FieldOrder order = FieldOrder::TFF;
    std::set<int64_t> resetFrames;  // signal loss from PingDub's log: tracking resets here
};

struct PreparedField {
    int64_t field = 0;
    int parity = 0;
    FieldLuma luma;
    std::vector<float> grad;
    std::array<float, 64> hist{};
    // Border references measured ahead, per slot, per field line.
    std::array<std::vector<LineMeasure>, kMaxReferences> border;
    std::array<bool, kMaxReferences> borderDone{};
};

struct TrackerResult {
    std::vector<int64_t> cuts;                        // frames
    std::vector<std::pair<int, int64_t>> autoEnds;    // reference slot, frame its span ended at a cut
};

class EdgeTracker {
public:
    explicit EdgeTracker(const TrackerConfig& c);
    ~EdgeTracker();

    PreparedField prepare(int64_t field, FieldLuma&& luma) const;
    void push(PreparedField&& f);
    // Flushes the fields still waiting for look-ahead.
    void finish();

    std::function<void(FieldOffsets&&)> onField;
    const TrackerResult& result() const { return m_result; }
    const TrackerConfig& config() const { return m_c; }

    // Measures one reference on one field with no tracking state, for the
    // overlay: positions where it measured, NaN elsewhere.
    static void measureForOverlay(const TrackerConfig& c, int slot, const RefSpan& span, const FieldLuma& luma, double motionA,
                                  double motionB, std::vector<double>& pos, std::vector<double>& conf);

private:
    void finalizeOne();
    struct Impl;
    TrackerConfig m_c;
    std::unique_ptr<Impl> m_i;
    TrackerResult m_result;
};

// Whittaker smoother (a discrete smoothing spline), second differences.
void whittaker(const std::vector<double>& v, const std::vector<double>& w, double lambda, std::vector<double>& z);

}  // namespace ud

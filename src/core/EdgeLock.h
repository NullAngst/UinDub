// SPDX-License-Identifier: GPL-3.0-or-later
// Edge Lock, the software TBC.
//
// Every line of every field gets one horizontal offset, measured against a
// reference edge and undone with sub-pixel resampling. Measuring:
//   1. each field on its own, since its lines were scanned at their own time
//   2. a template per reference: the luma gradient across the edge, 24 px
//   3. per line, normalized cross-correlation with the template over +-12 px
//      (+-24 in the top hook zone), on the gradient so fades do not move it
//   4. parabola through the peak for sub-pixel position
//   5. confidence from peak height, ambiguity and contrast; below the
//      threshold a line has no measurement, never a guess
//   6. model: measured = edge shape + real motion + timebase error. The
//      border cannot move; a drawn edge moves by a translation and small
//      tilt, fitted robustly and smoothed over time with a look-ahead window
//   7. gaps filled from a smoothing spline, eased to zero past the max gap
//   8. references combined per line by confidence, the border first
//
// Values: offsets are the timebase error e in pixels. A line whose picture
// sits e pixels right of where it belongs is corrected by sampling at x + e.
#pragma once

#include "Picture.h"
#include "Types.h"

#include <cstdint>
#include <string>
#include <vector>

namespace ud {

struct EdgeLockSettings {
    int searchRange = 12;         // px
    int topLines = 40;            // frame lines in the top hook zone
    int topRange = 24;            // px search there
    int templateWidth = 24;       // px
    double confidence = 0.6;      // below this a line is not measured
    int maxGap = 24;              // lines filled by interpolation
    int motionSmoothing = 15;     // fields, look-ahead window for real motion
    double strength = 1.0;        // scales the correction at render
    bool fillRepeat = false;      // false: black (16); true: repeat the edge pixel
    bool lengthCorrection = false;
    bool operator==(const EdgeLockSettings& o) const;
};

enum class RefKind { LeftBorder, RightBorder, Drawn };

struct PointF {
    double x = 0, y = 0;  // frame pixels, frame lines
};

// One stretch of the timeline a reference covers. A drawn reference drawn
// again at a later frame gets a new span with its own shape and template.
struct RefSpan {
    int64_t start = 0, end = 0;   // frames, end exclusive
    std::vector<PointF> points;   // border: one point, x is the edge
    std::vector<float> templ;     // gradient profile across the edge
    double templStep = 0;         // luma step across the edge in the template
    int64_t defFrame = 0;         // frame it was defined on
};

struct Reference {
    int id = 0;
    RefKind kind = RefKind::Drawn;
    std::string name;
    bool enabled = true;
    std::vector<RefSpan> spans;
    bool isBorder() const { return kind != RefKind::Drawn; }
    const RefSpan* spanAt(int64_t frame) const;
};

constexpr int kMaxReferences = 4;

// Hand overrides on the timeline.
struct EdgeOverride {
    enum class Kind { DontCorrect, Reanalyze };
    Kind kind = Kind::DontCorrect;
    int64_t start = 0, end = 0;  // frames, end exclusive
    EdgeLockSettings settings;   // Reanalyze only
};

// --- measurement -------------------------------------------------------------

struct LineMeasure {
    double pos = 0;      // measured edge position, px
    double conf = 0;     // 0..1
    double peak = 0;     // NCC at the peak
    double step = 0;     // luma step across the edge here
    bool contrast = false;  // enough contrast to measure at all
    double level = 0;       // mean luma around the match
    bool flat = false;      // no texture around the match
    bool clamped = false;   // peak at the end of the search range
};

struct EdgeTemplate {
    std::vector<float> t;  // zero mean, unit norm
    double step = 0;
    int width() const { return int(t.size()); }
    bool valid() const { return t.size() >= 8; }
};

// Template from raw profile values (as stored in a RefSpan).
EdgeTemplate makeTemplate(const std::vector<float>& profile, double step);

// Gradient of a luma row: (y[x+1] - y[x-1]) / 2.
void gradientRow(const float* y, int w, float* g);

// One line: search around `center` +- range.
LineMeasure measureLine(const float* luma, const float* grad, int w, const EdgeTemplate& t, double center, int range,
                        bool preferCenter = false);

// Raw profile across an edge at `center` (linear interpolation), `width` taps.
void sampleProfile(const float* grad, int w, double center, int width, float* out);
double stepAt(const float* luma, int w, double pos);

// Builds a template from a frame, refining it against itself twice so line
// jitter does not blur it. `xs` gives the expected edge per frame line
// (NaN where the edge is not covered). Returns the refined per-line
// positions in `found` and the template's raw profile.
struct TemplateBuild {
    std::vector<float> profile;
    double step = 0;
    std::vector<double> found;  // per frame line, NaN when not confident
    double medianOffset = 0;    // found - expected, median
    bool ok = false;
};
TemplateBuild buildTemplate(const Picture& frame, const std::vector<double>& xs, int width, int range, double minConf);

// Finds the left blanking edge (and the right one when the capture has it).
struct BorderFind {
    bool left = false, right = false;
    double leftX = 0, rightX = 0;
    TemplateBuild leftT, rightT;
};
BorderFind findBorders(const Picture& frame, const EdgeLockSettings& s);

// Expected edge x on a frame line for a span, NaN when outside it.
double spanX(const RefSpan& s, RefKind kind, double frameLine);
// Lines (frame) the span covers: [first, last].
void spanLines(const RefSpan& s, RefKind kind, int frameHeight, int& first, int& last);

// Reference made from a drawn polyline on `frame` (shape refined to the edge).
bool makeDrawnSpan(const Picture& frame, const std::vector<PointF>& pts, int64_t frameIndex, int64_t end, const EdgeLockSettings& s,
                   RefSpan& out, std::string& err);
bool makeBorderSpan(const Picture& frame, bool right, int64_t frames, const EdgeLockSettings& s, RefSpan& out);

// --- per-field result ------------------------------------------------------------

// Line states stored next to each offset (the 8-bit confidence byte).
enum : uint8_t {
    kConfNone = 0,          // no correction
    kConfInterpolated = 1,  // filled from neighbors
    kConfEased = 2,         // held and easing to zero
    kConfMeasuredMin = 3,   // 3..255: measured, scaled confidence
};

// Per reference, per field, what the tracker saw.
enum class RefState : uint8_t { Inactive, Active, Lost, Dark, Ended };

struct RefFieldTrack {
    RefState state = RefState::Inactive;
    uint8_t measured = 0;  // share of covered lines measured, 0..255
    float a = 0, b = 0;    // real motion: px, px per frame line (drawn only)
};

struct FieldTrack {
    uint8_t flags = 0;      // bit 0 cut, bit 1 dark, bit 2 measured anything, bit 3 signal loss reset
    float meanAbs = 0;      // average |offset| over lines
    float maxPos = 0, maxNeg = 0;
    RefFieldTrack refs[kMaxReferences];
};

enum : uint8_t { kFieldCut = 1, kFieldDark = 2, kFieldMeasured = 4, kFieldReset = 8 };

// A finished field: offsets for its lines (field lines, not frame lines).
struct FieldOffsets {
    int64_t field = 0;          // temporal field index (2 * frame + order)
    int parity = 0;
    std::vector<float> off;     // px
    std::vector<uint8_t> conf;  // kConf* or measured confidence
    std::vector<float> slope;   // length correction, empty when off
    FieldTrack track;
};

}  // namespace ud

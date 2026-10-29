// SPDX-License-Identifier: GPL-3.0-or-later
// Synthetic pictures with a known answer (tests, --self-test, Tools > Warble test): a blanking border, vertical
// bars with soft (band-limited) edges, optional pan, fade, occluder and cut,
// rendered analytically so a sub-pixel shift is exact.
#pragma once

#include "Damage.h"
#include "EdgeLock.h"
#include "EdgeTracker.h"
#include "Picture.h"
#include "Source.h"
#include "shared/Json.h"

#include <functional>
#include <string>
#include <vector>

namespace ud {

struct SynthScene {
    int width = 720, height = 486;
    double borderL = 12.5, borderR = 706.5;
    bool rightBorder = true;
    double sigma = 1.2;  // edge softness, px
    double black = 16, base = 110, bar = 185;
    std::vector<std::pair<double, double>> bars = {{150, 40}, {300, 40}, {450, 40}, {580, 30}};
    // A bar whose left edge pans, for real motion.
    bool panBar = false;
    double panX = 360, panW = 60, panPerField = 0;
    // Picture gain over black for the fade test, per field.
    std::function<double(int64_t field)> fade;
    // Occluder box sweeping across, per field: x0, x1 (frame px), lines y0..y1.
    std::function<bool(int64_t field, double& x0, double& x1)> box;
    double boxY0 = 60, boxY1 = 420, boxLevel = 60, boxTexture = 0;
    // A different picture from this frame on (a hard cut).
    int64_t cutFrame = -1;
    double noise = 0;  // Gaussian luma noise, codes
    uint32_t seed = 7;
    // 3:2 pulldown of 24p film: film frame i has the pan bar at panX + panPerFilm * i.
    bool telecine = false;
    int telecineOffset = 0;  // where in the 5-frame cycle the clip starts
    double panPerFilm = 8;
};

// Film frame shown in the top (parity 0) or bottom field of video frame n.
int64_t telecineFilmFrame(const SynthScene& s, int64_t n, int parity);

// Undamaged luma at x on a frame line in a temporal field.
double sceneLuma(const SynthScene& s, int64_t field, int frameLine, double x);
// One field with damage applied (content shifted by the model's offset).
void renderFieldLuma(const SynthScene& s, const DamageModel& d, int64_t field, int parity, FieldLuma& out);
// A whole frame, each field at its own temporal index.
void renderFrame(const SynthScene& s, const DamageModel& d, int64_t frame, FieldOrder order, Picture& out);

// Runs a tracker over `frames` frames and collects the per-field results.
struct SynthRun {
    std::vector<FieldOffsets> fields;
    TrackerResult result;
};
SynthRun runTracker(const SynthScene& s, const DamageModel& d, const TrackerConfig& cfg, int64_t frames);

// Residual of measured offsets against the true damage, RMS over lines that
// were measured, and over every line.
struct Residual {
    double rmsMeasured = 0, rmsAll = 0, maxAbs = 0, bias = 0;
    int measured = 0, lines = 0;
};
Residual residualOf(const SynthRun& r, const DamageModel& d, int64_t fromField = 0, int64_t toField = -1);

// Default config: a left border reference from frame 0.
TrackerConfig borderConfig(const SynthScene& s, const DamageModel& d, int64_t frames, bool right = false);

// Writes a synthetic interlaced capture to `path` in the given codec, with
// PingDub-style sidecars when `pingdub` is set. Returns false on failure.
struct SynthFile {
    std::string path;
    std::string codec = "ffv1";       // ffv1, utvideo, ffvhuff, huffyuv, rawvideo, mjpeg
    int pixfmt = -1;                  // AVPixelFormat, -1: the codec's natural 4:2:2
    std::string container = "mkv";    // mkv or avi
    int64_t frames = 30;
    bool audio = true;
    int sampleRate = 48000;
    int channels = 2;
    bool pingdub = true;
    std::string driver = "gvusb2";
    std::string captureStart = "2026-10-29T10:00:00";
    FieldOrder tag = FieldOrder::TFF;     // what the container says
    FieldOrder truth = FieldOrder::TFF;   // how the motion is really laid out
    int segments = 1;
    bool captions = false;
    bool chapters = false;
    bool events = false;
    bool pal = false;
    bool moving = true;  // horizontal motion so the comb test has something
};
bool writeSynthCapture(const SynthFile& f, const SynthScene& scene, const DamageModel& d, std::string& err);

std::string tempDir(const std::string& name);

// Runs ffprobe (UD_FFPROBE, else the one on PATH) with JSON output.
bool ffprobeJson(const std::string& path, const std::string& args, JsonValue& out, std::string& err);

}  // namespace ud

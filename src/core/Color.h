// SPDX-License-Identifier: GPL-3.0-or-later
// Color and levels, in the float working buffer after deinterlacing, then
// legalized back to BT.601 limited range (16-235 luma, 16-240 chroma).
// At neutral settings the stage is skipped entirely.
#pragma once

#include "Picture.h"

#include <array>
#include <cstdint>
#include <map>
#include <string>

namespace ud {

struct ColorSettings {
    double brightness = 0;     // -50..50 codes on luma
    double contrast = 1;       // 0.5..2 gain around code 16
    double gamma = 1;          // 0.5..2 on luma, black and white held
    double blackPoint = 16;    // picked codes mapped to 16 and 235
    double whitePoint = 235;
    double saturation = 1;     // 0..2
    double hue = 0;            // -30..30 degrees
    std::array<double, 3> lift{0, 0, 0};   // per R, G, B: -0.25..0.25
    std::array<double, 3> gammaRgb{1, 1, 1};  // 0.5..2
    std::array<double, 3> gain{1, 1, 1};   // 0.5..2
    double chromaDelayX = 0;   // -8..8 px, positive moves chroma right
    double chromaDelayY = 0;   // -2..2 lines
    double chromaBlur = 0;     // 0..3 px
    bool neutral() const;
    bool operator==(const ColorSettings& o) const;
};

struct ColorStats {
    double clipped = 0;  // share of pixels pushed past legal range by the settings
};

// Applies everything, then legalizes. `stats` (optional) gets the clip share,
// measured before legalizing.
void applyColor(Picture& pic, const ColorSettings& c, ColorStats* stats = nullptr);
void legalize(Picture& pic);

// Auto levels: 0.1st and 99.9th luma percentiles over the active picture
// (borders and the bottom 8 head-switching lines left out).
struct LevelHistogram {
    std::array<uint64_t, 256> luma{};
    uint64_t total = 0;
    void add(const Picture& pic, int border = 16, int bottom = 8);
    bool suggest(double& black, double& white) const;
};

// Per chapter color: chapter index -> settings. Missing: the clip's own.
using ChapterColors = std::map<int, ColorSettings>;

}  // namespace ud

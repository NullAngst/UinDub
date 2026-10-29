// SPDX-License-Identifier: GPL-3.0-or-later
// The small types PingDub's scopes and histogram take, without the rest of
// PingDub's frame analyzer.
#pragma once

#include <cstdint>
#include <vector>

namespace ud {

struct ActiveArea {
    int x0 = 0, x1 = 0;  // columns [x0, x1)
    int y0 = 0, y1 = 0;  // rows [y0, y1)
    int width() const { return x1 - x0; }
    int height() const { return y1 - y0; }
};

// Downsampled 8-bit planes for the waveform and vectorscope.
struct ScopeFrame {
    int width = 0, height = 0;    // luma
    int cwidth = 0, cheight = 0;  // chroma, 0 for a gray source
    std::vector<uint8_t> y, cb, cr;
    ActiveArea area;
};

// 8-bit luma plane for the histogram.
struct LumaPlane {
    std::vector<uint8_t> y;
    int width = 0, height = 0;
};

constexpr int kDefaultChromaLimit = 112;

}  // namespace ud

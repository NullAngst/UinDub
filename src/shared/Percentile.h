// SPDX-License-Identifier: GPL-3.0-or-later
// Copied from PingDub (github.com/NullAngst/PingDub, same author, GPL-3.0-or-later):
// LevelAccumulator's percentile and clip-pile reads, used for Auto levels.
#pragma once

#include <array>
#include <cmath>
#include <cstdint>

namespace ud {

// Smallest value whose cumulative count reaches `fraction` of the total.
inline int percentile(const std::array<uint64_t, 256>& h, uint64_t total, double fraction) {
    if (!total) return -1;
    const uint64_t want = uint64_t(std::ceil(fraction * double(total)));
    const uint64_t need = want > 0 ? want : 1;
    uint64_t cum = 0;
    for (int v = 0; v < 256; ++v) {
        cum += h[size_t(v)];
        if (cum >= need) return v;
    }
    return 255;
}

// A spike at the lowest code with picture continuing right above it: the
// dark end of the picture was cut off there. An isolated flat black (color
// bars) has nothing right above it and is not a clip.
inline bool pileLow(const std::array<uint64_t, 256>& h, uint64_t total) {
    if (!total) return false;
    int lo = 0;
    while (lo < 252 && !h[size_t(lo)]) ++lo;
    const double pile = double(h[size_t(lo)]);
    const double above = double(h[size_t(lo + 1)] + h[size_t(lo + 2)] + h[size_t(lo + 3)]);
    return pile >= 0.002 * double(total) && pile >= 4.0 * double(h[size_t(lo + 1)]) && above >= 0.0005 * double(total);
}

inline bool pileHigh(const std::array<uint64_t, 256>& h, uint64_t total) {
    std::array<uint64_t, 256> r;
    for (size_t i = 0; i < 256; ++i) r[i] = h[255 - i];
    return pileLow(r, total);
}

}  // namespace ud

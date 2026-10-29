// SPDX-License-Identifier: GPL-3.0-or-later
#include "Damage.h"

#include <cmath>

namespace ud {

static uint64_t mix(uint64_t x) {
    x ^= x >> 33;
    x *= 0xff51afd7ed558ccdULL;
    x ^= x >> 33;
    x *= 0xc4ceb9fe1a85ec53ULL;
    x ^= x >> 33;
    return x;
}

double DamageModel::offset(int64_t field, int y) const {
    double d = 0;
    if (jitter != 0) {
        const uint64_t h = mix(uint64_t(seed) * 0x9E3779B97F4A7C15ULL ^ mix(uint64_t(field) * 1315423911ULL + uint64_t(y)));
        const double u = double(h >> 11) / double(1ULL << 53);  // [0, 1)
        d += jitter * (2.0 * u - 1.0);
    }
    if (wobble != 0 && wobblePeriod > 0) d += wobble * std::sin(2.0 * M_PI * double(y) / wobblePeriod + wobbleCrawl * double(field));
    if (hook != 0 && hookLines > 1 && y < hookLines) {
        const double t = std::max(0.0, double(y - 1)) / (hookLines - 1.0);
        const double c = std::cos(0.5 * M_PI * std::min(1.0, t));
        d += hook * c * c;
    }
    if (fieldOffset != 0 && (field & 1)) d += fieldOffset;
    return d;
}

}  // namespace ud

// SPDX-License-Identifier: GPL-3.0-or-later
// Basic types shared by every stage.
#pragma once

#include <cstdint>
#include <string>

namespace ud {

struct Rational {
    int num = 0;
    int den = 1;
    bool valid() const { return num > 0 && den > 0; }
    double toDouble() const { return den ? double(num) / double(den) : 0.0; }
    bool operator==(const Rational& o) const { return int64_t(num) * o.den == int64_t(o.num) * den; }
    bool operator!=(const Rational& o) const { return !(*this == o); }
};

enum class FieldOrder { Unknown, Progressive, TFF, BFF };

const char* fieldOrderName(FieldOrder f);
FieldOrder fieldOrderFromName(const std::string& s);

// Which field comes first in time decides the parity of temporal field 0.
// Parity 0 is the top field (frame lines 0, 2, 4, ...).
inline int firstParity(FieldOrder f) { return f == FieldOrder::BFF ? 1 : 0; }

enum class ColorStd { Smpte170m, Bt470bg };

// "HH:MM:SS.mmm"
std::string clockString(double seconds);
// The time of frame n at this rate.
inline double frameSeconds(int64_t n, const Rational& fps) { return double(n) * fps.den / double(fps.num); }

}  // namespace ud

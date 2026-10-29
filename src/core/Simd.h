// SPDX-License-Identifier: GPL-3.0-or-later
// The tight per-line loops, with AVX2 versions picked at run time and plain
// C++ for every other x86-64 CPU.
#pragma once

#include <cstdint>

namespace ud {

bool cpuHasAvx2();

// Lanczos-3 weights for a sample point `t` past an integer position, taps -2..3.
void lanczos3Weights(double t, float w[6]);

// dst[x] = src at x + shift, Lanczos-3. Outside the row: `fill`, or the edge
// pixel when `repeat` is set.
void shiftRow(const float* src, float* dst, int w, double shift, float fill, bool repeat);

// dst[x] = src at x + off + (x - xref) * slope, for the line length correction.
void warpRow(const float* src, float* dst, int w, double off, double slope, double xref, float fill, bool repeat);

// sum(a[i] * b[i]) for n values.
float dot(const float* a, const float* b, int n);

// Forces the plain C++ kernels (tests compare the two).
void setSimdEnabled(bool on);
bool simdEnabled();

}  // namespace ud

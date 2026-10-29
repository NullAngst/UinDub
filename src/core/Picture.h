// SPDX-License-Identifier: GPL-3.0-or-later
// The working buffer: 32-bit float planar 4:4:4, values on the 8-bit code
// scale (16 is black, 235 white, 128 neutral chroma) whatever the source
// depth, so one set of numbers works for 8 and 10-bit files.
//
// Chroma is brought to full resolution on the way in, so it moves with luma
// at sub-pixel precision, and is subsampled again only on the way out.
#pragma once

#include "Av.h"

#include <cstdint>
#include <vector>

namespace ud {

struct Plane {
    int w = 0, h = 0;
    std::vector<float> d;
    void resize(int width, int height) {
        w = width;
        h = height;
        d.assign(size_t(w) * size_t(h), 0.0f);
    }
    float* row(int y) { return d.data() + size_t(y) * size_t(w); }
    const float* row(int y) const { return d.data() + size_t(y) * size_t(w); }
};

struct Picture {
    int width = 0, height = 0;
    bool gray = false;
    Plane p[3];  // Y, Cb, Cr
    void alloc(int w, int h, bool isGray);
    int planes() const { return gray ? 1 : 3; }
    bool empty() const { return width <= 0 || height <= 0; }
};

// AVFrame (any planar or packed YUV or gray format FFmpeg decodes) to the
// working buffer. `interlaced` keeps 4:2:0 chroma rows with their own field.
bool toPicture(const AVFrame* f, Picture& out, bool interlaced);

// Working buffer to a new AVFrame in `fmt`. Values are rounded and clipped
// to the format's range only (legalizing is the color stage's job).
// `interlaced` subsamples 4:2:0 chroma within each field.
FramePtr fromPicture(const Picture& pic, AVPixelFormat fmt, bool interlaced);

// Fields. Parity 0 is the top field (even frame lines).
void splitField(const Picture& frame, int parity, Picture& field);
void weaveField(const Picture& field, int parity, Picture& frame);

// Luma of one field straight from a decoded frame, for analysis.
struct FieldLuma {
    int width = 0, lines = 0, parity = 0;
    std::vector<float> y;
    const float* row(int l) const { return y.data() + size_t(l) * size_t(width); }
    float* row(int l) { return y.data() + size_t(l) * size_t(width); }
};
bool fieldLuma(const AVFrame* f, int parity, FieldLuma& out);
void fieldLumaOf(const Picture& frame, int parity, FieldLuma& out);

}  // namespace ud

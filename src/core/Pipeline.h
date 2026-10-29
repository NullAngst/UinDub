// SPDX-License-Identifier: GPL-3.0-or-later
// The stages shared by the render and the preview. Order is fixed:
//   decode, split fields, Edge Lock, deinterlace, color, crop, encode.
// Edge Lock runs on fields before anything mixes lines; color after it, so
// adjustments never change what the tracker measures; crop after
// deinterlacing, since an odd crop of interlaced lines swaps the fields.
#pragma once

#include "OffsetStore.h"
#include "Project.h"
#include "Source.h"

#include <string>

namespace ud {

void cropPicture(Picture& p, int left, int right, int top, int bottom);
// Horizontal Lanczos-3 resample to `width` (square pixels).
void resizeWidth(Picture& p, int width);

struct OutputGeometry {
    int cropLeft = 0, cropRight = 0, cropTop = 0, cropBottom = 0;
    int width = 0, height = 0;  // final, after crop and resize
    int croppedWidth = 0;       // before the resize
    Rational sar{1, 1};
    bool resize = false;
    std::vector<std::string> notes;  // what got snapped and why
};
// Crop snapped to what the output can hold: even width and height for 4:2:0,
// even width for 4:2:2, even top and bottom when the output stays interlaced.
OutputGeometry outputGeometry(const Project& p, const SourceInfo& s, AVPixelFormat encoderFormat);

// Decode, then Edge Lock (when it is on and analyzed), into the working buffer.
class InputStage {
public:
    bool open(const Project& p, const SourceInfo& s, std::string& err);
    bool edgeActive() const { return m_edge; }
    // The frame, interlaced, corrected. `corrected` tells whether Edge Lock
    // touched it (it is skipped entirely for frames with nothing to correct).
    bool frame(SourceReader& r, int64_t n, Picture& out, bool* corrected = nullptr);
    // The same from an already decoded frame.
    bool fromDecoded(const AVFrame* f, int64_t n, Picture& out, bool* corrected = nullptr);
    const OffsetStore& store() const { return m_store; }
    bool dontCorrect(int64_t frame) const;

private:
    bool m_edge = false;
    OffsetStore m_store;
    ApplyParams m_params;
    std::vector<EdgeOverride> m_off;
    bool m_interlaced = true;
};

// Color (per chapter), crop and resize on a progressive (or kept
// interlaced) frame.
void postProcess(Picture& pic, const Project& p, const SourceInfo& s, const OutputGeometry& g, int64_t sourceFrame,
                 ColorStats* stats = nullptr);

}  // namespace ud

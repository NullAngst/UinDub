// SPDX-License-Identifier: GPL-3.0-or-later
// QTGMC through the bundled VapourSynth, inside this process.
//
// The VapourSynth script library (VSScript) embeds the bundled Python. Our
// own source filter feeds it Edge Lock's fields in memory; the QTGMC script
// is evaluated from a string. No temp files, no scripts written to disk.
//
// Finding the stack: the AppImage carries it under usr/lib/uindub/python
// (Python, VapourSynth, its plugins and the scripts). A development build
// can point UINDUB_PYTHON at any Python that has VapourSynth installed.
#pragma once

#include "Av.h"
#include "Types.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace ud {

// The presets QTGMC itself has, slowest first.
const std::vector<std::string>& qtgmcPresets();

struct QtgmcConfig {
    std::string preset = "Slower";
    bool doubleRate = true;
    bool tff = true;
    int width = 0, height = 0;
    bool gray = false;
    Rational fps{30000, 1001};
    int sourceMatch = 0;     // 0 off, 1-3
    double sharpness = -1;   // -1: the preset's own
    int threads = 0;         // 0: VapourSynth's default
};

// The VapourSynth runtime, found and started once per process.
struct VsRuntimeInfo {
    bool ok = false;
    std::string error;
    std::string python, pythonLib, vsscript, version;
};
const VsRuntimeInfo& vapoursynthRuntime();

// Frames in: yuv444p16 (or gray16) interlaced frames, by index within the
// range being deinterlaced. Called from VapourSynth's threads, one at a time.
using QtgmcSource = std::function<FramePtr(int64_t index)>;

class QtgmcRunner {
public:
    QtgmcRunner();
    ~QtgmcRunner();
    QtgmcRunner(const QtgmcRunner&) = delete;
    QtgmcRunner& operator=(const QtgmcRunner&) = delete;

    bool open(const QtgmcConfig& c, int64_t inputFrames, QtgmcSource source, std::string& err);
    int64_t outputFrames() const;
    // Output frame k, progressive yuv444p16 (or gray16). Asking in order keeps
    // a window of frames in flight so every core works.
    FramePtr get(int64_t k, std::string& err);
    void close();

private:
    struct Impl;
    std::unique_ptr<Impl> m_i;
};

}  // namespace ud

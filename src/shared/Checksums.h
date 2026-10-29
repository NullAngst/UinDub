// SPDX-License-Identifier: GPL-3.0-or-later
// Copied from PingDub (github.com/NullAngst/PingDub, same author, GPL-3.0-or-later).
// Checksums at two levels, and checking a file against them later.
//
// Per frame: an MD5 of each frame's raw picture data before encoding, in a
// .framemd5 sidecar laid out like `ffmpeg -i file -map 0:v:0 -f framemd5 -`.
// Every codec PingDub writes is lossless, so decoding the file reproduces
// every hash.
//
// Per file: a SHA-256 of the finished file, in a .sha256 sidecar that
// `sha256sum -c` reads.
#pragma once

#include "core/Types.h"

#include <cstdint>
#include <cstdio>
#include <functional>
#include <string>
#include <vector>

struct AVFrame;
struct AVSHA;

namespace ud {

std::string toHex(const uint8_t* data, size_t n);

// MD5 of the picture planes in the order and row width av_image_copy_to_buffer
// uses with align 1. `size` gets the byte count hashed.
bool frameMd5(const AVFrame* f, uint8_t out[16], int& size);

// "<folder>/<stem>.sha256" for "<folder>/<stem>.mkv", likewise .framemd5.
std::string sidecarFor(const std::string& mediaPath, const std::string& ext);

struct FrameMd5Header {
    Rational tb{1001, 30000};    // one frame
    std::string codec = "rawvideo";
    int width = 0, height = 0;
    Rational sar{0, 1};
};

class FrameMd5Writer {
public:
    ~FrameMd5Writer();
    bool open(const std::string& path, const FrameMd5Header& h);
    void add(int64_t pts, int size, const uint8_t md5[16]);
    bool close();
    bool isOpen() const { return m_f != nullptr; }
    uint64_t count() const { return m_count; }
    std::string path() const { return m_path; }

private:
    FILE* m_f = nullptr;
    std::string m_path;
    uint64_t m_count = 0;
    bool m_failed = false;
};

struct FrameMd5Entry {
    int64_t pts = 0;
    int64_t size = 0;
    std::string hash;
};
bool readFrameMd5(const std::string& path, std::vector<FrameMd5Entry>& out, std::string& err);

// SHA-256. Uses the CPU's SHA instructions when it has them (Ryzen from the
// first generation, Intel from Ice Lake and Goldmont), about four times the
// speed of FFmpeg's plain C version, which is the fallback.
class Sha256 {
public:
    Sha256();
    ~Sha256();
    Sha256(const Sha256&) = delete;
    Sha256& operator=(const Sha256&) = delete;
    void update(const uint8_t* data, size_t n);
    void final(uint8_t out[32]);
    static bool hardware();

private:
    uint32_t m_state[8];
    uint8_t m_buf[64];
    size_t m_fill = 0;
    uint64_t m_total = 0;
    ::AVSHA* m_soft = nullptr;
};

// SHA-256 of a whole file. progress(done, total) returning false cancels.
bool sha256File(const std::string& path, std::string& hex, std::string& err,
                const std::function<bool(uint64_t, uint64_t)>& progress = {});
// Writes "<hex>  <file name>\n" next to the file. Returns the sidecar path or "".
std::string writeSha256Sidecar(const std::string& mediaPath, const std::string& hex);
std::string readSha256Sidecar(const std::string& sidecar);

struct VerifyResult {
    bool ok = false;           // every check that could run passed
    bool cancelled = false;
    std::string error;         // could not run at all
    // file level
    bool haveSha = false;
    bool shaMatch = false;
    std::string shaExpected, shaActual;
    // frame level (full verify)
    bool full = false;
    bool haveFrameMd5 = false;
    uint64_t framesExpected = 0;
    uint64_t framesChecked = 0;
    std::vector<int64_t> badFrames;      // decoded picture differs from the capture
    std::vector<int64_t> missingFrames;  // in the sidecar, not in the file
    std::vector<int64_t> decodeErrors;   // frames the decoder rejected
    std::vector<int64_t> crcErrors;      // FFV1 slice CRC mismatches
    Rational fps{30000, 1001};
    std::string summary() const;
};

// Quick: SHA-256 only. Full: SHA-256, then decode every frame and compare it
// with the .framemd5 sidecar. progress(fraction 0..1, stage) returning false cancels.
VerifyResult verifyCapture(const std::string& path, bool full,
                           const std::function<bool(double, const std::string&)>& progress = {});

}  // namespace ud

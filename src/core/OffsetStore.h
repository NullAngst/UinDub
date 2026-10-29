// SPDX-License-Identifier: GPL-3.0-or-later
// offsets.zst: what Analyze measured, read back by Render.
//
// One int16 per frame line in 1/64 px plus an 8-bit confidence, so 1458
// bytes per NTSC frame raw, in chunks of 256 frames, each chunk its own zstd
// frame. A zstd skippable frame at the start holds the header and one at the
// end the chunk index, so `zstd -d` still unpacks the file to the plain
// per-frame table and Render can jump to any frame. With length correction
// on, an int16 slope per line follows the confidence bytes.
//
// tracks.zst: per field, the tracker's state for each reference and the
// field's correction summary, for the timeline lanes and the safe crop.
#pragma once

#include "EdgeLock.h"

#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace ud {

struct FrameOffsets {
    std::vector<int16_t> off;    // 1/64 px per frame line
    std::vector<uint8_t> conf;   // kConf* per frame line
    std::vector<int16_t> slope;  // 2^-21 per px, empty when length correction is off
    void reset(int lines, bool withSlope);
    bool zero() const;           // nothing to correct anywhere
    double offsetPx(int y) const { return y < int(off.size()) ? off[size_t(y)] / 64.0 : 0.0; }
    double slopeAt(int y) const { return y < int(slope.size()) ? slope[size_t(y)] / double(1 << 21) : 0.0; }
};

constexpr double kOffsetScale = 64.0;
constexpr double kSlopeScale = double(1 << 21);

struct OffsetHeader {
    int lines = 0;          // frame lines
    int width = 0;
    int64_t frames = 0;
    int chunkFrames = 256;
    bool slope = false;
    float xref = 0;         // where the slope is zero
    int firstParity = 0;    // 0 top field first
    size_t frameBytes() const { return size_t(lines) * (slope ? 5 : 3); }
};

class OffsetStore {
public:
    bool open(const std::string& path, std::string& err);
    bool isOpen() const { return m_open; }
    const OffsetHeader& header() const { return m_h; }
    const std::string& path() const { return m_path; }
    // A frame's offsets. Frames outside the file come back zero.
    bool get(int64_t frame, FrameOffsets& out) const;
    // The compressed chunk as stored, for copying into a rewrite.
    bool rawChunk(int index, std::vector<uint8_t>& out) const;
    int chunks() const { return int(m_index.size()); }

private:
    bool loadChunk(int index, std::vector<uint8_t>& raw) const;

    bool m_open = false;
    std::string m_path;
    OffsetHeader m_h;
    std::vector<std::pair<uint64_t, uint32_t>> m_index;  // offset, size
    mutable std::mutex m_mutex;
    mutable std::map<int, std::vector<uint8_t>> m_cache;  // a few decompressed chunks
    mutable std::vector<int> m_lru;
};

// Writes a new store, optionally on top of an old one: frames in
// [from, to) come from put(), every other frame from `base` (or zero).
class OffsetWriter {
public:
    ~OffsetWriter();
    bool create(const std::string& path, const OffsetHeader& h, const OffsetStore* base, int64_t from, int64_t to, std::string& err);
    // Frames in increasing order, inside [from, to).
    bool put(int64_t frame, const FrameOffsets& fo);
    bool finish(std::string& err);
    void abort();

private:
    bool flushTo(int chunk);
    bool emitChunk(int chunk, const std::vector<uint8_t>* raw);
    bool startChunk(int chunk);

    std::string m_path, m_tmp;
    OffsetHeader m_h;
    const OffsetStore* m_base = nullptr;
    int64_t m_from = 0, m_to = 0;
    FILE* m_f = nullptr;
    uint64_t m_pos = 0;
    std::vector<std::pair<uint64_t, uint32_t>> m_index;
    int m_cur = -1;                 // chunk being built
    std::vector<uint8_t> m_raw;     // its raw bytes
    int m_written = 0;              // chunks written so far
    bool m_failed = false;
};

// Per field tracks for the whole clip, kept in memory (a two-hour tape is a
// few tens of MB).
class TrackStore {
public:
    std::vector<FieldTrack> fields;  // index = temporal field
    int refs = kMaxReferences;
    bool save(const std::string& path, std::string& err) const;
    bool load(const std::string& path, std::string& err);
    // Safe crop in px for left and right at full strength.
    void safeCrop(double strength, int& left, int& right) const;
};

// Applies a frame's offsets to the working buffer: every line resampled by
// its own offset. Lines with nothing to correct are not touched at all.
struct ApplyParams {
    double strength = 1.0;
    bool fillRepeat = false;
    float xref = 0;
};
void applyOffsets(Picture& pic, const FrameOffsets& fo, const ApplyParams& p);

// Frame lines from the two finished fields of a frame.
void fieldsToFrame(const FieldOffsets& a, const FieldOffsets& b, int frameLines, bool slope, FrameOffsets& out);

}  // namespace ud

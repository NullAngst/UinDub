// SPDX-License-Identifier: GPL-3.0-or-later
#include "OffsetStore.h"

#include "Simd.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <zstd.h>

namespace ud {

namespace fs = std::filesystem;

namespace {

constexpr uint32_t kSkipHeader = 0x184D2A50;  // zstd skippable frame magics
constexpr uint32_t kSkipIndex = 0x184D2A5E;
constexpr char kMagic[8] = {'U', 'D', 'O', 'F', 'F', 'S', '1', 0};
constexpr char kTrackMagic[8] = {'U', 'D', 'T', 'R', 'K', 'S', '1', 0};
constexpr int kLevel = 6;

void put32(std::vector<uint8_t>& b, uint32_t v) {
    for (int i = 0; i < 4; ++i) b.push_back(uint8_t(v >> (8 * i)));
}
void put64(std::vector<uint8_t>& b, uint64_t v) {
    for (int i = 0; i < 8; ++i) b.push_back(uint8_t(v >> (8 * i)));
}
void putF(std::vector<uint8_t>& b, float f) {
    uint32_t v;
    std::memcpy(&v, &f, 4);
    put32(b, v);
}
uint32_t get32(const uint8_t* p) { return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24; }
uint64_t get64(const uint8_t* p) { return uint64_t(get32(p)) | uint64_t(get32(p + 4)) << 32; }
float getF(const uint8_t* p) {
    const uint32_t v = get32(p);
    float f;
    std::memcpy(&f, &v, 4);
    return f;
}

bool readAt(FILE* f, uint64_t off, void* dst, size_t n) {
    if (fseeko(f, off_t(off), SEEK_SET) != 0) return false;
    return std::fread(dst, 1, n, f) == n;
}

// Raw frame layout: int16 off[lines], uint8 conf[lines], int16 slope[lines] (optional).
void packFrame(const FrameOffsets& fo, const OffsetHeader& h, uint8_t* dst) {
    const int L = h.lines;
    for (int y = 0; y < L; ++y) {
        const int16_t v = y < int(fo.off.size()) ? fo.off[size_t(y)] : 0;
        dst[2 * y] = uint8_t(uint16_t(v) & 0xFF);
        dst[2 * y + 1] = uint8_t(uint16_t(v) >> 8);
    }
    for (int y = 0; y < L; ++y) dst[2 * L + y] = y < int(fo.conf.size()) ? fo.conf[size_t(y)] : 0;
    if (h.slope)
        for (int y = 0; y < L; ++y) {
            const int16_t v = y < int(fo.slope.size()) ? fo.slope[size_t(y)] : 0;
            dst[3 * L + 2 * y] = uint8_t(uint16_t(v) & 0xFF);
            dst[3 * L + 2 * y + 1] = uint8_t(uint16_t(v) >> 8);
        }
}

void unpackFrame(const uint8_t* src, const OffsetHeader& h, FrameOffsets& fo) {
    const int L = h.lines;
    fo.reset(L, h.slope);
    for (int y = 0; y < L; ++y) fo.off[size_t(y)] = int16_t(uint16_t(src[2 * y]) | uint16_t(src[2 * y + 1]) << 8);
    std::memcpy(fo.conf.data(), src + 2 * L, size_t(L));
    if (h.slope)
        for (int y = 0; y < L; ++y) fo.slope[size_t(y)] = int16_t(uint16_t(src[3 * L + 2 * y]) | uint16_t(src[3 * L + 2 * y + 1]) << 8);
}

}  // namespace

void FrameOffsets::reset(int lines, bool withSlope) {
    off.assign(size_t(lines), 0);
    conf.assign(size_t(lines), kConfNone);
    slope.assign(withSlope ? size_t(lines) : 0, 0);
}

bool FrameOffsets::zero() const {
    for (int16_t v : off)
        if (v) return false;
    for (int16_t v : slope)
        if (v) return false;
    return true;
}

// ---------------------------------------------------------------------------

bool OffsetStore::open(const std::string& path, std::string& err) {
    std::lock_guard<std::mutex> lk(m_mutex);
    m_open = false;
    m_cache.clear();
    m_lru.clear();
    m_index.clear();
    m_path = path;
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) {
        err = "cannot open " + path;
        return false;
    }
    uint8_t hdr[8 + 64];
    const bool okHdr = std::fread(hdr, 1, sizeof hdr, f) == sizeof hdr;
    if (!okHdr || get32(hdr) != kSkipHeader || get32(hdr + 4) != 64 || std::memcmp(hdr + 8, kMagic, 8) != 0) {
        std::fclose(f);
        err = path + " is not an offsets file";
        return false;
    }
    const uint8_t* p = hdr + 16;
    // version at p[0..3]
    m_h.lines = int(get32(p + 4));
    m_h.width = int(get32(p + 8));
    m_h.frames = int64_t(get64(p + 12));
    m_h.chunkFrames = int(get32(p + 20));
    const uint32_t flags = get32(p + 24);
    m_h.slope = flags & 1;
    m_h.xref = getF(p + 28);
    m_h.firstParity = int(get32(p + 32));
    fseeko(f, 0, SEEK_END);
    const uint64_t size = uint64_t(ftello(f));
    uint8_t tail[4];
    if (size < 8 + 64 + 12 || !readAt(f, size - 4, tail, 4)) {
        std::fclose(f);
        err = path + " has no index";
        return false;
    }
    const uint32_t payload = get32(tail);
    std::vector<uint8_t> idx(size_t(payload) + 8);
    if (payload + 8 > size || !readAt(f, size - 8 - payload, idx.data(), idx.size()) || get32(idx.data()) != kSkipIndex) {
        std::fclose(f);
        err = path + " has a broken index";
        return false;
    }
    const uint32_t count = get32(idx.data() + 8);
    if (12 + size_t(count) * 12 + 4 > idx.size()) {
        std::fclose(f);
        err = path + " has a broken index";
        return false;
    }
    for (uint32_t i = 0; i < count; ++i) {
        const uint8_t* e = idx.data() + 12 + size_t(i) * 12;
        m_index.push_back({get64(e), get32(e + 8)});
    }
    std::fclose(f);
    m_open = true;
    return true;
}

bool OffsetStore::rawChunk(int index, std::vector<uint8_t>& out) const {
    if (index < 0 || index >= int(m_index.size())) return false;
    FILE* f = std::fopen(m_path.c_str(), "rb");
    if (!f) return false;
    out.resize(m_index[size_t(index)].second);
    const bool ok = readAt(f, m_index[size_t(index)].first, out.data(), out.size());
    std::fclose(f);
    return ok;
}

bool OffsetStore::loadChunk(int index, std::vector<uint8_t>& raw) const {
    std::vector<uint8_t> comp;
    if (!rawChunk(index, comp)) return false;
    const unsigned long long n = ZSTD_getFrameContentSize(comp.data(), comp.size());
    if (n == ZSTD_CONTENTSIZE_ERROR || n == ZSTD_CONTENTSIZE_UNKNOWN || n > (1ull << 31)) return false;
    raw.resize(size_t(n));
    const size_t r = ZSTD_decompress(raw.data(), raw.size(), comp.data(), comp.size());
    return !ZSTD_isError(r) && r == raw.size();
}

bool OffsetStore::get(int64_t frame, FrameOffsets& out) const {
    out.reset(m_h.lines, m_h.slope);
    if (!m_open || frame < 0 || frame >= m_h.frames || m_h.chunkFrames <= 0) return false;
    const int c = int(frame / m_h.chunkFrames);
    std::lock_guard<std::mutex> lk(m_mutex);
    auto it = m_cache.find(c);
    if (it == m_cache.end()) {
        std::vector<uint8_t> raw;
        if (!loadChunk(c, raw)) return false;
        if (m_cache.size() >= 8) {
            m_cache.erase(m_lru.front());
            m_lru.erase(m_lru.begin());
        }
        it = m_cache.emplace(c, std::move(raw)).first;
        m_lru.push_back(c);
    } else {
        m_lru.erase(std::find(m_lru.begin(), m_lru.end(), c));
        m_lru.push_back(c);
    }
    const size_t fb = m_h.frameBytes();
    const size_t at = size_t(frame - int64_t(c) * m_h.chunkFrames) * fb;
    if (at + fb > it->second.size()) return false;
    unpackFrame(it->second.data() + at, m_h, out);
    return true;
}

// ---------------------------------------------------------------------------

OffsetWriter::~OffsetWriter() { abort(); }

void OffsetWriter::abort() {
    if (m_f) {
        std::fclose(m_f);
        m_f = nullptr;
        std::remove(m_tmp.c_str());
    }
}

bool OffsetWriter::create(const std::string& path, const OffsetHeader& h, const OffsetStore* base, int64_t from, int64_t to,
                          std::string& err) {
    abort();
    m_path = path;
    m_tmp = path + ".tmp";
    m_h = h;
    m_base = base && base->isOpen() && base->header().lines == h.lines && base->header().frames == h.frames &&
                     base->header().chunkFrames == h.chunkFrames && base->header().slope == h.slope
                 ? base
                 : nullptr;
    m_from = from;
    m_to = to;
    m_index.clear();
    m_cur = -1;
    m_written = 0;
    m_failed = false;
    std::error_code ec;
    fs::create_directories(fs::path(path).parent_path(), ec);
    m_f = std::fopen(m_tmp.c_str(), "wb");
    if (!m_f) {
        err = "cannot write " + m_tmp;
        return false;
    }
    std::vector<uint8_t> hdr;
    put32(hdr, kSkipHeader);
    put32(hdr, 64);
    hdr.insert(hdr.end(), kMagic, kMagic + 8);
    put32(hdr, 1);  // version
    put32(hdr, uint32_t(h.lines));
    put32(hdr, uint32_t(h.width));
    put64(hdr, uint64_t(h.frames));
    put32(hdr, uint32_t(h.chunkFrames));
    put32(hdr, h.slope ? 1u : 0u);
    putF(hdr, h.xref);
    put32(hdr, uint32_t(h.firstParity));
    hdr.resize(8 + 64, 0);
    if (std::fwrite(hdr.data(), 1, hdr.size(), m_f) != hdr.size()) {
        err = "cannot write " + m_tmp;
        return false;
    }
    m_pos = hdr.size();
    return true;
}

bool OffsetWriter::emitChunk(int chunk, const std::vector<uint8_t>* raw) {
    std::vector<uint8_t> comp;
    if (raw) {
        comp.resize(ZSTD_compressBound(raw->size()));
        const size_t n = ZSTD_compress(comp.data(), comp.size(), raw->data(), raw->size(), kLevel);
        if (ZSTD_isError(n)) return false;
        comp.resize(n);
    } else if (!m_base || !m_base->rawChunk(chunk, comp)) {
        return false;
    }
    if (std::fwrite(comp.data(), 1, comp.size(), m_f) != comp.size()) return false;
    m_index.push_back({m_pos, uint32_t(comp.size())});
    m_pos += comp.size();
    m_written = chunk + 1;
    return true;
}

bool OffsetWriter::startChunk(int chunk) {
    const int64_t f0 = int64_t(chunk) * m_h.chunkFrames;
    const int64_t n = std::min<int64_t>(m_h.chunkFrames, m_h.frames - f0);
    const size_t fb = m_h.frameBytes();
    m_raw.assign(size_t(n) * fb, 0);
    FrameOffsets fo;
    for (int64_t i = 0; i < n; ++i) {
        const int64_t fr = f0 + i;
        if (fr >= m_from && fr < m_to) continue;  // filled by put()
        if (m_base && m_base->get(fr, fo)) packFrame(fo, m_h, m_raw.data() + size_t(i) * fb);
    }
    m_cur = chunk;
    return true;
}

// Writes every chunk before `chunk`: untouched ones are copied as stored.
bool OffsetWriter::flushTo(int chunk) {
    if (m_cur >= 0 && m_cur < chunk) {
        if (!emitChunk(m_cur, &m_raw)) return false;
        m_cur = -1;
    }
    while (m_written < chunk) {
        const int c = m_written;
        const int64_t f0 = int64_t(c) * m_h.chunkFrames, f1 = std::min<int64_t>(m_h.frames, f0 + m_h.chunkFrames);
        const bool touched = f1 > m_from && f0 < m_to;
        if (!touched && m_base) {
            if (!emitChunk(c, nullptr)) return false;
        } else {
            startChunk(c);
            if (!emitChunk(c, &m_raw)) return false;
            m_cur = -1;
        }
    }
    return true;
}

bool OffsetWriter::put(int64_t frame, const FrameOffsets& fo) {
    if (!m_f || m_failed || frame < 0 || frame >= m_h.frames) return false;
    const int c = int(frame / m_h.chunkFrames);
    if (c != m_cur) {
        if (!flushTo(c)) {
            m_failed = true;
            return false;
        }
        if (m_cur != c) startChunk(c);
    }
    packFrame(fo, m_h, m_raw.data() + size_t(frame - int64_t(c) * m_h.chunkFrames) * m_h.frameBytes());
    return true;
}

bool OffsetWriter::finish(std::string& err) {
    if (!m_f) {
        err = "not open";
        return false;
    }
    const int total = int((m_h.frames + m_h.chunkFrames - 1) / std::max(1, m_h.chunkFrames));
    if (m_failed || !flushTo(total)) {
        err = "cannot write " + m_tmp;
        abort();
        return false;
    }
    std::vector<uint8_t> idx;
    put32(idx, kSkipIndex);
    const uint32_t payload = uint32_t(4 + m_index.size() * 12 + 4);
    put32(idx, payload);
    put32(idx, uint32_t(m_index.size()));
    for (const auto& [o, s] : m_index) {
        put64(idx, o);
        put32(idx, s);
    }
    put32(idx, payload);
    const bool ok = std::fwrite(idx.data(), 1, idx.size(), m_f) == idx.size();
    const bool closed = std::fclose(m_f) == 0;
    m_f = nullptr;
    if (!ok || !closed) {
        std::remove(m_tmp.c_str());
        err = "cannot write " + m_tmp;
        return false;
    }
    std::error_code ec;
    fs::rename(m_tmp, m_path, ec);
    if (ec) {
        err = "cannot replace " + m_path + ": " + ec.message();
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------

bool TrackStore::save(const std::string& path, std::string& err) const {
    std::vector<uint8_t> raw;
    raw.reserve(fields.size() * 53);
    for (const FieldTrack& f : fields) {
        raw.push_back(f.flags);
        putF(raw, f.meanAbs);
        putF(raw, f.maxPos);
        putF(raw, f.maxNeg);
        for (int i = 0; i < kMaxReferences; ++i) {
            raw.push_back(uint8_t(f.refs[i].state));
            raw.push_back(f.refs[i].measured);
            putF(raw, f.refs[i].a);
            putF(raw, f.refs[i].b);
        }
    }
    std::vector<uint8_t> out;
    put32(out, kSkipHeader);
    put32(out, 24);
    out.insert(out.end(), kTrackMagic, kTrackMagic + 8);
    put32(out, 1);
    put64(out, uint64_t(fields.size()));
    put32(out, uint32_t(kMaxReferences));
    std::vector<uint8_t> comp(ZSTD_compressBound(raw.size()));
    const size_t n = ZSTD_compress(comp.data(), comp.size(), raw.data(), raw.size(), kLevel);
    if (ZSTD_isError(n)) {
        err = "compress failed";
        return false;
    }
    out.insert(out.end(), comp.begin(), comp.begin() + long(n));
    const std::string tmp = path + ".tmp";
    FILE* f = std::fopen(tmp.c_str(), "wb");
    if (!f) {
        err = "cannot write " + path;
        return false;
    }
    const bool ok = std::fwrite(out.data(), 1, out.size(), f) == out.size();
    if (std::fclose(f) != 0 || !ok) {
        std::remove(tmp.c_str());
        err = "cannot write " + path;
        return false;
    }
    std::error_code ec;
    fs::rename(tmp, path, ec);
    if (ec) {
        err = ec.message();
        return false;
    }
    return true;
}

bool TrackStore::load(const std::string& path, std::string& err) {
    fields.clear();
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) {
        err = "cannot open " + path;
        return false;
    }
    std::vector<uint8_t> all;
    uint8_t buf[65536];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) all.insert(all.end(), buf, buf + n);
    std::fclose(f);
    if (all.size() < 32 || get32(all.data()) != kSkipHeader || std::memcmp(all.data() + 8, kTrackMagic, 8) != 0) {
        err = path + " is not a tracks file";
        return false;
    }
    const uint64_t count = get64(all.data() + 20);
    const int nrefs = int(get32(all.data() + 28));
    const uint8_t* z = all.data() + 32;
    const size_t zn = all.size() - 32;
    const unsigned long long rawSize = ZSTD_getFrameContentSize(z, zn);
    const size_t rec = 13 + size_t(nrefs) * 10;
    if (rawSize == ZSTD_CONTENTSIZE_ERROR || rawSize == ZSTD_CONTENTSIZE_UNKNOWN || rawSize != count * rec) {
        err = path + " is damaged";
        return false;
    }
    std::vector<uint8_t> raw(static_cast<size_t>(rawSize));
    if (ZSTD_isError(ZSTD_decompress(raw.data(), raw.size(), z, zn))) {
        err = path + " is damaged";
        return false;
    }
    fields.resize(size_t(count));
    for (uint64_t i = 0; i < count; ++i) {
        const uint8_t* p = raw.data() + i * rec;
        FieldTrack& t = fields[size_t(i)];
        t.flags = p[0];
        t.meanAbs = getF(p + 1);
        t.maxPos = getF(p + 5);
        t.maxNeg = getF(p + 9);
        for (int r = 0; r < nrefs && r < kMaxReferences; ++r) {
            const uint8_t* q = p + 13 + size_t(r) * 10;
            t.refs[r].state = RefState(q[0]);
            t.refs[r].measured = q[1];
            t.refs[r].a = getF(q + 2);
            t.refs[r].b = getF(q + 6);
        }
    }
    refs = nrefs;
    return true;
}

void TrackStore::safeCrop(double strength, int& left, int& right) const {
    double mx = 0, mn = 0;
    for (const FieldTrack& f : fields) {
        mx = std::max(mx, double(f.maxPos));
        mn = std::min(mn, double(f.maxNeg));
    }
    // Sampling at x + e: a positive e pulls in picture from past the right
    // end, a negative one from before the left end.
    left = int(std::ceil(-mn * strength - 1e-6));
    right = int(std::ceil(mx * strength - 1e-6));
    left = std::max(0, left);
    right = std::max(0, right);
}

// ---------------------------------------------------------------------------

void applyOffsets(Picture& pic, const FrameOffsets& fo, const ApplyParams& p) {
    if (p.strength == 0) return;
    const int w = pic.width;
    std::vector<float> tmp(static_cast<size_t>(w));
    for (int y = 0; y < pic.height && y < int(fo.off.size()); ++y) {
        const double e = fo.offsetPx(y) * p.strength;
        const double s = fo.slopeAt(y) * p.strength;
        if (e == 0 && s == 0) continue;
        for (int c = 0; c < pic.planes(); ++c) {
            float* row = pic.p[c].row(y);
            const float fill = c == 0 ? 16.0f : 128.0f;
            if (s == 0) shiftRow(row, tmp.data(), w, e, fill, p.fillRepeat);
            else warpRow(row, tmp.data(), w, e, s, p.xref, fill, p.fillRepeat);
            std::memcpy(row, tmp.data(), sizeof(float) * size_t(w));
        }
    }
}

void fieldsToFrame(const FieldOffsets& a, const FieldOffsets& b, int frameLines, bool slope, FrameOffsets& out) {
    out.reset(frameLines, slope);
    for (const FieldOffsets* f : {&a, &b}) {
        for (size_t l = 0; l < f->off.size(); ++l) {
            const int y = 2 * int(l) + f->parity;
            if (y >= frameLines) continue;
            out.off[size_t(y)] = int16_t(std::clamp(std::lround(f->off[l] * kOffsetScale), -32767L, 32767L));
            out.conf[size_t(y)] = f->conf[l];
            if (slope && l < f->slope.size())
                out.slope[size_t(y)] = int16_t(std::clamp(std::lround(f->slope[l] * kSlopeScale), -32767L, 32767L));
        }
    }
}

}  // namespace ud

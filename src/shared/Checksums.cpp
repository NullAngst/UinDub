// SPDX-License-Identifier: GPL-3.0-or-later
// Copied from PingDub (github.com/NullAngst/PingDub, same author, GPL-3.0-or-later).
#include "Checksums.h"



#include <algorithm>
#include <atomic>
#include <cinttypes>
#include <cmath>
#include <cctype>
#include <cstdarg>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <set>
#include <sys/stat.h>
#include <thread>

#if defined(__x86_64__) || defined(__i386__)
#include <immintrin.h>
#define PD_SHA_NI 1
#endif

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavformat/version.h>
#include <libavutil/imgutils.h>
#include <libavutil/md5.h>
#include <libavutil/mem.h>
#include <libavutil/pixdesc.h>
#include <libavutil/sha.h>
}

namespace ud {

std::string toHex(const uint8_t* d, size_t n) {
    static const char* x = "0123456789abcdef";
    std::string s(n * 2, '0');
    for (size_t i = 0; i < n; ++i) {
        s[2 * i] = x[d[i] >> 4];
        s[2 * i + 1] = x[d[i] & 15];
    }
    return s;
}

bool frameMd5(const AVFrame* f, uint8_t out[16], int& size) {
    const AVPixelFormat fmt = AVPixelFormat(f->format);
    const AVPixFmtDescriptor* d = av_pix_fmt_desc_get(fmt);
    if (!d) return false;
    int lines[4] = {0, 0, 0, 0};
    if (av_image_fill_linesizes(lines, fmt, f->width) < 0) return false;
    AVMD5* ctx = av_md5_alloc();
    if (!ctx) return false;
    av_md5_init(ctx);
    const int planes = av_pix_fmt_count_planes(fmt);
    for (int p = 0; p < planes && p < 4; ++p) {
        const int h = (p == 1 || p == 2) ? AV_CEIL_RSHIFT(f->height, d->log2_chroma_h) : f->height;
        for (int y = 0; y < h; ++y) av_md5_update(ctx, f->data[p] + size_t(y) * size_t(f->linesize[p]), size_t(lines[p]));
    }
    av_md5_final(ctx, out);
    av_free(ctx);
    size = av_image_get_buffer_size(fmt, f->width, f->height, 1);
    return true;
}

std::string sidecarFor(const std::string& mediaPath, const std::string& ext) {
    const size_t slash = mediaPath.find_last_of('/');
    const size_t dot = mediaPath.find_last_of('.');
    const std::string stem = (dot == std::string::npos || (slash != std::string::npos && dot < slash)) ? mediaPath : mediaPath.substr(0, dot);
    return stem + ext;
}

// ---------------------------------------------------------------------------

FrameMd5Writer::~FrameMd5Writer() { close(); }

bool FrameMd5Writer::open(const std::string& path, const FrameMd5Header& h) {
    close();
    m_f = std::fopen(path.c_str(), "w");
    if (!m_f) return false;
    m_path = path;
    m_count = 0;
    m_failed = false;
    std::fprintf(m_f,
                 "#format: frame checksums\n#version: 2\n#hash: MD5\n#software: %s\n#tb 0: %d/%d\n#media_type 0: video\n"
                 "#codec_id 0: %s\n#dimensions 0: %dx%d\n#sar 0: %d/%d\n#stream#, dts,        pts, duration,     size, hash\n",
                 LIBAVFORMAT_IDENT, h.tb.num, h.tb.den, h.codec.c_str(), h.width, h.height, h.sar.num, h.sar.den);
    return true;
}

void FrameMd5Writer::add(int64_t pts, int size, const uint8_t md5[16]) {
    if (!m_f) return;
    const std::string hx = toHex(md5, 16);
    if (std::fprintf(m_f, "0, %10" PRId64 ", %10" PRId64 ", %8" PRId64 ", %8d, %s\n", pts, pts, int64_t(1), size, hx.c_str()) < 0)
        m_failed = true;
    ++m_count;
    // The writer thread flushes once a second's worth, so a crash keeps nearly all of it.
    if (m_count % 30 == 0) std::fflush(m_f);
}

bool FrameMd5Writer::close() {
    if (!m_f) return true;
    const bool ok = std::fclose(m_f) == 0 && !m_failed;
    m_f = nullptr;
    return ok;
}

bool readFrameMd5(const std::string& path, std::vector<FrameMd5Entry>& out, std::string& err) {
    out.clear();
    FILE* f = std::fopen(path.c_str(), "r");
    if (!f) {
        err = "no " + path;
        return false;
    }
    char line[512];
    while (std::fgets(line, sizeof line, f)) {
        if (line[0] == '#' || line[0] == '\n') continue;
        int stream = -1;
        long long dts = 0, pts = 0, dur = 0, size = 0;
        char hash[129] = {0};
        if (std::sscanf(line, "%d , %lld , %lld , %lld , %lld , %128s", &stream, &dts, &pts, &dur, &size, hash) == 6 && stream == 0)
            out.push_back({pts, size, hash});
    }
    std::fclose(f);
    return true;
}

// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// SHA-256 with the x86 SHA extensions, checked at run time.

namespace {

alignas(16) const uint32_t kK[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01,
    0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc,
    0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147,
    0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116, 0x1e376c08,
    0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
    0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};

#ifdef PD_SHA_NI
__attribute__((target("sha,sse4.1,ssse3"))) void shaBlocks(uint32_t st[8], const uint8_t* data, size_t blocks) {
    const __m128i mask = _mm_set_epi64x(0x0c0d0e0f08090a0bLL, 0x0405060700010203LL);
    __m128i tmp = _mm_loadu_si128(reinterpret_cast<const __m128i*>(&st[0]));
    __m128i s1 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(&st[4]));
    tmp = _mm_shuffle_epi32(tmp, 0xB1);           // CDAB
    s1 = _mm_shuffle_epi32(s1, 0x1B);             // EFGH
    __m128i s0 = _mm_alignr_epi8(tmp, s1, 8);     // ABEF
    s1 = _mm_blend_epi16(s1, tmp, 0xF0);          // CDGH
    while (blocks--) {
        const __m128i abef = s0, cdgh = s1;
        __m128i w[4];
        for (int k = 0; k < 4; ++k)
            w[k] = _mm_shuffle_epi8(_mm_loadu_si128(reinterpret_cast<const __m128i*>(data + 16 * k)), mask);
        for (int i = 0; i < 16; ++i) {
            __m128i m = _mm_add_epi32(w[i & 3], _mm_load_si128(reinterpret_cast<const __m128i*>(&kK[4 * i])));
            s1 = _mm_sha256rnds2_epu32(s1, s0, m);
            m = _mm_shuffle_epi32(m, 0x0E);
            s0 = _mm_sha256rnds2_epu32(s0, s1, m);
            if (i < 12) {
                // Next four schedule words from the last sixteen.
                __m128i t = _mm_sha256msg1_epu32(w[i & 3], w[(i + 1) & 3]);
                t = _mm_add_epi32(t, _mm_alignr_epi8(w[(i + 3) & 3], w[(i + 2) & 3], 4));
                w[i & 3] = _mm_sha256msg2_epu32(t, w[(i + 3) & 3]);
            }
        }
        s0 = _mm_add_epi32(s0, abef);
        s1 = _mm_add_epi32(s1, cdgh);
        data += 64;
    }
    tmp = _mm_shuffle_epi32(s0, 0x1B);            // FEBA
    s1 = _mm_shuffle_epi32(s1, 0xB1);             // DCHG
    s0 = _mm_blend_epi16(tmp, s1, 0xF0);          // DCBA
    s1 = _mm_alignr_epi8(s1, tmp, 8);             // ABEF
    _mm_storeu_si128(reinterpret_cast<__m128i*>(&st[0]), s0);
    _mm_storeu_si128(reinterpret_cast<__m128i*>(&st[4]), s1);
}
#endif

}  // namespace

bool Sha256::hardware() {
#ifdef PD_SHA_NI
    // PINGDUB_NO_SHA_NI=1 forces the plain C path, for testing it on a CPU that has the instructions.
    static const bool yes = __builtin_cpu_supports("sha") && __builtin_cpu_supports("sse4.1") && __builtin_cpu_supports("ssse3") &&
                            !std::getenv("PINGDUB_NO_SHA_NI");
    return yes;
#else
    return false;
#endif
}

Sha256::Sha256() {
    static const uint32_t init[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    std::memcpy(m_state, init, sizeof m_state);
    if (!hardware()) {
        m_soft = av_sha_alloc();
        av_sha_init(m_soft, 256);
    }
}

Sha256::~Sha256() { av_free(m_soft); }

void Sha256::update(const uint8_t* d, size_t n) {
    if (m_soft) {
        av_sha_update(m_soft, d, n);
        return;
    }
#ifdef PD_SHA_NI
    m_total += n;
    if (m_fill) {
        const size_t take = std::min(n, size_t(64) - m_fill);
        std::memcpy(m_buf + m_fill, d, take);
        m_fill += take;
        d += take;
        n -= take;
        if (m_fill < 64) return;
        shaBlocks(m_state, m_buf, 1);
        m_fill = 0;
    }
    if (n >= 64) {
        shaBlocks(m_state, d, n / 64);
        d += n & ~size_t(63);
        n &= 63;
    }
    if (n) {
        std::memcpy(m_buf, d, n);
        m_fill = n;
    }
#endif
}

void Sha256::final(uint8_t out[32]) {
    if (m_soft) {
        av_sha_final(m_soft, out);
        return;
    }
#ifdef PD_SHA_NI
    const uint64_t bits = m_total * 8;
    uint8_t pad[72] = {0x80};
    const size_t padLen = (m_fill < 56 ? 56 : 120) - m_fill;
    uint8_t len[8];
    for (int i = 0; i < 8; ++i) len[i] = uint8_t(bits >> (56 - 8 * i));
    const uint64_t keep = m_total;
    update(pad, padLen);
    update(len, 8);
    m_total = keep;
    for (int i = 0; i < 8; ++i) {
        out[4 * i] = uint8_t(m_state[i] >> 24);
        out[4 * i + 1] = uint8_t(m_state[i] >> 16);
        out[4 * i + 2] = uint8_t(m_state[i] >> 8);
        out[4 * i + 3] = uint8_t(m_state[i]);
    }
#endif
}

bool sha256File(const std::string& path, std::string& hex, std::string& err,
                const std::function<bool(uint64_t, uint64_t)>& progress) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) {
        err = "cannot open " + path;
        return false;
    }
    struct stat st;
    const uint64_t total = ::stat(path.c_str(), &st) == 0 ? uint64_t(st.st_size) : 0;
    Sha256 sha;
    std::vector<uint8_t> buf(4u << 20);
    uint64_t done = 0;
    bool ok = true;
    while (true) {
        const size_t n = std::fread(buf.data(), 1, buf.size(), f);
        if (n) sha.update(buf.data(), n);
        done += n;
        if (progress && !progress(done, total)) {
            err = "cancelled";
            ok = false;
            break;
        }
        if (n < buf.size()) {
            if (std::ferror(f)) {
                err = "read error in " + path;
                ok = false;
            }
            break;
        }
    }
    std::fclose(f);
    uint8_t dig[32];
    sha.final(dig);
    if (ok) hex = toHex(dig, 32);
    return ok;
}

std::string writeSha256Sidecar(const std::string& mediaPath, const std::string& hex) {
    const std::string side = sidecarFor(mediaPath, ".sha256");
    const size_t slash = mediaPath.find_last_of('/');
    const std::string name = slash == std::string::npos ? mediaPath : mediaPath.substr(slash + 1);
    FILE* f = std::fopen(side.c_str(), "w");
    if (!f) return {};
    const bool ok = std::fprintf(f, "%s  %s\n", hex.c_str(), name.c_str()) > 0;
    return std::fclose(f) == 0 && ok ? side : std::string();
}

std::string readSha256Sidecar(const std::string& sidecar) {
    FILE* f = std::fopen(sidecar.c_str(), "r");
    if (!f) return {};
    char buf[160] = {0};
    std::string hex;
    if (std::fgets(buf, sizeof buf, f)) {
        for (char* p = buf; *p && !std::isspace(static_cast<unsigned char>(*p)); ++p) hex += char(std::tolower(static_cast<unsigned char>(*p)));
    }
    std::fclose(f);
    return hex.size() == 64 ? hex : std::string();
}

// ---------------------------------------------------------------------------
// FFV1 reports a slice CRC mismatch only through av_log, so a log hook counts
// them per decoder context while a verify runs.

namespace {

std::mutex g_logMutex;
std::map<const void*, int>* g_crc = nullptr;

void logHook(void* avcl, int level, const char* fmt, va_list vl) {
    {
        std::lock_guard<std::mutex> lk(g_logMutex);
        if (g_crc && avcl && g_crc->count(avcl) && fmt && std::strstr(fmt, "CRC")) ++(*g_crc)[avcl];
    }
    va_list copy;
    va_copy(copy, vl);
    av_log_default_callback(avcl, level, fmt, copy);
    va_end(copy);
}

void watchCrc(const void* ctx, bool on) {
    static std::once_flag once;
    std::call_once(once, [] {
        g_crc = new std::map<const void*, int>();
        av_log_set_callback(logHook);
    });
    std::lock_guard<std::mutex> lk(g_logMutex);
    if (on) (*g_crc)[ctx] = 0;
    else g_crc->erase(ctx);
}

int crcCount(const void* ctx) {
    std::lock_guard<std::mutex> lk(g_logMutex);
    if (!g_crc) return 0;
    const auto it = g_crc->find(ctx);
    return it == g_crc->end() ? 0 : it->second;
}

}  // namespace

std::string VerifyResult::summary() const {
    if (!error.empty()) return error;
    if (cancelled) return "Verify cancelled.";
    std::string s;
    if (!haveSha) s += "No .sha256 sidecar, so the file checksum could not be compared. ";
    else if (shaMatch) s += "SHA-256 matches. ";
    else s += "SHA-256 DOES NOT MATCH: the file changed since capture. ";
    if (full) {
        char buf[200];
        if (!haveFrameMd5) {
            std::snprintf(buf, sizeof buf, "No .framemd5 sidecar; %llu frames decoded. ", (unsigned long long)framesChecked);
            s += buf;
        } else if (badFrames.empty() && missingFrames.empty() && decodeErrors.empty()) {
            std::snprintf(buf, sizeof buf, "All %llu frames decode to the captured picture. ", (unsigned long long)framesChecked);
            s += buf;
        } else {
            std::snprintf(buf, sizeof buf, "%zu frames differ from the capture, %zu missing, %zu would not decode. ", badFrames.size(),
                          missingFrames.size(), decodeErrors.size());
            s += buf;
        }
        if (!crcErrors.empty()) {
            std::snprintf(buf, sizeof buf, "%zu FFV1 slice CRC errors. ", crcErrors.size());
            s += buf;
        }
    }
    while (!s.empty() && s.back() == ' ') s.pop_back();
    return s;
}

VerifyResult verifyCapture(const std::string& path, bool full, const std::function<bool(double, const std::string&)>& progress) {
    VerifyResult r;
    r.full = full;
    const std::string expected = readSha256Sidecar(sidecarFor(path, ".sha256"));
    r.haveSha = !expected.empty();
    r.shaExpected = expected;
    {
        std::string err;
        const bool ok = sha256File(path, r.shaActual, err, [&](uint64_t d, uint64_t t) {
            const double frac = t ? double(d) / double(t) : 0.0;
            return !progress || progress(full ? frac * 0.3 : frac, "SHA-256");
        });
        if (!ok) {
            if (err == "cancelled") r.cancelled = true;
            else r.error = err;
            return r;
        }
        r.shaMatch = r.haveSha && r.shaActual == expected;
    }
    if (!full) {
        r.ok = r.haveSha && r.shaMatch;
        return r;
    }

    std::vector<FrameMd5Entry> sidecar;
    std::string err;
    r.haveFrameMd5 = readFrameMd5(sidecarFor(path, ".framemd5"), sidecar, err) && !sidecar.empty();
    std::map<int64_t, const FrameMd5Entry*> want;
    for (const auto& e : sidecar) want[e.pts] = &e;
    r.framesExpected = sidecar.size();

    AVFormatContext* in = nullptr;
    if (avformat_open_input(&in, path.c_str(), nullptr, nullptr) < 0) {
        r.error = "cannot open " + path;
        return r;
    }
    avformat_find_stream_info(in, nullptr);
    const int vi = av_find_best_stream(in, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (vi < 0) {
        avformat_close_input(&in);
        r.error = "no video stream in " + path;
        return r;
    }
    AVStream* st = in->streams[vi];
    AVRational fr = st->avg_frame_rate.num ? st->avg_frame_rate : st->r_frame_rate;
    if (!fr.num) fr = AVRational{30000, 1001};
    r.fps = {fr.num, fr.den};
    const AVCodec* dec = avcodec_find_decoder(st->codecpar->codec_id);
    AVCodecContext* dc = dec ? avcodec_alloc_context3(dec) : nullptr;
    if (!dc || avcodec_parameters_to_context(dc, st->codecpar) < 0) {
        if (dc) avcodec_free_context(&dc);
        avformat_close_input(&in);
        r.error = "no decoder for this file";
        return r;
    }
    dc->err_recognition = AV_EF_CRCCHECK;
    // Slice threads keep decoding synchronous, so a CRC report belongs to the packet just sent.
    dc->thread_type = FF_THREAD_SLICE;
    dc->thread_count = int(std::max(1u, std::thread::hardware_concurrency()));
    if (avcodec_open2(dc, dec, nullptr) < 0) {
        avcodec_free_context(&dc);
        avformat_close_input(&in);
        r.error = "cannot open the decoder";
        return r;
    }
    watchCrc(dc, true);
    auto indexOf = [&](int64_t ts) -> int64_t {
        if (ts == AV_NOPTS_VALUE) return -1;
        return int64_t(std::llround(double(ts) * av_q2d(st->time_base) * av_q2d(fr)));
    };
    struct stat sb;
    const double total = ::stat(path.c_str(), &sb) == 0 ? double(sb.st_size) : 0.0;
    std::set<int64_t> seen;
    AVPacket* pkt = av_packet_alloc();
    AVFrame* frame = av_frame_alloc();
    auto receive = [&] {
        while (true) {
            const int rr = avcodec_receive_frame(dc, frame);
            if (rr == AVERROR(EAGAIN) || rr == AVERROR_EOF) return;
            if (rr < 0) return;
            const int64_t idx = indexOf(frame->best_effort_timestamp);
            ++r.framesChecked;
            seen.insert(idx);
            uint8_t md5[16];
            int size = 0;
            if (frame->decode_error_flags) r.decodeErrors.push_back(idx);
            if (r.haveFrameMd5 && frameMd5(frame, md5, size)) {
                auto it = want.find(idx);
                if (it != want.end() && (it->second->hash != toHex(md5, 16) || it->second->size != size)) r.badFrames.push_back(idx);
            }
            av_frame_unref(frame);
        }
    };
    bool cancelled = false;
    while (av_read_frame(in, pkt) >= 0) {
        if (pkt->stream_index != vi) {
            av_packet_unref(pkt);
            continue;
        }
        const int64_t idx = indexOf(pkt->pts != AV_NOPTS_VALUE ? pkt->pts : pkt->dts);
        const int before = crcCount(dc);
        if (avcodec_send_packet(dc, pkt) < 0) r.decodeErrors.push_back(idx);
        receive();
        if (crcCount(dc) > before) r.crcErrors.push_back(idx);
        if (progress && total > 0 && pkt->pos >= 0 && !progress(0.3 + 0.7 * double(pkt->pos) / total, "frames")) {
            cancelled = true;
            av_packet_unref(pkt);
            break;
        }
        av_packet_unref(pkt);
    }
    if (!cancelled) {
        avcodec_send_packet(dc, nullptr);
        receive();
    }
    watchCrc(dc, false);
    av_frame_free(&frame);
    av_packet_free(&pkt);
    avcodec_free_context(&dc);
    avformat_close_input(&in);
    if (cancelled) {
        r.cancelled = true;
        return r;
    }
    if (r.haveFrameMd5)
        for (const auto& e : sidecar)
            if (!seen.count(e.pts)) r.missingFrames.push_back(e.pts);
    auto uniq = [](std::vector<int64_t>& v) {
        std::sort(v.begin(), v.end());
        v.erase(std::unique(v.begin(), v.end()), v.end());
    };
    uniq(r.badFrames);
    uniq(r.decodeErrors);
    uniq(r.crcErrors);
    const bool framesOk = r.badFrames.empty() && r.missingFrames.empty() && r.decodeErrors.empty() && r.crcErrors.empty();
    r.ok = r.haveSha && r.shaMatch && r.haveFrameMd5 && framesOk;
    return r;
}

}  // namespace ud

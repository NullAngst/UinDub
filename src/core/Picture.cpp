// SPDX-License-Identifier: GPL-3.0-or-later
#include "Picture.h"

#include <algorithm>
#include <cmath>
#include <cstring>

extern "C" {
#include <libavutil/imgutils.h>
#include <libavutil/pixdesc.h>
}

namespace ud {

void Picture::alloc(int w, int h, bool isGray) {
    width = w;
    height = h;
    gray = isGray;
    for (int i = 0; i < 3; ++i) {
        if (i == 0 || !isGray) p[i].resize(w, h);
        else p[i] = Plane{};
    }
}

namespace {

// Planar layout with one component per plane, native endian, byte aligned.
bool simplePlanar(const AVPixFmtDescriptor* d) {
    if (!d || (d->flags & (AV_PIX_FMT_FLAG_BITSTREAM | AV_PIX_FMT_FLAG_PAL | AV_PIX_FMT_FLAG_HWACCEL | AV_PIX_FMT_FLAG_RGB))) return false;
    if (d->nb_components != 1 && !(d->flags & AV_PIX_FMT_FLAG_PLANAR)) return false;
#if AV_HAVE_BIGENDIAN
    const bool native = (d->flags & AV_PIX_FMT_FLAG_BE) != 0 || d->comp[0].depth <= 8;
#else
    const bool native = (d->flags & AV_PIX_FMT_FLAG_BE) == 0 || d->comp[0].depth <= 8;
#endif
    if (!native) return false;
    const int n = std::min<int>(3, d->nb_components);
    for (int c = 0; c < n; ++c) {
        const int bytes = d->comp[c].depth > 8 ? 2 : 1;
        if (d->comp[c].plane != c || d->comp[c].offset != 0 || d->comp[c].step != bytes || d->comp[c].shift != 0) return false;
    }
    return true;
}

// Reads one plane at its own resolution into `dst` (dw x dh floats).
void readPlane(const AVFrame* f, const AVPixFmtDescriptor* d, int comp, int dw, int dh, float scale, std::vector<float>& dst) {
    dst.resize(size_t(dw) * size_t(dh));
    if (simplePlanar(d)) {
        const int bytes = d->comp[comp].depth > 8 ? 2 : 1;
        for (int y = 0; y < dh; ++y) {
            const uint8_t* src = f->data[comp] + size_t(y) * size_t(f->linesize[comp]);
            float* o = dst.data() + size_t(y) * size_t(dw);
            if (bytes == 1)
                for (int x = 0; x < dw; ++x) o[x] = float(src[x]) * scale;
            else {
                const uint16_t* s16 = reinterpret_cast<const uint16_t*>(src);
                for (int x = 0; x < dw; ++x) o[x] = float(s16[x]) * scale;
            }
        }
        return;
    }
    std::vector<uint16_t> line(static_cast<size_t>(dw));
    const uint8_t* data[4] = {f->data[0], f->data[1], f->data[2], f->data[3]};
    for (int y = 0; y < dh; ++y) {
        av_read_image_line2(line.data(), data, f->linesize, d, 0, y, comp, dw, 0, 2);
        float* o = dst.data() + size_t(y) * size_t(dw);
        for (int x = 0; x < dw; ++x) o[x] = float(line[size_t(x)]) * scale;
    }
}

// Horizontal upsample of a cosited chroma row (MPEG-2 siting: chroma sample k
// sits on luma sample k * factor).
void upsampleRow(const float* c, int cw, float* o, int w, int factor) {
    if (factor == 1) {
        std::memcpy(o, c, sizeof(float) * size_t(std::min(cw, w)));
        return;
    }
    auto at = [&](int k) { return c[std::clamp(k, 0, cw - 1)]; };
    if (factor == 2) {
        for (int k = 0; k < cw; ++k) {
            const int x = 2 * k;
            if (x < w) o[x] = c[k];
            // Four-tap cubic halfway between k and k+1.
            if (x + 1 < w) o[x + 1] = (-at(k - 1) + 9.0f * at(k) + 9.0f * at(k + 1) - at(k + 2)) * (1.0f / 16.0f);
        }
        return;
    }
    for (int x = 0; x < w; ++x) {
        const float pos = float(x) / float(factor);
        const int k = int(std::floor(pos));
        const float t = pos - float(k);
        o[x] = at(k) * (1.0f - t) + at(k + 1) * t;
    }
}

void downsampleRow(const float* s, int w, float* o, int cw, int factor) {
    if (factor == 1) {
        std::memcpy(o, s, sizeof(float) * size_t(std::min(cw, w)));
        return;
    }
    auto at = [&](int x) { return s[std::clamp(x, 0, w - 1)]; };
    if (factor == 2) {
        for (int k = 0; k < cw; ++k) o[k] = 0.25f * at(2 * k - 1) + 0.5f * at(2 * k) + 0.25f * at(2 * k + 1);
        return;
    }
    for (int k = 0; k < cw; ++k) {
        float sum = 0;
        for (int i = 0; i < factor; ++i) sum += at(k * factor + i);
        o[k] = sum / float(factor);
    }
}

}  // namespace

bool toPicture(const AVFrame* f, Picture& out, bool interlaced) {
    const AVPixFmtDescriptor* d = av_pix_fmt_desc_get(AVPixelFormat(f->format));
    if (!d || (d->flags & (AV_PIX_FMT_FLAG_RGB | AV_PIX_FMT_FLAG_PAL | AV_PIX_FMT_FLAG_HWACCEL))) return false;
    const int w = f->width, h = f->height;
    const bool gray = d->nb_components < 3;
    out.alloc(w, h, gray);
    const float scale = d->comp[0].depth > 8 ? 1.0f / float(1 << (d->comp[0].depth - 8)) : float(1 << (8 - d->comp[0].depth));
    // Full-range input (MJPEG from cheap devices) is mapped onto the limited scale.
    const bool full = f->color_range == AVCOL_RANGE_JPEG || f->format == AV_PIX_FMT_YUVJ420P || f->format == AV_PIX_FMT_YUVJ422P ||
                      f->format == AV_PIX_FMT_YUVJ444P;
    std::vector<float> tmp;
    readPlane(f, d, 0, w, h, scale, out.p[0].d);
    if (full)
        for (float& v : out.p[0].d) v = 16.0f + v * (219.0f / 255.0f);
    if (gray) return true;
    const int fx = 1 << d->log2_chroma_w, fy = 1 << d->log2_chroma_h;
    const int cw = AV_CEIL_RSHIFT(w, d->log2_chroma_w), ch = AV_CEIL_RSHIFT(h, d->log2_chroma_h);
    std::vector<float> wide(size_t(w) * size_t(ch));
    for (int c = 1; c <= 2; ++c) {
        readPlane(f, d, c, cw, ch, scale, tmp);
        if (full)
            for (float& v : tmp) v = 128.0f + (v - 128.0f) * (224.0f / 255.0f);
        for (int y = 0; y < ch; ++y) upsampleRow(tmp.data() + size_t(y) * size_t(cw), cw, wide.data() + size_t(y) * size_t(w), w, fx);
        Plane& P = out.p[c];
        if (fy == 1) {
            std::memcpy(P.d.data(), wide.data(), sizeof(float) * wide.size());
            continue;
        }
        auto crow = [&](int r) { return wide.data() + size_t(std::clamp(r, 0, ch - 1)) * size_t(w); };
        for (int y = 0; y < h; ++y) {
            float* o = P.row(y);
            if (interlaced && fy == 2) {
                // Chroma row 2m + parity belongs to that field; within the field,
                // chroma row m sits between field lines 2m and 2m + 1.
                const int parity = y & 1, l = y >> 1;
                const float pos = (float(l) - 0.5f) * 0.5f;
                const int m = int(std::floor(pos));
                const float t = pos - float(m);
                const float* a = crow(2 * std::max(m, 0) + parity);
                const float* b = crow(2 * std::min(m + 1, (ch - 1 - parity) / 2) + parity);
                for (int x = 0; x < w; ++x) o[x] = a[x] * (1.0f - t) + b[x] * t;
            } else {
                const float pos = (float(y) + 0.5f) / float(fy) - 0.5f;
                const int m = int(std::floor(pos));
                const float t = pos - float(m);
                const float* a = crow(m);
                const float* b = crow(m + 1);
                for (int x = 0; x < w; ++x) o[x] = a[x] * (1.0f - t) + b[x] * t;
            }
        }
    }
    return true;
}

FramePtr fromPicture(const Picture& pic, AVPixelFormat fmt, bool interlaced) {
    const AVPixFmtDescriptor* d = av_pix_fmt_desc_get(fmt);
    if (!d) return nullptr;
    FramePtr f = makeFrame();
    f->format = fmt;
    f->width = pic.width;
    f->height = pic.height;
    if (av_frame_get_buffer(f.get(), 32) < 0) return nullptr;
    const int depth = d->comp[0].depth;
    const float mul = depth >= 8 ? float(1 << (depth - 8)) : 1.0f / float(1 << (8 - depth));
    const float maxv = float((1 << depth) - 1);
    const bool simple = simplePlanar(d);
    const int w = pic.width, h = pic.height;
    const bool outGray = d->nb_components < 3;
    if (!simple) {
        // av_write_image_line2 ORs bits into place for packed formats.
        for (int pl = 0; pl < 4 && f->buf[pl]; ++pl) std::memset(f->buf[pl]->data, 0, f->buf[pl]->size);
    }
    auto store = [&](int comp, int y, const float* v, int n) {
        if (simple) {
            uint8_t* dst = f->data[comp] + size_t(y) * size_t(f->linesize[comp]);
            if (depth <= 8)
                for (int x = 0; x < n; ++x) dst[x] = uint8_t(std::clamp(std::lrint(v[x] * mul), 0L, long(maxv)));
            else {
                uint16_t* d16 = reinterpret_cast<uint16_t*>(dst);
                for (int x = 0; x < n; ++x) d16[x] = uint16_t(std::clamp(std::lrint(v[x] * mul), 0L, long(maxv)));
            }
            return;
        }
        std::vector<uint16_t> line(static_cast<size_t>(n));
        for (int x = 0; x < n; ++x) line[size_t(x)] = uint16_t(std::clamp(std::lrint(v[x] * mul), 0L, long(maxv)));
        av_write_image_line2(line.data(), f->data, f->linesize, d, 0, y, comp, n, 2);
    };
    for (int y = 0; y < h; ++y) store(0, y, pic.p[0].row(y), w);
    if (outGray) return f;
    const int fx = 1 << d->log2_chroma_w, fy = 1 << d->log2_chroma_h;
    const int cw = AV_CEIL_RSHIFT(w, d->log2_chroma_w), ch = AV_CEIL_RSHIFT(h, d->log2_chroma_h);
    std::vector<float> narrow(size_t(cw) * size_t(h)), row(static_cast<size_t>(cw));
    for (int c = 1; c <= 2; ++c) {
        if (pic.gray) {
            std::fill(row.begin(), row.end(), 128.0f);
            for (int y = 0; y < ch; ++y) store(c, y, row.data(), cw);
            continue;
        }
        for (int y = 0; y < h; ++y) downsampleRow(pic.p[c].row(y), w, narrow.data() + size_t(y) * size_t(cw), cw, fx);
        auto nrow = [&](int r) { return narrow.data() + size_t(std::clamp(r, 0, h - 1)) * size_t(cw); };
        for (int y = 0; y < ch; ++y) {
            if (fy == 1) {
                store(c, y, nrow(y), cw);
                continue;
            }
            if (interlaced && fy == 2) {
                // Chroma row y = 2m + parity from field lines 2m and 2m + 1 of that field.
                const int parity = y & 1, m = y >> 1;
                const int last = ((h - 1 - parity) / 2) * 2 + parity;
                auto fl = [&](int l) { return nrow(std::clamp(2 * l + parity, parity, last)); };
                const float* a = fl(2 * m - 1);
                const float* b = fl(2 * m);
                const float* cc = fl(2 * m + 1);
                const float* e = fl(2 * m + 2);
                for (int x = 0; x < cw; ++x) row[size_t(x)] = 0.125f * a[x] + 0.375f * b[x] + 0.375f * cc[x] + 0.125f * e[x];
            } else if (fy == 2) {
                const float* a = nrow(2 * y - 1);
                const float* b = nrow(2 * y);
                const float* cc = nrow(2 * y + 1);
                const float* e = nrow(2 * y + 2);
                for (int x = 0; x < cw; ++x) row[size_t(x)] = 0.125f * a[x] + 0.375f * b[x] + 0.375f * cc[x] + 0.125f * e[x];
            } else {
                for (int x = 0; x < cw; ++x) {
                    float s = 0;
                    for (int i = 0; i < fy; ++i) s += nrow(y * fy + i)[x];
                    row[size_t(x)] = s / float(fy);
                }
            }
            store(c, y, row.data(), cw);
        }
    }
    return f;
}

void splitField(const Picture& frame, int parity, Picture& field) {
    const int lines = (frame.height - parity + 1) / 2;
    field.alloc(frame.width, lines, frame.gray);
    for (int c = 0; c < frame.planes(); ++c)
        for (int l = 0; l < lines; ++l)
            std::memcpy(field.p[c].row(l), frame.p[c].row(2 * l + parity), sizeof(float) * size_t(frame.width));
}

void weaveField(const Picture& field, int parity, Picture& frame) {
    for (int c = 0; c < frame.planes() && c < field.planes(); ++c)
        for (int l = 0; l < field.height && 2 * l + parity < frame.height; ++l)
            std::memcpy(frame.p[c].row(2 * l + parity), field.p[c].row(l), sizeof(float) * size_t(frame.width));
}

bool fieldLuma(const AVFrame* f, int parity, FieldLuma& out) {
    const AVPixFmtDescriptor* d = av_pix_fmt_desc_get(AVPixelFormat(f->format));
    if (!d || (d->flags & AV_PIX_FMT_FLAG_RGB)) return false;
    out.width = f->width;
    out.parity = parity;
    out.lines = (f->height - parity + 1) / 2;
    out.y.resize(size_t(out.width) * size_t(out.lines));
    const float scale = d->comp[0].depth > 8 ? 1.0f / float(1 << (d->comp[0].depth - 8)) : 1.0f;
    const bool full = f->color_range == AVCOL_RANGE_JPEG || f->format == AV_PIX_FMT_YUVJ422P || f->format == AV_PIX_FMT_YUVJ420P;
    const bool simple = simplePlanar(d);
    std::vector<uint16_t> line(size_t(out.width));
    const uint8_t* data[4] = {f->data[0], f->data[1], f->data[2], f->data[3]};
    for (int l = 0; l < out.lines; ++l) {
        const int y = 2 * l + parity;
        float* o = out.row(l);
        if (simple) {
            const uint8_t* src = f->data[0] + size_t(y) * size_t(f->linesize[0]);
            if (d->comp[0].depth <= 8)
                for (int x = 0; x < out.width; ++x) o[x] = float(src[x]);
            else {
                const uint16_t* s16 = reinterpret_cast<const uint16_t*>(src);
                for (int x = 0; x < out.width; ++x) o[x] = float(s16[x]) * scale;
            }
        } else {
            av_read_image_line2(line.data(), data, f->linesize, d, 0, y, 0, out.width, 0, 2);
            for (int x = 0; x < out.width; ++x) o[x] = float(line[size_t(x)]) * scale;
        }
        if (full)
            for (int x = 0; x < out.width; ++x) o[x] = 16.0f + o[x] * (219.0f / 255.0f);
    }
    return true;
}

void fieldLumaOf(const Picture& frame, int parity, FieldLuma& out) {
    out.width = frame.width;
    out.parity = parity;
    out.lines = (frame.height - parity + 1) / 2;
    out.y.resize(size_t(out.width) * size_t(out.lines));
    for (int l = 0; l < out.lines; ++l) std::memcpy(out.row(l), frame.p[0].row(2 * l + parity), sizeof(float) * size_t(out.width));
}

}  // namespace ud

// SPDX-License-Identifier: GPL-3.0-or-later
#include "Av.h"

#include "Types.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>

extern "C" {
#include <libavutil/error.h>
#include <libavutil/log.h>
}

namespace ud {

void quietFfmpegLog() {
    const char* v = std::getenv("UINDUB_FFMPEG_LOG");
    av_log_set_level(v && *v == '1' ? AV_LOG_INFO : AV_LOG_ERROR);
    // SVT-AV1 logs on its own; errors only unless asked.
    setenv("SVT_LOG", v && *v == '1' ? "4" : "2", 0);
}

std::string avError(int code) {
    char buf[AV_ERROR_MAX_STRING_SIZE] = {0};
    av_strerror(code, buf, sizeof buf);
    return buf;
}

bool frameInterlaced(const AVFrame* f) {
#ifdef AV_FRAME_FLAG_INTERLACED
    return (f->flags & AV_FRAME_FLAG_INTERLACED) != 0;
#else
    return f->interlaced_frame != 0;
#endif
}

bool frameTopFirst(const AVFrame* f) {
#ifdef AV_FRAME_FLAG_TOP_FIELD_FIRST
    return (f->flags & AV_FRAME_FLAG_TOP_FIELD_FIRST) != 0;
#else
    return f->top_field_first != 0;
#endif
}

void setFrameInterlace(AVFrame* f, bool interlaced, bool topFirst) {
#ifdef AV_FRAME_FLAG_INTERLACED
    f->flags &= ~(AV_FRAME_FLAG_INTERLACED | AV_FRAME_FLAG_TOP_FIELD_FIRST);
    if (interlaced) f->flags |= AV_FRAME_FLAG_INTERLACED;
    if (interlaced && topFirst) f->flags |= AV_FRAME_FLAG_TOP_FIELD_FIRST;
#else
    f->interlaced_frame = interlaced;
    f->top_field_first = interlaced && topFirst;
#endif
}

const char* fieldOrderName(FieldOrder f) {
    switch (f) {
    case FieldOrder::Progressive: return "progressive";
    case FieldOrder::TFF: return "TFF";
    case FieldOrder::BFF: return "BFF";
    default: return "unknown";
    }
}

FieldOrder fieldOrderFromName(const std::string& s) {
    if (s == "TFF" || s == "tff" || s == "tt") return FieldOrder::TFF;
    if (s == "BFF" || s == "bff" || s == "bb") return FieldOrder::BFF;
    if (s == "progressive") return FieldOrder::Progressive;
    return FieldOrder::Unknown;
}

std::string clockString(double sec) {
    if (sec < 0) sec = 0;
    const int h = int(sec / 3600), m = int(std::fmod(sec, 3600) / 60);
    char buf[32];
    std::snprintf(buf, sizeof buf, "%02d:%02d:%06.3f", h, m, std::fmod(sec, 60));
    return buf;
}

}  // namespace ud

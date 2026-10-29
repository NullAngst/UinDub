// SPDX-License-Identifier: GPL-3.0-or-later
#include "PreviewEngine.h"

#include "core/Color.h"
#include "core/Deinterlace.h"
#include "core/EdgeTracker.h"
#include "core/Qtgmc.h"

#include <algorithm>
#include <cmath>

namespace ud {

namespace {

constexpr size_t kCache = 10;

template <class M>
void trim(M& map, std::vector<int64_t>& order, int64_t keep) {
    order.erase(std::remove(order.begin(), order.end(), keep), order.end());
    order.push_back(keep);
    while (order.size() > kCache) {
        map.erase(order.front());
        order.erase(order.begin());
    }
}

void to8(const Picture& p, std::vector<uint8_t>& y, std::vector<uint8_t>& u, std::vector<uint8_t>& v) {
    const size_t n = size_t(p.width) * size_t(p.height);
    y.resize(n);
    u.resize(n);
    v.resize(n);
    for (size_t i = 0; i < n; ++i) y[i] = uint8_t(std::clamp(std::lround(p.p[0].d[i]), 0L, 255L));
    if (p.gray) {
        std::fill(u.begin(), u.end(), 128);
        std::fill(v.begin(), v.end(), 128);
        return;
    }
    for (size_t i = 0; i < n; ++i) {
        u[i] = uint8_t(std::clamp(std::lround(p.p[1].d[i]), 0L, 255L));
        v[i] = uint8_t(std::clamp(std::lround(p.p[2].d[i]), 0L, 255L));
    }
}

void scopesOf(const Picture& p, ScopeFrame& sf, std::shared_ptr<LumaPlane>& lp) {
    // Half size each way is plenty for the scopes and keeps them cheap.
    sf.width = p.width / 2;
    sf.height = p.height / 2;
    sf.cwidth = p.gray ? 0 : sf.width;
    sf.cheight = p.gray ? 0 : sf.height;
    sf.y.resize(size_t(sf.width) * size_t(sf.height));
    if (!p.gray) {
        sf.cb.resize(sf.y.size());
        sf.cr.resize(sf.y.size());
    } else {
        sf.cb.clear();
        sf.cr.clear();
    }
    for (int r = 0; r < sf.height; ++r)
        for (int x = 0; x < sf.width; ++x) {
            const size_t i = size_t(r) * size_t(sf.width) + size_t(x);
            const int sy = 2 * r, sx = 2 * x;
            sf.y[i] = uint8_t(std::clamp(std::lround(p.p[0].row(sy)[sx]), 0L, 255L));
            if (!p.gray) {
                sf.cb[i] = uint8_t(std::clamp(std::lround(p.p[1].row(sy)[sx]), 0L, 255L));
                sf.cr[i] = uint8_t(std::clamp(std::lround(p.p[2].row(sy)[sx]), 0L, 255L));
            }
        }
    sf.area = {0, sf.width, 0, sf.height};
    auto l = std::make_shared<LumaPlane>();
    l->width = p.width / 2;
    l->height = p.height;
    l->y.resize(size_t(l->width) * size_t(l->height));
    for (int r = 0; r < p.height; ++r)
        for (int x = 0; x < l->width; ++x) l->y[size_t(r) * size_t(l->width) + size_t(x)] = uint8_t(std::clamp(std::lround(p.p[0].row(r)[2 * x]), 0L, 255L));
    lp = l;
}

// One field, each line shown twice, so the picture keeps its shape.
Picture lineDoubled(const Picture& frame, int parity) {
    Picture o;
    o.alloc(frame.width, frame.height, frame.gray);
    for (int c = 0; c < frame.planes(); ++c)
        for (int y = 0; y < frame.height; ++y) {
            int src = (y & ~1) + parity;
            if (src >= frame.height) src -= 2;
            std::copy(frame.p[c].row(src), frame.p[c].row(src) + frame.width, o.p[c].row(y));
        }
    return o;
}

}  // namespace

PreviewEngine::PreviewEngine(QObject* parent) : QObject(parent) {
    qRegisterMetaType<ud::PreviewFramePtr>("ud::PreviewFramePtr");
    m_thread = std::thread([this] { run(); });
}

PreviewEngine::~PreviewEngine() {
    {
        QMutexLocker lk(&m_m);
        m_stop = true;
        m_cv.wakeAll();
    }
    m_thread.join();
}

void PreviewEngine::setSource(const SourceInfo& s) {
    QMutexLocker lk(&m_m);
    m_sourceIn = s;
    m_haveSource = true;
    m_dirty |= 0xFFFF;
}

void PreviewEngine::setProject(const Project& p, int changes) {
    QMutexLocker lk(&m_m);
    m_projectIn = p;
    m_dirty |= changes;
}

void PreviewEngine::clear() {
    QMutexLocker lk(&m_m);
    m_haveSource = false;
    m_pending = false;
    m_dirty |= 0xFFFF;
}

void PreviewEngine::request(int64_t frame, int field, bool fieldView, bool qtgmc) {
    QMutexLocker lk(&m_m);
    m_reqFrame = frame;
    m_reqField = field;
    m_reqFieldView = fieldView;
    m_reqQtgmc = qtgmc;
    m_pending = true;
    m_cv.wakeAll();
}

void PreviewEngine::run() {
    for (;;) {
        int64_t frame;
        int field, dirty;
        bool fieldView, qtgmc, have;
        {
            QMutexLocker lk(&m_m);
            while (!m_stop && !m_pending) m_cv.wait(&m_m);
            if (m_stop) return;
            m_pending = false;
            frame = m_reqFrame;
            field = m_reqField;
            fieldView = m_reqFieldView;
            qtgmc = m_reqQtgmc;
            dirty = m_dirty;
            m_dirty = 0;
            have = m_haveSource;
            if (dirty) {
                m_source = m_sourceIn;
                m_project = m_projectIn;
            }
        }
        if (!have) continue;
        if (dirty & 1) {
            m_reader = std::make_unique<SourceReader>();
            std::string err;
            if (!m_reader->open(m_source, err)) m_reader.reset();
            m_raw.clear();
            m_rawOrder.clear();
        }
        // Source (1), references (2), Edge Lock (4) or analysis (256) changes
        // invalidate the corrected frames.
        if (dirty & (1 | 2 | 4 | 256)) {
            std::string err;
            m_inputOk = m_input.open(m_project, m_source, err);
            m_corr.clear();
            m_corrOrder.clear();
            m_tracks = TrackStore{};
            if (m_project.analysisCurrent()) m_tracks.load(m_project.tracksPath(), err);
        }
        if (!m_reader) continue;
        build(frame, field, fieldView, qtgmc);
    }
}

bool PreviewEngine::raw(int64_t n, std::shared_ptr<Picture>& out) {
    auto it = m_raw.find(n);
    if (it != m_raw.end()) {
        out = it->second;
        trim(m_raw, m_rawOrder, n);
        return true;
    }
    FramePtr f = m_reader->read(n);
    if (!f) return false;
    auto p = std::make_shared<Picture>();
    if (!toPicture(f.get(), *p, true)) return false;
    m_raw[n] = p;
    trim(m_raw, m_rawOrder, n);
    out = p;
    return true;
}

bool PreviewEngine::corrected(int64_t n, std::shared_ptr<Picture>& out) {
    if (!m_inputOk || !m_input.edgeActive() || m_input.dontCorrect(n)) return raw(n, out);
    auto it = m_corr.find(n);
    if (it != m_corr.end()) {
        out = it->second;
        trim(m_corr, m_corrOrder, n);
        return true;
    }
    std::shared_ptr<Picture> r;
    if (!raw(n, r)) return false;
    FrameOffsets fo;
    auto p = std::make_shared<Picture>(*r);
    if (m_input.store().get(n, fo) && !fo.zero()) {
        ApplyParams ap;
        ap.strength = m_project.edge.strength;
        ap.fillRepeat = m_project.edge.fillRepeat;
        ap.xref = m_input.store().header().xref;
        applyOffsets(*p, fo, ap);
    }
    m_corr[n] = p;
    trim(m_corr, m_corrOrder, n);
    out = p;
    return true;
}

void PreviewEngine::build(int64_t n, int field, bool fieldView, bool qtgmc) {
    auto pf = std::make_shared<PreviewFrame>();
    pf->frame = n;
    pf->field = field;
    pf->fieldView = fieldView;
    pf->sar = m_source.sar;
    const FieldOrder order = m_project.renderFieldOrder();
    pf->parity = field == 0 ? firstParity(order) : 1 - firstParity(order);
    std::shared_ptr<Picture> src, cur;
    if (!raw(n, src) || !corrected(n, cur)) {
        pf->error = tr("Cannot decode frame %1").arg(n);
        emit ready(pf);
        return;
    }
    Picture shown, source;
    if (fieldView) {
        shown = lineDoubled(*cur, pf->parity);
        source = lineDoubled(*src, pf->parity);
    } else {
        source = *src;
        const DeintMode mode = m_project.deint.mode;
        if (mode == DeintMode::None) {
            shown = *cur;
        } else if (qtgmc && mode == DeintMode::Qtgmc) {
            // The real result on a paused frame: QTGMC over a short window around it.
            const int64_t a = std::max<int64_t>(0, n - 8), b = std::min<int64_t>(m_source.frames, n + 9);
            QtgmcConfig qc;
            qc.preset = m_project.deint.qtgmcPreset;
            qc.doubleRate = m_project.deint.doubleRate;
            qc.tff = order == FieldOrder::TFF;
            qc.width = m_source.width;
            qc.height = m_source.height;
            qc.gray = m_source.gray;
            qc.fps = m_source.fps;
            qc.sourceMatch = m_project.deint.sourceMatch;
            qc.sharpness = m_project.deint.sharpness;
            QtgmcRunner q;
            std::string err;
            const AVPixelFormat work = m_source.gray ? AV_PIX_FMT_GRAY16 : AV_PIX_FMT_YUV444P16;
            bool ok = q.open(qc, b - a, [&](int64_t i) -> FramePtr {
                std::shared_ptr<Picture> p;
                if (!corrected(a + i, p)) return nullptr;
                return fromPicture(*p, work, true);
            }, err);
            FramePtr out;
            if (ok) out = q.get(qc.doubleRate ? 2 * (n - a) + field : (n - a), err);
            if (out && toPicture(out.get(), shown, false)) {
                pf->qtgmc = true;
            } else {
                pf->error = tr("QTGMC preview: %1").arg(QString::fromStdString(err));
                shown = *cur;
            }
        } else {
            // bwdif keeps scrubbing live, whatever the render will use.
            std::shared_ptr<Picture> prev, next;
            if (n == 0 || !corrected(n - 1, prev)) prev = cur;
            if (n + 1 >= m_source.frames || !corrected(n + 1, next)) next = cur;
            FilterDeinterlacer d;
            std::string err;
            const AVPixelFormat work = m_source.gray ? AV_PIX_FMT_GRAY16 : AV_PIX_FMT_YUV444P16;
            if (d.open(DeintMode::Bwdif, true, order == FieldOrder::TFF, m_source.width, m_source.height, work, m_source.fps, m_source.sar, err)) {
                d.push(fromPicture(*prev, work, true), err);
                d.push(fromPicture(*cur, work, true), err);
                d.push(fromPicture(*next, work, true), err);
                d.push(nullptr, err);
                std::vector<FramePtr> outs;
                while (FramePtr o = d.pull()) outs.push_back(std::move(o));
                // Two outputs per input; the current frame's fields are 2 and 3.
                const size_t want = size_t(2 + field);
                if (outs.size() > want && toPicture(outs[want].get(), shown, false)) {
                } else {
                    shown = *cur;
                }
            } else {
                shown = *cur;
            }
        }
        ColorStats st;
        const ColorSettings& c = m_project.colorFor(n, m_source.chapters);
        if (!c.neutral()) applyColor(shown, c, &st);
        pf->clipped = st.clipped;
    }
    pf->width = shown.width;
    pf->height = shown.height;
    to8(shown, pf->y, pf->u, pf->v);
    to8(source, pf->sy, pf->su, pf->sv);
    scopesOf(shown, pf->scope, pf->luma);
    scopesOf(source, pf->scopeSource, pf->lumaSource);
    if (m_inputOk && m_input.store().isOpen()) pf->haveOffsets = m_input.store().get(n, pf->offsets);

    // Overlay: each reference measured on the raw field being shown.
    TrackerConfig tc;
    tc.s = m_project.edge;
    tc.refs = m_project.refs;
    tc.width = m_source.width;
    tc.frameHeight = m_source.height;
    FieldLuma fl;
    fieldLumaOf(*src, pf->parity, fl);
    const int64_t fieldIndex = 2 * n + field;
    for (size_t i = 0; i < m_project.refs.size() && i < size_t(kMaxReferences); ++i) {
        const Reference& r = m_project.refs[i];
        if (!r.enabled) continue;
        const RefSpan* sp = r.spanAt(n);
        if (!sp) continue;
        OverlayRef o;
        o.slot = int(i);
        o.kind = r.kind;
        double a = 0, b = 0;
        if (fieldIndex >= 0 && size_t(fieldIndex) < m_tracks.fields.size()) {
            const RefFieldTrack& t = m_tracks.fields[size_t(fieldIndex)].refs[i];
            o.state = t.state;
            a = t.a;
            b = t.b;
        } else {
            o.state = RefState::Active;
        }
        EdgeTracker::measureForOverlay(tc, int(i), *sp, fl, a, b, o.pos, o.conf);
        o.shape = sp->points;
        if (r.kind == RefKind::Drawn)
            for (PointF& p : o.shape) p.x += a + b * p.y;
        pf->overlay.push_back(std::move(o));
    }
    emit ready(pf);
}

}  // namespace ud

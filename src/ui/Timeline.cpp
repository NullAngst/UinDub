// SPDX-License-Identifier: GPL-3.0-or-later
#include "Timeline.h"

#include "shared/Theme.h"

#include <QContextMenuEvent>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>

namespace ud {

namespace {
constexpr int kChapters = 16, kEvents = 10, kRuler = 24, kCorr = 24, kLane = 14, kBand = 5;
constexpr int yEvents = kChapters, yRuler = yEvents + kEvents, yCorr = yRuler + kRuler, yLanes = yCorr + kCorr;
constexpr int kSnapPx = 8;
}  // namespace

Timeline::Timeline(Session* s, QWidget* parent) : QWidget(parent), m_s(s) {
    setMouseTracking(true);
    setMinimumHeight(yLanes + kLane + kBand);
    setToolTip(tr("Click or drag to move the playhead. Shift-drag selects a span; right-click for what to do with it. Ctrl+wheel zooms, "
                  "wheel scrolls."));
    connect(s, &Session::opened, this, [this] {
        fitView();
        update();
    });
    connect(s, &Session::closed, this, [this] { update(); });
    connect(s, &Session::projectChanged, this, [this](int) {
        m_sumA = -1;
        updateGeometry();
        update();
    });
    connect(s, &Session::positionChanged, this, [this](qint64 f, int) {
        ensureVisible(f);
        update();
    });
}

QSize Timeline::sizeHint() const {
    const int refs = m_s->isOpen() ? std::max<int>(1, int(m_s->project().refs.size())) : 1;
    return {800, yLanes + refs * kLane + kBand};
}

void Timeline::setRefColors(const std::vector<QColor>& c) {
    m_colors = c;
    update();
}

void Timeline::setProgress(double fraction, bool active) {
    m_progress = fraction;
    m_rendering = active;
    update();
}

void Timeline::clearSelection() {
    m_selA = m_selB = 0;
    update();
}

void Timeline::setCutStart(int64_t f) {
    m_cutStart = f;
    update();
}

void Timeline::fitView() {
    m_viewA = 0;
    m_viewB = std::max<int64_t>(1, m_s->frames());
    m_sumA = -1;
}

void Timeline::ensureVisible(int64_t f) {
    const int64_t span = m_viewB - m_viewA;
    if (f < m_viewA || f >= m_viewB) {
        m_viewA = std::clamp<int64_t>(f - span / 4, 0, std::max<int64_t>(0, m_s->frames() - span));
        m_viewB = m_viewA + span;
        m_sumA = -1;
    }
}

void Timeline::resizeEvent(QResizeEvent*) { m_sumA = -1; }

double Timeline::xOf(int64_t frame) const {
    const double w = std::max(1, width());
    return double(frame - m_viewA) / double(std::max<int64_t>(1, m_viewB - m_viewA)) * w;
}

int64_t Timeline::frameAt(double x) const {
    const double w = std::max(1, width());
    const int64_t f = m_viewA + int64_t(std::floor(x / w * double(m_viewB - m_viewA)));
    return std::clamp<int64_t>(f, 0, std::max<int64_t>(0, m_s->frames() - 1));
}

int Timeline::laneAt(double y) const {
    if (y < yLanes) return -1;
    const int i = int((y - yLanes) / kLane);
    return m_s->isOpen() && i < int(m_s->project().refs.size()) ? i : -1;
}

int64_t Timeline::snap(int64_t f) const {
    if (!m_s->isOpen()) return f;
    // PingDub's recording boundaries are where a cut usually belongs.
    std::vector<int64_t> marks;
    for (const Chapter& c : m_s->source().chapters) marks.push_back(c.start);
    for (const SourceEvent& e : m_s->source().events)
        if (e.kind == SourceEvent::Kind::SignalLost) {
            marks.push_back(e.frame);
            marks.push_back(e.endFrame + 1);
        }
    int64_t best = f;
    double bestD = kSnapPx + 1;
    for (int64_t m : marks) {
        const double d = std::fabs(xOf(m) - xOf(f));
        if (d < bestD) {
            bestD = d;
            best = m;
        }
    }
    return best;
}

void Timeline::buildSummary() {
    const TrackStore& t = m_s->tracks();
    const int w = std::max(1, width());
    if (m_sumA == m_viewA && m_sumB == m_viewB && m_sumW == w && m_sumTracks == t.fields.size() && m_sumTrackPtr == t.fields.data()) return;
    m_sumA = m_viewA;
    m_sumB = m_viewB;
    m_sumW = w;
    m_sumTracks = t.fields.size();
    m_sumTrackPtr = t.fields.data();
    m_colMean.assign(size_t(w), 0.0f);
    m_colState.assign(size_t(w), {});
    if (t.fields.empty()) return;
    for (int x = 0; x < w; ++x) {
        const int64_t a = frameAt(x), b = std::max(a + 1, frameAt(x + 1));
        float mx = 0;
        std::array<uint8_t, kMaxReferences> st{};
        for (int64_t f = a; f < b; ++f)
            for (int k = 0; k < 2; ++k) {
                const size_t i = size_t(2 * f + k);
                if (i >= t.fields.size()) break;
                const FieldTrack& ft = t.fields[i];
                mx = std::max(mx, ft.meanAbs);
                for (int r = 0; r < kMaxReferences; ++r) {
                    // Worst state wins: Lost over Dark over Ended over Active.
                    const uint8_t s = uint8_t(ft.refs[r].state);
                    auto rank = [](uint8_t v) {
                        switch (RefState(v)) {
                        case RefState::Lost: return 4;
                        case RefState::Dark: return 3;
                        case RefState::Ended: return 2;
                        case RefState::Active: return 1;
                        default: return 0;
                        }
                    };
                    if (rank(s) > rank(st[size_t(r)])) st[size_t(r)] = s;
                }
            }
        m_colMean[size_t(x)] = mx;
        m_colState[size_t(x)] = st;
    }
}

void Timeline::paintEvent(QPaintEvent*) {
    QPainter p(this);
    const QPalette pal = palette();
    p.fillRect(rect(), pal.color(QPalette::Base));
    if (!m_s->isOpen() || m_s->frames() <= 0) {
        p.setPen(pal.color(QPalette::PlaceholderText));
        p.drawText(rect(), Qt::AlignCenter, tr("No source"));
        return;
    }
    const Project& pr = m_s->project();
    const SourceInfo& src = m_s->source();
    const int W = width();
    const QColor text = pal.color(QPalette::WindowText), muted = pal.color(QPalette::PlaceholderText);
    const int lanes = std::max<int>(1, int(pr.refs.size()));
    const int bottom = yLanes + lanes * kLane;

    // Ruler background, trimmed parts dimmed.
    p.fillRect(QRect(0, yRuler, W, kRuler), pal.color(QPalette::AlternateBase));
    const int64_t in = std::clamp<int64_t>(pr.edit.in, 0, src.frames), out = pr.edit.out < 0 ? src.frames : std::clamp<int64_t>(pr.edit.out, 0, src.frames);
    QColor dim(0, 0, 0, 90);
    if (in > m_viewA) p.fillRect(QRectF(0, yRuler, xOf(in), bottom - yRuler), dim);
    if (out < m_viewB) p.fillRect(QRectF(xOf(out), yRuler, W - xOf(out), bottom - yRuler), dim);
    // Ticks and time labels.
    const double fps = src.fps.toDouble();
    const double secs = double(m_viewB - m_viewA) / fps;
    double step = 1;
    for (double s : {1.0, 2.0, 5.0, 10.0, 30.0, 60.0, 120.0, 300.0, 600.0, 1800.0, 3600.0})
        if (secs / s < W / 90.0) {
            step = s;
            break;
        }
    p.setPen(muted);
    for (double t = std::ceil(m_viewA / fps / step) * step; t * fps < m_viewB; t += step) {
        const double x = xOf(int64_t(std::llround(t * fps)));
        p.drawLine(QPointF(x, yRuler + kRuler - 6), QPointF(x, yRuler + kRuler));
        p.drawText(QRectF(x + 3, yRuler, 90, kRuler - 6), Qt::AlignLeft | Qt::AlignVCenter, QString::fromStdString(clockString(t)).left(8));
    }
    // In and out brackets.
    p.setPen(QPen(pal.color(QPalette::Highlight), 2));
    if (in >= m_viewA && in <= m_viewB) {
        const double x = xOf(in);
        p.drawLine(QPointF(x, yRuler), QPointF(x, bottom));
        p.drawLine(QPointF(x, yRuler), QPointF(x + 6, yRuler));
    }
    if (out >= m_viewA && out <= m_viewB) {
        const double x = xOf(out);
        p.drawLine(QPointF(x, yRuler), QPointF(x, bottom));
        p.drawLine(QPointF(x, yRuler), QPointF(x - 6, yRuler));
    }

    // Chapters along the top.
    for (size_t i = 0; i < src.chapters.size(); ++i) {
        const Chapter& c = src.chapters[i];
        if (c.end < m_viewA || c.start > m_viewB) continue;
        const double x = xOf(c.start);
        p.setPen(QPen(pal.color(QPalette::Link), 1));
        p.drawLine(QPointF(x, 0), QPointF(x, kChapters));
        p.setPen(text);
        p.drawText(QRectF(x + 3, 0, std::max(10.0, xOf(c.end) - x - 4), kChapters), Qt::AlignLeft | Qt::AlignVCenter,
                   QString::fromStdString(c.title));
    }
    // PingDub's events as ticks.
    for (const SourceEvent& e : src.events) {
        if (e.endFrame < m_viewA || e.frame > m_viewB) continue;
        QColor c = e.kind == SourceEvent::Kind::Insert ? StatusColor::Warn : StatusColor::Bad;
        if (e.kind == SourceEvent::Kind::Damaged) c = QColor(0xB0, 0x7C, 0xFF);
        const double x0 = xOf(e.frame), x1 = std::max(x0 + 2, xOf(e.endFrame + 1));
        p.fillRect(QRectF(x0, yEvents + (e.kind == SourceEvent::Kind::SignalLost ? 0 : 3), x1 - x0, kEvents - 3), c);
    }

    // Correction lane: average correction per frame.
    buildSummary();
    p.fillRect(QRect(0, yCorr, W, kCorr), QColor(0x1C, 0x1C, 0x1C));
    float peak = 1.0f;
    for (float v : m_colMean) peak = std::max(peak, v);
    p.setPen(Qt::NoPen);
    for (int x = 0; x < W && x < int(m_colMean.size()); ++x) {
        const double h = m_colMean[size_t(x)] / peak * (kCorr - 4);
        if (h <= 0) continue;
        p.fillRect(QRectF(x, yCorr + kCorr - 2 - h, 1, h), StatusColor::Ok);
    }
    p.setPen(muted);
    p.drawText(QRect(4, yCorr, 200, kCorr), Qt::AlignLeft | Qt::AlignVCenter,
               m_s->analyzed() ? tr("correction (peak %1 px)").arg(double(peak), 0, 'f', 1) : tr("not analyzed"));
    for (int64_t c : pr.detectedCuts) {
        if (c < m_viewA || c > m_viewB) continue;
        p.setPen(QPen(QColor(255, 255, 255, 160), 1, Qt::DashLine));
        p.drawLine(QPointF(xOf(c), yCorr), QPointF(xOf(c), bottom));
    }

    // One lane per reference: spans, Lost shading.
    for (int i = 0; i < lanes; ++i) {
        const int y = yLanes + i * kLane;
        p.fillRect(QRect(0, y, W, kLane - 1), pal.color(QPalette::AlternateBase));
        if (i >= int(pr.refs.size())) {
            p.setPen(muted);
            p.drawText(QRect(4, y, W, kLane), Qt::AlignLeft | Qt::AlignVCenter, tr("no references"));
            continue;
        }
        const Reference& r = pr.refs[size_t(i)];
        QColor c = i < int(m_colors.size()) ? m_colors[size_t(i)] : QColor(Qt::gray);
        if (!r.enabled) c.setAlpha(70);
        for (const RefSpan& s : r.spans) {
            if (s.end < m_viewA || s.start > m_viewB) continue;
            const double x0 = xOf(s.start), x1 = xOf(s.end);
            p.fillRect(QRectF(x0, y + 3, std::max(2.0, x1 - x0), kLane - 7), c);
        }
        for (int x = 0; x < W && x < int(m_colState.size()); ++x) {
            const RefState st = RefState(m_colState[size_t(x)][size_t(i)]);
            if (st == RefState::Lost) p.fillRect(QRectF(x, y, 1, kLane - 1), QColor(0xF8, 0x51, 0x49, 170));
            else if (st == RefState::Dark) p.fillRect(QRectF(x, y, 1, kLane - 1), QColor(0x80, 0x80, 0x80, 150));
            else if (st == RefState::Ended) p.fillRect(QRectF(x, y + kLane / 2, 1, 1), QColor(255, 255, 255, 120));
        }
        p.setPen(text);
        p.drawText(QRect(4, y, W, kLane), Qt::AlignLeft | Qt::AlignVCenter,
                   QString::fromStdString(r.name.empty() ? std::string("Reference ") + std::to_string(i + 1) : r.name));
    }

    // Overrides and cut spans over everything below the ruler.
    for (const EdgeOverride& o : pr.overrides) {
        if (o.end < m_viewA || o.start > m_viewB) continue;
        const QRectF rr(xOf(o.start), yCorr, xOf(o.end) - xOf(o.start), bottom - yCorr);
        if (o.kind == EdgeOverride::Kind::DontCorrect) {
            p.fillRect(rr, QBrush(QColor(0x90, 0x90, 0x90, 120), Qt::BDiagPattern));
        } else {
            p.setPen(QPen(pal.color(QPalette::Highlight), 1));
            p.drawRect(rr);
        }
    }
    for (const CutSpan& c : pr.edit.cuts) {
        if (c.end < m_viewA || c.start > m_viewB) continue;
        const QRectF rr(xOf(c.start), yRuler, std::max(2.0, xOf(c.end) - xOf(c.start)), bottom - yRuler);
        p.fillRect(rr, QColor(0, 0, 0, 110));
        p.fillRect(rr, QBrush(QColor(0xF8, 0x51, 0x49, 200), Qt::FDiagPattern));
    }
    if (m_cutStart >= 0) {
        const double x = xOf(m_cutStart);
        p.setPen(QPen(StatusColor::Bad, 2, Qt::DashLine));
        p.drawLine(QPointF(x, yRuler), QPointF(x, bottom));
    }
    if (m_selB > m_selA) p.fillRect(QRectF(xOf(m_selA), yRuler, xOf(m_selB) - xOf(m_selA), bottom - yRuler), QColor(0x5B, 0x9B, 0xFF, 70));

    // Render progress band.
    if (m_rendering) {
        p.fillRect(QRect(0, height() - kBand, W, kBand), QColor(0x30, 0x30, 0x30));
        p.fillRect(QRectF(0, height() - kBand, W * std::clamp(m_progress, 0.0, 1.0), kBand), pal.color(QPalette::Highlight));
    }
    // Playhead.
    const double px = xOf(m_s->frame());
    p.setPen(QPen(QColor(0xFF, 0xFF, 0xFF), 1));
    p.drawLine(QPointF(px, yEvents), QPointF(px, bottom));
    p.setBrush(QColor(0xFF, 0xFF, 0xFF));
    p.drawPolygon(QPolygonF({QPointF(px - 5, yRuler), QPointF(px + 5, yRuler), QPointF(px, yRuler + 6)}));
}

void Timeline::mousePressEvent(QMouseEvent* e) {
    if (!m_s->isOpen() || e->button() != Qt::LeftButton) return;
    const int64_t f = frameAt(e->position().x());
    if (e->modifiers() & Qt::ShiftModifier) {
        m_selecting = true;
        m_selAnchor = (e->modifiers() & Qt::AltModifier) ? f : snap(f);
        m_selA = m_selB = m_selAnchor;
        update();
        return;
    }
    m_dragging = true;
    m_s->seek(f, 0);
}

void Timeline::mouseMoveEvent(QMouseEvent* e) {
    if (!m_s->isOpen()) return;
    const int64_t f = frameAt(e->position().x());
    if (m_selecting) {
        const int64_t g = (e->modifiers() & Qt::AltModifier) ? f : snap(f);
        m_selA = std::min(m_selAnchor, g);
        m_selB = std::max(m_selAnchor, g);
        update();
        return;
    }
    if (m_dragging) {
        m_s->seek(f, 0);
        return;
    }
    // Hover tip: time and what is there.
    QString tip = QString::fromStdString(clockString(frameSeconds(f, m_s->source().fps))) + tr("  frame %1").arg(f);
    for (const SourceEvent& ev : m_s->source().events)
        if (f >= ev.frame && f <= ev.endFrame) tip += "\n" + QString::fromStdString(ev.name + " " + ev.detail);
    setToolTip(tip);
}

void Timeline::mouseReleaseEvent(QMouseEvent*) {
    m_dragging = false;
    m_selecting = false;
}

void Timeline::wheelEvent(QWheelEvent* e) {
    if (!m_s->isOpen()) return;
    const int64_t total = std::max<int64_t>(1, m_s->frames());
    const int64_t span = m_viewB - m_viewA;
    if (e->modifiers() & Qt::ControlModifier) {
        const int64_t at = frameAt(e->position().x());
        const double k = e->angleDelta().y() > 0 ? 0.7 : 1.4;
        int64_t ns = std::clamp<int64_t>(int64_t(std::llround(double(span) * k)), 30, total);
        const double t = double(at - m_viewA) / double(std::max<int64_t>(1, span));
        m_viewA = std::clamp<int64_t>(at - int64_t(t * ns), 0, total - ns);
        m_viewB = m_viewA + ns;
    } else {
        const int64_t d = std::max<int64_t>(1, span / 10) * (e->angleDelta().y() > 0 ? -1 : 1);
        m_viewA = std::clamp<int64_t>(m_viewA + d, 0, total - span);
        m_viewB = m_viewA + span;
    }
    m_sumA = -1;
    update();
}

void Timeline::contextMenuEvent(QContextMenuEvent* e) {
    if (!m_s->isOpen()) return;
    const int64_t f = frameAt(e->pos().x());
    const Project& pr = m_s->project();
    QMenu m(this);
    if (m_selB > m_selA) {
        const int64_t a = m_selA, b = m_selB;
        m.addAction(tr("Cut this span out"), this, [this, a, b] { emit cutRequested(a, b); });
        m.addAction(tr("Set in and out to this span"), this, [this, a, b] { emit setInOutRequested(a, b); });
        m.addSeparator();
        m.addAction(tr("Edge Lock: don't correct this span"), this, [this, a, b] { emit dontCorrectRequested(a, b); });
        m.addAction(tr("Edge Lock: re-analyze this span with the current settings"), this, [this, a, b] { emit reanalyzeRequested(a, b); });
        m.addSeparator();
        m.addAction(tr("Clear selection"), this, [this] { clearSelection(); });
    }
    for (const CutSpan& c : pr.edit.cuts)
        if (f >= c.start && f < c.end) m.addAction(tr("Remove this cut"), this, [this, f] { emit deleteCutRequested(f); });
    for (const EdgeOverride& o : pr.overrides)
        if (f >= o.start && f < o.end) m.addAction(tr("Remove this Edge Lock override"), this, [this, f] { emit deleteOverrideRequested(f); });
    const int lane = laneAt(e->pos().y());
    if (lane >= 0 && lane < int(pr.refs.size()) && pr.refs[size_t(lane)].kind == RefKind::Drawn) {
        m.addSeparator();
        m.addAction(tr("Carry this reference forward to here"), this, [this, lane, f] { emit carryForwardRequested(lane, f); });
        if (pr.refs[size_t(lane)].spanAt(f)) m.addAction(tr("End this reference's span here"), this, [this, lane, f] { emit endSpanRequested(lane, f); });
    }
    if (m.isEmpty()) m.addAction(tr("Shift-drag to select a span first"))->setEnabled(false);
    m.exec(e->globalPos());
}

}  // namespace ud

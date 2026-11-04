// SPDX-License-Identifier: GPL-3.0-or-later
#include "Transport.h"

#include "Session.h"
#include "Timeline.h"
#include "core/Edit.h"

#include <QFrame>
#include <QHBoxLayout>
#include <QStyle>

#include <algorithm>
#include <cmath>

namespace ud {

namespace {

constexpr int kZoomSteps = 1000;

QFrame* separator(QWidget* parent) {
    auto* f = new QFrame(parent);
    f->setFrameShape(QFrame::VLine);
    f->setFrameShadow(QFrame::Sunken);
    return f;
}

QString hms(double s) {
    const long long t = std::max(0LL, (long long)std::floor(s + 1e-6));
    return QString("%1:%2:%3").arg(t / 3600).arg((t / 60) % 60, 2, 10, QChar('0')).arg(t % 60, 2, 10, QChar('0'));
}

}  // namespace

TransportBar::TransportBar(Session* s, Timeline* t, QWidget* parent) : QWidget(parent), m_s(s), m_t(t) {
    auto* l = new QHBoxLayout(this);
    l->setContentsMargins(0, 2, 0, 2);
    l->setSpacing(2);

    l->addWidget(button("go_start", QString(), tr("Go to the start"), QStyle::SP_MediaSkipBackward));
    l->addWidget(button("prev_frame", QString(), tr("Back one frame"), QStyle::SP_MediaSeekBackward));
    m_play = button("play", QString(), tr("Play / pause"), QStyle::SP_MediaPlay);
    l->addWidget(m_play);
    l->addWidget(button("next_frame", QString(), tr("Forward one frame"), QStyle::SP_MediaSeekForward));
    l->addWidget(button("go_end", QString(), tr("Go to the end"), QStyle::SP_MediaSkipForward));
    l->addWidget(separator(this));

    l->addWidget(button("mark_in", tr("In"), tr("Set the in point at the playhead")));
    l->addWidget(button("mark_out", tr("Out"), tr("Set the out point after the playhead frame")));
    l->addWidget(button("go_in", tr("Go In"), tr("Jump to the in point")));
    l->addWidget(button("go_out", tr("Go Out"), tr("Jump to the last kept frame before the out point")));
    l->addWidget(button("clear_trim", tr("Clear"), tr("Clear the trim: in at the start, out at the end")));
    l->addWidget(separator(this));

    l->addWidget(button("mark_cut", tr("Cut"), tr("Start a cut at the playhead; press again at its end")));
    l->addWidget(button("cut_selection", tr("Cut Sel"), tr("Cut out the span selected with Shift-drag")));
    l->addWidget(button("delete_cut", tr("Uncut"), tr("Remove the cut under the playhead")));
    l->addStretch(1);

    // Lives in the status bar; the bar itself stays narrow enough for small screens.
    m_trim = new QLabel;
    m_trim->setTextInteractionFlags(Qt::TextSelectableByMouse);

    l->addWidget(button("zoom_out", tr("-"), tr("Zoom the timeline out")));
    m_zoom = new QSlider(Qt::Horizontal, this);
    m_zoom->setRange(0, kZoomSteps);
    m_zoom->setFixedWidth(100);
    m_zoom->setToolTip(tr("Timeline zoom. Ctrl+wheel on the timeline does the same around the pointer."));
    l->addWidget(m_zoom);
    l->addWidget(button("zoom_in", tr("+"), tr("Zoom the timeline in")));
    l->addWidget(button("zoom_fit", tr("Fit"), tr("Show the whole clip")));
    l->addWidget(button("zoom_trim", tr("Trim"), tr("Zoom to the in and out points")));

    m_scroll = new QScrollBar(Qt::Horizontal);
    m_scroll->setVisible(false);

    connect(m_zoom, &QSlider::valueChanged, this, [this](int v) {
        if (m_syncing || !m_s->isOpen()) return;
        const double total = double(std::max<int64_t>(1, m_s->frames()));
        const double mn = std::min<double>(double(Timeline::kMinSpan), total);
        const double span = total * std::pow(mn / total, double(v) / kZoomSteps);
        m_t->setViewSpan(int64_t(std::llround(span)));
    });
    connect(m_scroll, &QScrollBar::valueChanged, this, [this](int v) {
        if (!m_syncing) m_t->setViewStart(v);
    });
    connect(m_t, &Timeline::viewChanged, this, &TransportBar::updateZoom);
    connect(s, &Session::opened, this, &TransportBar::updateTrim);
    connect(s, &Session::closed, this, &TransportBar::updateTrim);
    connect(s, &Session::projectChanged, this, [this](int w) {
        if (w & (ChEdit | ChSource)) updateTrim();
    });
    updateTrim();
}

QToolButton* TransportBar::button(const QString& id, const QString& text, const QString& tip, QStyle::StandardPixmap icon) {
    auto* b = new QToolButton(this);
    if (icon != QStyle::SP_CustomBase) b->setIcon(style()->standardIcon(icon));
    else b->setText(text);
    b->setAutoRaise(true);
    b->setToolTip(tip);
    b->setFocusPolicy(Qt::NoFocus);  // keys keep going to the window
    m_buttons[id] = b;
    m_tips[id] = tip;
    connect(b, &QToolButton::clicked, this, [this, id] { emit action(id); });
    return b;
}

void TransportBar::setKeyHints(const QHash<QString, QString>& keys) {
    for (auto it = m_buttons.begin(); it != m_buttons.end(); ++it) {
        const QString k = keys.value(it.key());
        it.value()->setToolTip(k.isEmpty() ? m_tips.value(it.key()) : tr("%1 (%2)").arg(m_tips.value(it.key()), k));
    }
}

void TransportBar::setPlaying(bool playing) {
    m_play->setIcon(style()->standardIcon(playing ? QStyle::SP_MediaPause : QStyle::SP_MediaPlay));
}

void TransportBar::updateTrim() {
    const bool open = m_s->isOpen();
    for (QToolButton* b : m_buttons) b->setEnabled(open);
    m_zoom->setEnabled(open);
    if (!open) {
        m_trim->clear();
        m_scroll->setVisible(false);
        return;
    }
    const SourceInfo& src = m_s->source();
    const Project& p = m_s->project();
    const EditList el = EditList::build(src.frames, p.edit);
    const int64_t in = std::clamp<int64_t>(p.edit.in, 0, src.frames);
    const int64_t out = p.edit.out < 0 ? src.frames : std::clamp<int64_t>(p.edit.out, 0, src.frames);
    QString t = tr("In %1  Out %2  Kept %3 of %4").arg(hms(frameSeconds(in, src.fps)), hms(frameSeconds(out, src.fps)),
                                                      hms(frameSeconds(el.outputFrames(), src.fps)), hms(frameSeconds(src.frames, src.fps)));
    if (!p.edit.cuts.empty()) t += tr("  (%n cut(s))", nullptr, int(p.edit.cuts.size()));
    m_trim->setText(t);
}

void TransportBar::updateZoom(qint64 a, qint64 b, qint64 total) {
    m_syncing = true;
    const double span = double(std::max<qint64>(1, b - a));
    const double tot = double(std::max<qint64>(1, total));
    const double mn = std::min<double>(double(Timeline::kMinSpan), tot);
    const int v = mn >= tot ? 0 : int(std::lround(kZoomSteps * std::log(span / tot) / std::log(mn / tot)));
    m_zoom->setValue(std::clamp(v, 0, kZoomSteps));
    const qint64 room = std::max<qint64>(0, total - (b - a));
    m_scroll->setRange(0, int(room));
    m_scroll->setPageStep(int(std::max<qint64>(1, b - a)));
    m_scroll->setSingleStep(int(std::max<qint64>(1, (b - a) / 20)));
    m_scroll->setValue(int(a));
    m_scroll->setVisible(room > 0);
    m_syncing = false;
}

}  // namespace ud

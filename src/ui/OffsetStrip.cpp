// SPDX-License-Identifier: GPL-3.0-or-later
#include "OffsetStrip.h"

#include "core/Warble.h"
#include "shared/Theme.h"

#include <QPainter>

#include <cmath>

namespace ud {

OffsetStrip::OffsetStrip(QWidget* parent) : QWidget(parent) {
    setToolTip(tr("Per-line correction for the field on screen, line 1 at the top. Green measured, amber interpolated, gray easing "
                  "out. After a warble test the white dotted line is the true answer."));
}

void OffsetStrip::showFrame(PreviewFramePtr f, double strength) {
    m_frame = std::move(f);
    m_strength = strength;
    update();
}

void OffsetStrip::setTruth(bool on, const DamageModel& d, FieldOrder order) {
    m_truth = on;
    m_damage = d;
    m_order = order;
    update();
}

void OffsetStrip::paintEvent(QPaintEvent*) {
    QPainter p(this);
    const QRect area = rect().adjusted(2, 18, -2, -18);
    p.fillRect(rect(), palette().color(QPalette::Base));
    p.fillRect(area, QColor(0x18, 0x18, 0x18));
    p.setPen(palette().color(QPalette::PlaceholderText));
    if (!m_frame || !m_frame->haveOffsets) {
        p.drawText(area, Qt::AlignCenter | Qt::TextWordWrap, tr("No offsets yet. Set a reference and Analyze (F5)."));
        return;
    }
    const FrameOffsets& fo = m_frame->offsets;
    const int parity = m_frame->parity;
    const int lines = (int(fo.off.size()) - parity + 1) / 2;
    // Scale: the largest correction in this field, at least 2 px.
    double mx = 2.0;
    for (int l = 0; l < lines; ++l) mx = std::max(mx, std::fabs(fo.offsetPx(2 * l + parity)));
    if (m_truth)
        for (int l = 0; l < lines; ++l) mx = std::max(mx, std::fabs(warbleTruth(m_damage, m_order, m_frame->frame, 2 * l + parity)));
    mx = std::ceil(mx);
    const double cx = area.center().x();
    auto xOf = [&](double v) { return cx + v / mx * (area.width() / 2.0 - 2); };
    auto yOf = [&](int l) { return area.top() + (l + 0.5) * area.height() / double(std::max(1, lines)); };
    p.setPen(QPen(QColor(0x60, 0x60, 0x60), 1));
    p.drawLine(QPointF(cx, area.top()), QPointF(cx, area.bottom()));
    for (int l = 0; l < lines; ++l) {
        const int y = 2 * l + parity;
        const double v = fo.offsetPx(y) * m_strength;
        const uint8_t c = fo.conf[size_t(y)];
        QColor col = c >= kConfMeasuredMin ? StatusColor::Ok : (c == kConfInterpolated ? StatusColor::Warn : QColor(0x90, 0x90, 0x90));
        if (c == kConfNone && v == 0) continue;
        p.setPen(QPen(col, 1));
        p.drawLine(QPointF(cx, yOf(l)), QPointF(xOf(v), yOf(l)));
    }
    if (m_truth) {
        p.setPen(QPen(QColor(255, 255, 255), 1, Qt::DotLine));
        QPolygonF poly;
        for (int l = 0; l < lines; ++l) poly << QPointF(xOf(warbleTruth(m_damage, m_order, m_frame->frame, 2 * l + parity)), yOf(l));
        p.drawPolyline(poly);
    }
    p.setPen(palette().color(QPalette::WindowText));
    p.drawText(QRect(0, 0, width(), 16), Qt::AlignCenter, tr("field %1 (%2)").arg(m_frame->field + 1).arg(parity ? tr("bottom") : tr("top")));
    p.setPen(palette().color(QPalette::PlaceholderText));
    p.drawText(QRect(0, height() - 16, width(), 16), Qt::AlignLeft, QString("-%1").arg(mx));
    p.drawText(QRect(0, height() - 16, width(), 16), Qt::AlignCenter, tr("px"));
    p.drawText(QRect(0, height() - 16, width(), 16), Qt::AlignRight, QString("+%1").arg(mx));
}

}  // namespace ud

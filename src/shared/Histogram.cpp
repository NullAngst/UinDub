// SPDX-License-Identifier: GPL-3.0-or-later
// Copied from PingDub (github.com/NullAngst/PingDub, same author, GPL-3.0-or-later).
#include "Histogram.h"

#include "shared/Theme.h"

#include <QPainter>

#include <cmath>

namespace ud {

Histogram::Histogram(QWidget* parent) : QWidget(parent) {
    setMinimumHeight(70);
    setToolTip(tr("Luma histogram. Lines at 16 (black) and 235 (white); the numbers are the share of pixels below and "
                  "above them."));
}

void Histogram::setLuma(std::shared_ptr<const LumaPlane> luma) {
    if (!luma || luma->y.empty()) return;
    m_hist.fill(0);
    m_total = m_below = m_above = 0;
    const int w = luma->width, h = luma->height;
    for (int r = 0; r < h; r += 2) {
        const uint8_t* row = luma->y.data() + size_t(r) * w;
        for (int x = 0; x < w; ++x) ++m_hist[row[x]];
    }
    for (int v = 0; v < 256; ++v) {
        m_total += m_hist[size_t(v)];
        if (v < 16) m_below += m_hist[size_t(v)];
        if (v > 235) m_above += m_hist[size_t(v)];
    }
    update();
}

void Histogram::paintEvent(QPaintEvent*) {
    QPainter p(this);
    const QPalette pal = palette();
    const QRect area = rect().adjusted(2, 2, -2, -16);
    p.fillRect(area, QColor(0x20, 0x20, 0x20));
    auto xOf = [&](int v) { return area.left() + v * area.width() / 256; };
    if (m_total) {
        uint32_t peak = 1;
        for (int v = 1; v < 255; ++v) peak = std::max(peak, m_hist[size_t(v)]);
        const double lp = std::log1p(double(peak));
        p.setPen(Qt::NoPen);
        for (int v = 0; v < 256; ++v) {
            const double hgt = std::log1p(double(m_hist[size_t(v)])) / lp;
            const int bh = int(std::min(1.0, hgt) * area.height());
            QColor c = (v < 16 || v > 235) ? StatusColor::Bad : QColor(0xC8, 0xC8, 0xC8);
            p.fillRect(QRect(xOf(v), area.bottom() - bh, std::max(1, xOf(v + 1) - xOf(v)), bh), c);
        }
        p.setPen(QPen(StatusColor::Warn, 1, Qt::DashLine));
        p.drawLine(xOf(16), area.top(), xOf(16), area.bottom());
        p.drawLine(xOf(235), area.top(), xOf(235), area.bottom());
    }
    p.setPen(pal.color(QPalette::PlaceholderText));
    const double below = m_total ? 100.0 * double(m_below) / double(m_total) : 0;
    const double above = m_total ? 100.0 * double(m_above) / double(m_total) : 0;
    const QRect txt(2, height() - 15, width() - 4, 14);
    p.setPen(below > 0.5 ? StatusColor::Bad : pal.color(QPalette::PlaceholderText));
    p.drawText(txt, Qt::AlignLeft | Qt::AlignVCenter, tr("below 16: %1 (%2%)").arg(m_below * 4).arg(below, 0, 'f', 2));
    p.setPen(above > 0.5 ? StatusColor::Bad : pal.color(QPalette::PlaceholderText));
    p.drawText(txt, Qt::AlignRight | Qt::AlignVCenter, tr("above 235: %1 (%2%)").arg(m_above * 4).arg(above, 0, 'f', 2));
    p.setPen(pal.color(QPalette::PlaceholderText));
    p.drawText(txt, Qt::AlignHCenter | Qt::AlignVCenter, tr("histogram"));
}

}  // namespace ud

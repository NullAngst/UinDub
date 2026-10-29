// SPDX-License-Identifier: GPL-3.0-or-later
// Copied from PingDub (github.com/NullAngst/PingDub, same author, GPL-3.0-or-later).
#include "Scopes.h"

#include "shared/Theme.h"

#include <QPainter>
#include <QPainterPath>

#include <algorithm>
#include <cmath>
#include <vector>

namespace ud {

namespace {

// 75% color bars in BT.601, (Cb, Cr) and label.
struct Target {
    int cb, cr;
    const char* name;
};
const Target kTargets[] = {{44, 142, "Yl"}, {156, 44, "Cy"}, {72, 58, "G"}, {184, 198, "Mg"}, {100, 212, "R"}, {212, 114, "B"}};

QRgb tint(int level, int which) {
    level = std::clamp(level, 0, 255);
    switch (which) {
    case 1: return qRgb(level * 6 / 10, level * 8 / 10, level);  // Cb, bluish
    case 2: return qRgb(level, level * 7 / 10, level * 6 / 10);  // Cr, reddish
    default: return qRgb(level * 9 / 10, level, level * 9 / 10);  // Y, faint green
    }
}

}  // namespace

Scope::Scope(Mode m, QWidget* parent) : QWidget(parent), m_mode(m) {
    setMinimumHeight(80);
    if (m == Mode::Waveform)
        setToolTip(tr("Waveform: luma of each column of the picture, 0 at the bottom, 255 at the top, lines at 16 and 235. "
                      "Parade shows Y, Cb and Cr side by side."));
    else
        setToolTip(tr("Vectorscope: Cb across, Cr up. Boxes are 75% color bars; the circle is the chroma limit."));
}

void Scope::setParade(bool on) {
    m_parade = on;
    m_dirty = true;
    update();
}

void Scope::setChromaLimit(int limit) {
    m_limit = limit;
    update();
}

void Scope::clear() {
    m_img = QImage();
    m_frame = ScopeFrame{};
    update();
}

void Scope::buildWaveform(const ScopeFrame& f, int outW, int outH) {
    const bool chroma = m_parade && f.cwidth > 0;
    m_panels = chroma ? 3 : 1;
    if (f.width <= 0 || f.height <= 0 || outW < 4 || outH < 4) return;
    // Luma takes half the width in parade, each chroma plane a quarter.
    const int wy = chroma ? outW / 2 : outW, wc = chroma ? (outW - wy) / 2 : 0;
    const int cols = wy + 2 * wc;
    std::vector<uint16_t> acc(size_t(cols) * size_t(outH), 0);
    auto add = [&](const std::vector<uint8_t>& plane, int w, int h, int x0, int pw) {
        for (int r = 0; r < h; ++r) {
            const uint8_t* row = plane.data() + size_t(r) * size_t(w);
            for (int x = 0; x < w; ++x) {
                const int bin = (int(row[x]) * outH) >> 8;
                // Wider than the source: each source column covers a run of output columns.
                const int c0 = x * pw / w, c1 = std::max(c0 + 1, (x + 1) * pw / w);
                for (int c = c0; c < c1; ++c) {
                    uint16_t& a = acc[size_t(bin) * size_t(cols) + size_t(x0 + c)];
                    if (a < 65535) ++a;
                }
            }
        }
    };
    add(f.y, f.width, f.height, 0, wy);
    if (chroma) {
        add(f.cb, f.cwidth, f.cheight, wy, wc);
        add(f.cr, f.cwidth, f.cheight, wy + wc, wc);
    }
    m_img = QImage(cols, outH, QImage::Format_RGB32);
    const double perCol = double(f.height) * double(f.width) / double(std::max(1, wy));
    const double norm = 255.0 / std::log1p(std::max(4.0, perCol / 6.0));
    for (int v = 0; v < outH; ++v) {
        QRgb* line = reinterpret_cast<QRgb*>(m_img.scanLine(outH - 1 - v));
        for (int x = 0; x < cols; ++x) {
            const int lvl = int(std::min(255.0, std::log1p(double(acc[size_t(v) * size_t(cols) + size_t(x)])) * norm));
            const int which = x < wy ? 0 : (x < wy + wc ? 1 : 2);
            line[x] = lvl ? tint(std::max(lvl, 60), which) : qRgb(0x14, 0x14, 0x14);
        }
    }
}

void Scope::buildVector(const ScopeFrame& f, int side) {
    m_gray = f.cwidth == 0;
    if (m_gray || side < 8) {
        m_img = QImage();
        return;
    }
    std::vector<uint32_t> acc(size_t(side) * size_t(side), 0);
    for (size_t i = 0; i < f.cb.size(); ++i) {
        const int x = (int(f.cb[i]) * side) >> 8, y = ((255 - int(f.cr[i])) * side) >> 8;
        ++acc[size_t(y) * size_t(side) + size_t(x)];
    }
    m_img = QImage(side, side, QImage::Format_RGB32);
    const double norm = 255.0 / std::log1p(std::max(8.0, double(f.cb.size()) / 3000.0));
    for (int y = 0; y < side; ++y) {
        QRgb* line = reinterpret_cast<QRgb*>(m_img.scanLine(y));
        for (int x = 0; x < side; ++x) {
            const int lvl = int(std::min(255.0, std::log1p(double(acc[size_t(y) * size_t(side) + size_t(x)])) * norm));
            line[x] = lvl ? qRgb(std::max(lvl, 60) * 8 / 10, std::max(lvl, 60), std::max(lvl, 60) * 8 / 10) : qRgb(0x14, 0x14, 0x14);
        }
    }
}

void Scope::setFrame(const ScopeFrame& f) {
    if (f.y.empty()) return;
    m_frame = f;
    m_dirty = true;
    update();
}

void Scope::paintEvent(QPaintEvent*) {
    QPainter p(this);
    const QRect area = rect().adjusted(2, 2, -2, -2);
    p.fillRect(area, QColor(0x14, 0x14, 0x14));
    const QColor grat(0x70, 0x70, 0x70);
    p.setPen(palette().color(QPalette::PlaceholderText));
    if (m_mode == Mode::Waveform) {
        if (!m_frame.y.empty() && (m_dirty || m_img.size() != area.size())) buildWaveform(m_frame, area.width(), area.height());
        m_dirty = false;
        if (!m_img.isNull()) p.drawImage(area.topLeft(), m_img);
        auto yOf = [&](int v) { return area.bottom() - (v * area.height()) / 256; };
        p.setPen(QPen(StatusColor::Warn, 1, Qt::DashLine));
        p.drawLine(area.left(), yOf(16), area.right(), yOf(16));
        p.drawLine(area.left(), yOf(235), area.right(), yOf(235));
        if (m_panels == 3 && !m_img.isNull()) {
            p.setPen(QPen(grat, 1));
            // Panel edges: luma takes half the columns, each chroma panel a quarter.
            const int x1 = area.left() + area.width() / 2, x2 = area.left() + area.width() * 3 / 4;
            p.drawLine(x1, area.top(), x1, area.bottom());
            p.drawLine(x2, area.top(), x2, area.bottom());
            p.setPen(palette().color(QPalette::PlaceholderText));
            p.drawText(QRect(area.left() + 4, area.top() + 2, 40, 14), Qt::AlignLeft, "Y");
            p.drawText(QRect(x1 + 4, area.top() + 2, 40, 14), Qt::AlignLeft, "Cb");
            p.drawText(QRect(x2 + 4, area.top() + 2, 40, 14), Qt::AlignLeft, "Cr");
        }
        p.setPen(palette().color(QPalette::PlaceholderText));
        p.drawText(area.adjusted(0, 0, -4, 0), Qt::AlignRight | Qt::AlignTop, m_parade ? tr("parade") : tr("waveform"));
        if (m_img.isNull()) p.drawText(area, Qt::AlignCenter, tr("no picture"));
        return;
    }
    // Vectorscope: a square in the middle.
    const int side = std::min(area.width(), area.height());
    const QRect sq(area.left() + (area.width() - side) / 2, area.top() + (area.height() - side) / 2, side, side);
    if (!m_frame.y.empty() && (m_dirty || m_img.width() != side)) buildVector(m_frame, side);
    m_dirty = false;
    if (m_gray) {
        p.drawText(area, Qt::AlignCenter, tr("gray source: no chroma"));
        return;
    }
    if (!m_img.isNull()) p.drawImage(sq.topLeft(), m_img);
    p.setRenderHint(QPainter::Antialiasing, true);
    auto pt = [&](double cb, double cr) {
        return QPointF(sq.left() + cb * sq.width() / 256.0, sq.top() + (255.0 - cr) * sq.height() / 256.0);
    };
    p.setPen(QPen(grat, 1));
    p.drawLine(pt(128, 0), pt(128, 255));
    p.drawLine(pt(0, 128), pt(255, 128));
    const double r = m_limit * sq.width() / 256.0;
    p.setPen(QPen(StatusColor::Warn, 1, Qt::DashLine));
    p.drawEllipse(pt(128, 128), r, r);
    p.setPen(QPen(grat, 1));
    const double box = 8.0 * sq.width() / 256.0;
    QFont f = font();
    f.setPointSizeF(std::max(6.0, f.pointSizeF() * 0.8));
    p.setFont(f);
    for (const Target& t : kTargets) {
        const QPointF c = pt(t.cb, t.cr);
        p.drawRect(QRectF(c.x() - box, c.y() - box, 2 * box, 2 * box));
        p.drawText(QPointF(c.x() + box + 2, c.y() + 4), t.name);
    }
}

}  // namespace ud

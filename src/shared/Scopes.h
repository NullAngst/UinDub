// SPDX-License-Identifier: GPL-3.0-or-later
// Copied from PingDub (github.com/NullAngst/PingDub, same author, GPL-3.0-or-later).
// Waveform monitor and vectorscope, drawn from the analysis thread's
// downsampled copy of each frame. They never touch the capture path.
//
// Waveform: luma per column of the picture, 0 to 255, lines at 16 and 235.
// Parade shows Y, Cb and Cr side by side.
// Vectorscope: Cb against Cr, target boxes for 75% color bars and the circle
// of the chroma limit.
#pragma once

#include "ui/ScopeTypes.h"

#include <QImage>
#include <QWidget>

namespace ud {

class Scope : public QWidget {
    Q_OBJECT
public:
    enum class Mode { Waveform, Vectorscope };
    explicit Scope(Mode m, QWidget* parent = nullptr);
    void setFrame(const ScopeFrame& f);
    void setParade(bool on);
    bool parade() const { return m_parade; }
    void setChromaLimit(int limit);
    void clear();
    QSize sizeHint() const override { return {300, 140}; }
    QSize minimumSizeHint() const override { return {120, 80}; }

protected:
    void paintEvent(QPaintEvent*) override;

private:
    // Built at the size it is drawn, so one-code lines never vanish in a downscale.
    void buildWaveform(const ScopeFrame& f, int w, int h);
    void buildVector(const ScopeFrame& f, int side);

    Mode m_mode;
    bool m_parade = false;
    int m_limit = kDefaultChromaLimit;
    ScopeFrame m_frame;
    bool m_dirty = false;
    QImage m_img;
    int m_panels = 1;
    bool m_gray = false;
};

}  // namespace ud

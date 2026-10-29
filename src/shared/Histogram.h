// SPDX-License-Identifier: GPL-3.0-or-later
// Copied from PingDub (github.com/NullAngst/PingDub, same author, GPL-3.0-or-later).
// Luma histogram with the 16 and 235 limits marked and the share of pixels
// outside them, so brightness and contrast can be set without crushing or
// clipping. The waveform monitor and vectorscope are their own tabs (Scopes).
#pragma once

#include "ui/ScopeTypes.h"

#include <QImage>
#include <QWidget>

#include <array>
#include <memory>

namespace ud {

class Histogram : public QWidget {
    Q_OBJECT
public:
    explicit Histogram(QWidget* parent = nullptr);
    void setLuma(std::shared_ptr<const LumaPlane> luma);
    QSize sizeHint() const override { return {300, 90}; }

protected:
    void paintEvent(QPaintEvent*) override;

private:
    std::array<uint32_t, 256> m_hist{};
    uint64_t m_total = 0, m_below = 0, m_above = 0;
};

}  // namespace ud

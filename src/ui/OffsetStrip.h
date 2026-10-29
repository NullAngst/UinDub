// SPDX-License-Identifier: GPL-3.0-or-later
// The offset strip beside the preview: the per-line correction for the
// current field as a graph, line 1 at the top. After a warble test it also
// draws the true answer, so you see how close Analyze came.
#pragma once

#include "PreviewEngine.h"
#include "core/Damage.h"

#include <QWidget>

namespace ud {

class OffsetStrip : public QWidget {
    Q_OBJECT
public:
    explicit OffsetStrip(QWidget* parent = nullptr);
    void showFrame(PreviewFramePtr f, double strength);
    void setTruth(bool on, const DamageModel& d, FieldOrder order);
    QSize sizeHint() const override { return {150, 400}; }
    QSize minimumSizeHint() const override { return {90, 120}; }

protected:
    void paintEvent(QPaintEvent*) override;

private:
    PreviewFramePtr m_frame;
    double m_strength = 1.0;
    bool m_truth = false;
    DamageModel m_damage;
    FieldOrder m_order = FieldOrder::TFF;
};

}  // namespace ud

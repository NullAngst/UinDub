// SPDX-License-Identifier: GPL-3.0-or-later
// Small building blocks for the side panel: a slider tied to a spin box,
// and running a job off the GUI thread with its result posted back.
#pragma once

#include <QDoubleSpinBox>
#include <QLabel>
#include <QObject>
#include <QSlider>
#include <QThread>
#include <QWidget>

#include <functional>

namespace ud {

class SliderSpin : public QWidget {
    Q_OBJECT
public:
    SliderSpin(double min, double max, double step, int decimals, const QString& suffix = {}, QWidget* parent = nullptr);
    double value() const { return m_spin->value(); }
    void setValue(double v);  // no signal
    void setDefault(double v) { m_default = v; }
    QDoubleSpinBox* spin() const { return m_spin; }

signals:
    void valueChanged(double v);

protected:
    void mouseDoubleClickEvent(QMouseEvent*) override;

private:
    QSlider* m_slider;
    QDoubleSpinBox* m_spin;
    double m_step;
    double m_default = 0;
};

// Runs `work` on a new thread, then `done` on the GUI thread with `context` alive.
void runInBackground(QObject* context, std::function<void()> work, std::function<void()> done);

QLabel* noteLabel(const QString& text, QWidget* parent = nullptr);

}  // namespace ud

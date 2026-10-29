// SPDX-License-Identifier: GPL-3.0-or-later
#include "Controls.h"

#include <QHBoxLayout>
#include <QMetaObject>
#include <QPointer>
#include <QSignalBlocker>

#include <cmath>

namespace ud {

SliderSpin::SliderSpin(double min, double max, double step, int decimals, const QString& suffix, QWidget* parent)
    : QWidget(parent), m_step(step) {
    auto* l = new QHBoxLayout(this);
    l->setContentsMargins(0, 0, 0, 0);
    m_slider = new QSlider(Qt::Horizontal, this);
    m_slider->setRange(int(std::lround(min / step)), int(std::lround(max / step)));
    m_spin = new QDoubleSpinBox(this);
    m_spin->setRange(min, max);
    m_spin->setSingleStep(step);
    m_spin->setDecimals(decimals);
    m_spin->setSuffix(suffix);
    m_spin->setKeyboardTracking(false);
    m_spin->setMinimumWidth(78);
    l->addWidget(m_slider, 1);
    l->addWidget(m_spin);
    connect(m_slider, &QSlider::valueChanged, this, [this](int v) {
        const QSignalBlocker b(m_spin);
        m_spin->setValue(v * m_step);
        emit valueChanged(m_spin->value());
    });
    connect(m_spin, qOverload<double>(&QDoubleSpinBox::valueChanged), this, [this](double v) {
        const QSignalBlocker b(m_slider);
        m_slider->setValue(int(std::lround(v / m_step)));
        emit valueChanged(v);
    });
    setToolTip(tr("Double-click to reset."));
}

void SliderSpin::setValue(double v) {
    const QSignalBlocker a(m_slider), b(m_spin);
    m_spin->setValue(v);
    m_slider->setValue(int(std::lround(v / m_step)));
}

void SliderSpin::mouseDoubleClickEvent(QMouseEvent*) {
    setValue(m_default);
    emit valueChanged(m_default);
}

void runInBackground(QObject* context, std::function<void()> work, std::function<void()> done) {
    QPointer<QObject> ctx(context);
    QThread* t = QThread::create([work = std::move(work)] { work(); });
    QObject::connect(t, &QThread::finished, context, [ctx, t, done = std::move(done)] {
        t->deleteLater();
        if (ctx && done) done();
    });
    t->start();
}

QLabel* noteLabel(const QString& text, QWidget* parent) {
    auto* l = new QLabel(text, parent);
    l->setObjectName("note");
    l->setWordWrap(true);
    return l;
}

}  // namespace ud

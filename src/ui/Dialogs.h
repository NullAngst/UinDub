// SPDX-License-Identifier: GPL-3.0-or-later
// Tools > Warble test, and Preferences (theme, keys, folders).
#pragma once

#include "Controls.h"
#include "core/Damage.h"
#include "core/Source.h"

#include <QDialog>
#include <QKeySequenceEdit>
#include <QLineEdit>
#include <QMap>
#include <QProgressBar>
#include <QSpinBox>

namespace ud {

class WarbleDialog : public QDialog {
    Q_OBJECT
public:
    WarbleDialog(const SourceInfo& clean, FieldOrder order, int64_t start, QWidget* parent = nullptr);
    // The project folder of the copy, when one was made.
    QString createdProject() const { return m_created; }

private:
    void make();
    SourceInfo m_clean;
    FieldOrder m_order;
    SliderSpin *m_jitter, *m_wobble, *m_period, *m_crawl, *m_hook, *m_hookLines, *m_field;
    QSpinBox *m_start, *m_frames;
    QLineEdit* m_out;
    QProgressBar* m_progress;
    QPushButton* m_go;
    QString m_created;
};

class PreferencesDialog : public QDialog {
    Q_OBJECT
public:
    explicit PreferencesDialog(QWidget* parent = nullptr);

signals:
    void keysChanged();

private:
    QMap<QString, QKeySequenceEdit*> m_keys;
};

}  // namespace ud

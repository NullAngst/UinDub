// SPDX-License-Identifier: GPL-3.0-or-later
// The side panel tabs, in pipeline order: Source, Edge Lock, Deinterlace,
// Color, Crop, Output. Each edits the session's project and says what changed.
#pragma once

#include "Controls.h"
#include "Session.h"

#include <QCheckBox>
#include <QComboBox>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QProgressBar>
#include <QPushButton>
#include <QSpinBox>
#include <QWidget>

namespace ud {

std::vector<QColor> referenceColors();

class SourceTab : public QWidget {
    Q_OBJECT
public:
    explicit SourceTab(Session* s, QWidget* parent = nullptr);
    void refresh();

private:
    void verify();
    Session* m_s;
    QLabel* m_info;
    QLabel* m_pingdub;
    QComboBox* m_field;
    QLabel* m_fieldInfo;
    QLineEdit* m_label;
    QPushButton* m_verify;
    QCheckBox* m_verifyBefore;
    QLabel* m_verifyResult;
};

class EdgeLockTab : public QWidget {
    Q_OBJECT
public:
    explicit EdgeLockTab(Session* s, QWidget* parent = nullptr);
    void refresh();
    void setAnalyzing(bool running, double fraction, const QString& status);

signals:
    void analyzeRequested();
    void stopRequested();
    void useBorderRequested();
    void drawRequested();

private:
    void settingChanged();
    Session* m_s;
    QCheckBox* m_enable;
    QListWidget* m_refs;
    QPushButton* m_border;
    QPushButton* m_draw;
    QPushButton* m_remove;
    QPushButton* m_carry;
    QSpinBox* m_range;
    QSpinBox* m_topLines;
    QSpinBox* m_topRange;
    QSpinBox* m_tmpl;
    QDoubleSpinBox* m_conf;
    QSpinBox* m_gap;
    QSpinBox* m_smooth;
    SliderSpin* m_strength;
    QComboBox* m_fill;
    QCheckBox* m_length;
    QPushButton* m_analyze;
    QProgressBar* m_progress;
    QLabel* m_status;
    QLabel* m_score;
    bool m_running = false;
};

class DeinterlaceTab : public QWidget {
    Q_OBJECT
public:
    explicit DeinterlaceTab(Session* s, QWidget* parent = nullptr);
    void refresh();

private:
    void changed();
    Session* m_s;
    QComboBox* m_mode;
    QComboBox* m_rate;
    QComboBox* m_preset;
    QSpinBox* m_sourceMatch;
    QDoubleSpinBox* m_sharp;
    QLabel* m_vs;
    QLabel* m_out;
    QLabel* m_warn;
};

class ColorTab : public QWidget {
    Q_OBJECT
public:
    explicit ColorTab(Session* s, QWidget* parent = nullptr);
    void refresh();
    void setClipped(double share);

private:
    ColorSettings& current();
    void push();
    void autoLevels();
    Session* m_s;
    QCheckBox* m_perChapter;
    QComboBox* m_scope;
    SliderSpin *m_bright, *m_contrast, *m_gamma, *m_black, *m_white, *m_sat, *m_hue;
    SliderSpin *m_lift[3], *m_gammaRgb[3], *m_gain[3];
    SliderSpin *m_delayX, *m_delayY, *m_blur;
    QPushButton* m_auto;
    QLabel* m_clip;
};

class CropTab : public QWidget {
    Q_OBJECT
public:
    explicit CropTab(Session* s, QWidget* parent = nullptr);
    void refresh();
    bool editing() const;
    void safeCrop(int& l, int& r, int& bottom) const;

signals:
    void editToggled(bool on);

private:
    void changed();
    Session* m_s;
    QSpinBox *m_l, *m_r, *m_t, *m_b;
    QCheckBox* m_edit;
    QLabel* m_safe;
    QPushButton* m_toSafe;
    QComboBox* m_aspect;
    QLabel* m_result;
};

class OutputTab : public QWidget {
    Q_OBJECT
public:
    explicit OutputTab(Session* s, QWidget* parent = nullptr);
    void refresh();

signals:
    void renderRequested();
    void queueRequested();

private:
    void changed();
    Session* m_s;
    QComboBox* m_codec;
    SliderSpin* m_rf;
    QComboBox* m_preset;
    QCheckBox* m_422;
    QComboBox* m_audio;
    QSpinBox* m_kbps;
    QComboBox* m_container;
    QLineEdit* m_folder;
    QLineEdit* m_name;
    QCheckBox* m_chapters;
    QCheckBox* m_captions;
    QLabel* m_path;
    QPushButton* m_render;
    QPushButton* m_queue;
};

}  // namespace ud

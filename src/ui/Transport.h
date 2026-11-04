// SPDX-License-Identifier: GPL-3.0-or-later
// The bar over the timeline: playback buttons, trim and cut buttons and the
// timeline zoom, plus a status bar label with what the trim keeps. Every button runs the same action
// as its key, so the tooltip names the key that does it too.
#pragma once

#include <QHash>
#include <QStyle>
#include <QLabel>
#include <QScrollBar>
#include <QSlider>
#include <QToolButton>
#include <QWidget>

namespace ud {

class Session;
class Timeline;

class TransportBar : public QWidget {
    Q_OBJECT
public:
    TransportBar(Session* s, Timeline* t, QWidget* parent = nullptr);
    void setPlaying(bool playing);
    // Puts the bound key in each button's tooltip.
    void setKeyHints(const QHash<QString, QString>& keys);
    // The scrollbar that pans a zoomed timeline; it lives under the timeline.
    QScrollBar* scrollBar() const { return m_scroll; }
    // What the trim keeps, for the status bar.
    QLabel* trimLabel() const { return m_trim; }

signals:
    // An action id from keyActions(): "play", "mark_in", "zoom_in" and so on.
    void action(const QString& id);

private:
    QToolButton* button(const QString& id, const QString& text, const QString& tip, QStyle::StandardPixmap icon = QStyle::SP_CustomBase);
    void updateTrim();
    void updateZoom(qint64 a, qint64 b, qint64 total);

    Session* m_s;
    Timeline* m_t;
    QHash<QString, QToolButton*> m_buttons;
    QHash<QString, QString> m_tips;
    QToolButton* m_play = nullptr;
    QLabel* m_trim = nullptr;
    QSlider* m_zoom = nullptr;
    QScrollBar* m_scroll = nullptr;
    bool m_syncing = false;
};

}  // namespace ud

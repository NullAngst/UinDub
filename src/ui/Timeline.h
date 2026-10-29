// SPDX-License-Identifier: GPL-3.0-or-later
// The timeline under the preview: scrubber, in and out points, chapters
// along the top, PingDub's events (inserts, drops, signal loss) as ticks,
// the correction lane and one Edge Lock lane per reference with its spans
// and Lost shading, cut spans hatched, and a render-progress band.
#pragma once

#include "Session.h"

#include <QWidget>

#include <array>
#include <vector>

namespace ud {

class Timeline : public QWidget {
    Q_OBJECT
public:
    explicit Timeline(Session* s, QWidget* parent = nullptr);
    QSize sizeHint() const override;
    void setRefColors(const std::vector<QColor>& c);
    void setProgress(double fraction, bool active);
    // Selection made with Shift-drag: [a, b), or a == b when none.
    std::pair<int64_t, int64_t> selection() const { return {m_selA, m_selB}; }
    void clearSelection();
    // Nearest boundary (chapter, signal loss) within reach of `frame`, or itself.
    int64_t snap(int64_t frame) const;
    void setCutStart(int64_t frame);  // -1 clears the pending C mark
    int64_t cutStart() const { return m_cutStart; }

signals:
    void cutRequested(qint64 a, qint64 b);
    void dontCorrectRequested(qint64 a, qint64 b);
    void reanalyzeRequested(qint64 a, qint64 b);
    void setInOutRequested(qint64 a, qint64 b);
    void deleteCutRequested(qint64 frame);
    void deleteOverrideRequested(qint64 frame);
    void carryForwardRequested(int slot, qint64 frame);
    void endSpanRequested(int slot, qint64 frame);

protected:
    void paintEvent(QPaintEvent*) override;
    void mousePressEvent(QMouseEvent* e) override;
    void mouseMoveEvent(QMouseEvent* e) override;
    void mouseReleaseEvent(QMouseEvent* e) override;
    void wheelEvent(QWheelEvent* e) override;
    void contextMenuEvent(QContextMenuEvent* e) override;
    void resizeEvent(QResizeEvent*) override;

private:
    double xOf(int64_t frame) const;
    int64_t frameAt(double x) const;
    int laneAt(double y) const;  // -1 none, else reference slot
    void fitView();
    void ensureVisible(int64_t frame);
    void buildSummary();

    Session* m_s;
    std::vector<QColor> m_colors;
    int64_t m_viewA = 0, m_viewB = 1;  // frames shown
    bool m_dragging = false, m_selecting = false;
    int64_t m_selA = 0, m_selB = 0, m_selAnchor = 0;
    int64_t m_cutStart = -1;
    double m_progress = 0;
    bool m_rendering = false;
    // per column summaries of the tracks
    std::vector<float> m_colMean;
    std::vector<std::array<uint8_t, kMaxReferences>> m_colState;
    int64_t m_sumA = -1, m_sumB = -1;
    int m_sumW = 0;
    size_t m_sumTracks = 0;
    const void* m_sumTrackPtr = nullptr;
};

}  // namespace ud

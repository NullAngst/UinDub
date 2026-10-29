// SPDX-License-Identifier: GPL-3.0-or-later
// The window: preview in the middle, the offset strip beside it, the
// timeline under it, the side panel tabs in pipeline order, the scopes and
// queue docks. Laid out like PingDub so moving between the two feels the same.
#pragma once

#include "Session.h"

#include <QAction>
#include <QLabel>
#include <QMainWindow>
#include <QTimer>

#include <atomic>
#include <memory>

namespace ud {

class PreviewEngine;
class PreviewWidget;
class OffsetStrip;
class Timeline;
class SourceTab;
class EdgeLockTab;
class DeinterlaceTab;
class ColorTab;
class CropTab;
class OutputTab;
class ScopesDock;
class QueueDock;

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(QWidget* parent = nullptr);
    ~MainWindow() override;
    bool openPath(const QString& path);

protected:
    void closeEvent(QCloseEvent* e) override;
    void dragEnterEvent(QDragEnterEvent* e) override;
    void dropEvent(QDropEvent* e) override;

private:
    void buildMenus();
    void bindKeys();
    void requestPreview();
    void onOpened();
    void openDialog();
    void saveAs();
    void useBorder();
    void startDraw();
    void drawnEdge(const std::vector<PointF>& pts);
    void analyze(int64_t start = 0, int64_t end = -1, const EdgeLockSettings* settings = nullptr);
    void stopAll();
    void render(bool startNow);
    void markCut();
    void setPlaying(int speed);
    void tick();
    void updateStatus();
    void updateRecent();
    void showMessage(const QString& m);

    Session* m_s;
    PreviewEngine* m_engine;
    PreviewWidget* m_preview;
    OffsetStrip* m_strip;
    Timeline* m_timeline;
    SourceTab* m_sourceTab;
    EdgeLockTab* m_edgeTab;
    DeinterlaceTab* m_deintTab;
    ColorTab* m_colorTab;
    CropTab* m_cropTab;
    OutputTab* m_outputTab;
    ScopesDock* m_scopes;
    QueueDock* m_queue;
    QLabel* m_pos;
    QLabel* m_msg;
    QMenu* m_recentMenu = nullptr;

    QAction* m_actProcessed = nullptr;
    QAction* m_actSource = nullptr;
    QAction* m_actSplit = nullptr;
    QAction* m_actField = nullptr;
    QAction* m_actOverlay = nullptr;
    QAction* m_actQtgmc = nullptr;
    QMap<QString, QAction*> m_keyed;

    QTimer m_play;
    int m_speed = 0;  // frames per tick, negative backwards
    bool m_qtgmcPending = false;

    std::shared_ptr<std::atomic<bool>> m_analyzeCancel;
    bool m_analyzing = false;
};

}  // namespace ud

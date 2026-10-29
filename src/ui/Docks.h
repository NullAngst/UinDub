// SPDX-License-Identifier: GPL-3.0-or-later
// The scopes dock (histogram, waveform with parade, vectorscope, drawn from
// the processed frame or the source) and the render queue dock.
#pragma once

#include "PreviewEngine.h"
#include "core/Render.h"

#include <QDockWidget>
#include <QLabel>
#include <QListWidget>
#include <QPlainTextEdit>
#include <QPushButton>

#include <atomic>
#include <memory>

namespace ud {

class Histogram;
class Scope;

class ScopesDock : public QDockWidget {
    Q_OBJECT
public:
    explicit ScopesDock(QWidget* parent = nullptr);
    void showFrame(PreviewFramePtr f);

private:
    void redraw();
    PreviewFramePtr m_frame;
    Histogram* m_hist;
    Scope* m_wave;
    Scope* m_vector;
    QPushButton* m_source;
    QPushButton* m_parade;
    QLabel* m_clip;
};

struct QueueItem {
    RenderJob job;
    QString title;
    enum class State { Waiting, Rendering, Done, Failed, Cancelled } state = State::Waiting;
    double progress = 0;
    QString status;
    QStringList log;
};

class QueueDock : public QDockWidget {
    Q_OBJECT
public:
    explicit QueueDock(QWidget* parent = nullptr);
    ~QueueDock() override;
    void add(const RenderJob& job, const QString& title, bool start);
    bool running() const { return m_running >= 0; }
    void stop();

signals:
    void progress(double fraction, bool active);
    void finished(const QString& outPath, bool ok);

private:
    void startNext();
    void refreshList();
    void showLog();

    QListWidget* m_list;
    QPlainTextEdit* m_log;
    QPushButton* m_start;
    QPushButton* m_stop;
    QPushButton* m_remove;
    std::vector<std::shared_ptr<QueueItem>> m_items;
    int m_running = -1;
    std::shared_ptr<std::atomic<bool>> m_cancel;
};

}  // namespace ud

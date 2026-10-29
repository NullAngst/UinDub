// SPDX-License-Identifier: GPL-3.0-or-later
#include "Docks.h"

#include "Controls.h"
#include "shared/Histogram.h"
#include "shared/Scopes.h"

#include <QDateTime>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QMetaObject>
#include <QPointer>
#include <QSplitter>
#include <QThread>
#include <QVBoxLayout>

namespace ud {

ScopesDock::ScopesDock(QWidget* parent) : QDockWidget(tr("Scopes"), parent) {
    setObjectName("scopesDock");
    auto* w = new QWidget(this);
    auto* l = new QVBoxLayout(w);
    l->setContentsMargins(4, 4, 4, 4);
    auto* row = new QHBoxLayout;
    m_source = new QPushButton(tr("Show source"), w);
    m_source->setCheckable(true);
    m_source->setToolTip(tr("Scopes drawn from the processed frame; toggle to see the source instead."));
    m_parade = new QPushButton(tr("Parade"), w);
    m_parade->setCheckable(true);
    row->addWidget(m_source);
    row->addWidget(m_parade);
    row->addStretch(1);
    l->addLayout(row);
    m_hist = new Histogram(w);
    m_wave = new Scope(Scope::Mode::Waveform, w);
    m_vector = new Scope(Scope::Mode::Vectorscope, w);
    auto* split = new QSplitter(Qt::Vertical, w);
    split->addWidget(m_hist);
    split->addWidget(m_wave);
    split->addWidget(m_vector);
    l->addWidget(split, 1);
    m_clip = new QLabel(w);
    m_clip->setWordWrap(true);
    l->addWidget(m_clip);
    setWidget(w);
    connect(m_source, &QPushButton::toggled, this, &ScopesDock::redraw);
    connect(m_parade, &QPushButton::toggled, this, [this](bool on) { m_wave->setParade(on); });
}

void ScopesDock::showFrame(PreviewFramePtr f) {
    m_frame = std::move(f);
    if (isVisible()) redraw();
}

void ScopesDock::redraw() {
    if (!m_frame) return;
    const bool src = m_source->isChecked();
    if (auto l = src ? m_frame->lumaSource : m_frame->luma) m_hist->setLuma(l);
    m_wave->setFrame(src ? m_frame->scopeSource : m_frame->scope);
    m_vector->setFrame(src ? m_frame->scopeSource : m_frame->scope);
    const double c = m_frame->clipped;
    if (c <= 0) m_clip->setText(tr("<span style='color:#3FB950'>Settings push nothing past legal range.</span>"));
    else m_clip->setText(tr("<span style='color:%1'>%2% pushed past legal range by the color settings.</span>").arg(c > 0.005 ? "#F85149" : "#D29922").arg(c * 100, 0, 'f', 2));
}

// ---------------------------------------------------------------------------

QueueDock::QueueDock(QWidget* parent) : QDockWidget(tr("Queue"), parent) {
    setObjectName("queueDock");
    auto* w = new QWidget(this);
    auto* l = new QVBoxLayout(w);
    l->setContentsMargins(4, 4, 4, 4);
    m_list = new QListWidget(w);
    m_log = new QPlainTextEdit(w);
    m_log->setReadOnly(true);
    m_log->setMaximumBlockCount(5000);
    auto* split = new QSplitter(Qt::Vertical, w);
    split->addWidget(m_list);
    split->addWidget(m_log);
    l->addWidget(split, 1);
    auto* row = new QHBoxLayout;
    m_start = new QPushButton(tr("Start"), w);
    m_stop = new QPushButton(tr("Stop"), w);
    m_remove = new QPushButton(tr("Remove"), w);
    row->addWidget(m_start);
    row->addWidget(m_stop);
    row->addWidget(m_remove);
    l->addLayout(row);
    setWidget(w);
    connect(m_start, &QPushButton::clicked, this, &QueueDock::startNext);
    connect(m_stop, &QPushButton::clicked, this, &QueueDock::stop);
    connect(m_remove, &QPushButton::clicked, this, [this] {
        const int i = m_list->currentRow();
        if (i < 0 || i >= int(m_items.size()) || i == m_running) return;
        m_items.erase(m_items.begin() + i);
        if (m_running > i) --m_running;
        refreshList();
    });
    connect(m_list, &QListWidget::currentRowChanged, this, &QueueDock::showLog);
    refreshList();
}

QueueDock::~QueueDock() {
    if (m_cancel) *m_cancel = true;
}

void QueueDock::add(const RenderJob& job, const QString& title, bool start) {
    auto it = std::make_shared<QueueItem>();
    it->job = job;
    it->title = title;
    it->log << QDateTime::currentDateTime().toString(Qt::ISODate) + tr(" queued: ") + QString::fromStdString(job.outPath);
    m_items.push_back(it);
    refreshList();
    m_list->setCurrentRow(int(m_items.size()) - 1);
    if (start && m_running < 0) startNext();
}

void QueueDock::stop() {
    if (m_cancel) *m_cancel = true;
}

void QueueDock::refreshList() {
    const int keep = m_list->currentRow();
    m_list->clear();
    for (const auto& it : m_items) {
        QString st;
        switch (it->state) {
        case QueueItem::State::Waiting: st = tr("waiting"); break;
        case QueueItem::State::Rendering: st = tr("%1%  %2").arg(it->progress * 100, 0, 'f', 1).arg(it->status); break;
        case QueueItem::State::Done: st = tr("done"); break;
        case QueueItem::State::Failed: st = tr("failed"); break;
        case QueueItem::State::Cancelled: st = tr("stopped"); break;
        }
        m_list->addItem(it->title + "  [" + st + "]");
    }
    if (keep >= 0 && keep < m_list->count()) m_list->setCurrentRow(keep);
    m_stop->setEnabled(m_running >= 0);
    m_start->setEnabled(m_running < 0);
}

void QueueDock::showLog() {
    const int i = m_list->currentRow();
    m_log->setPlainText(i >= 0 && i < int(m_items.size()) ? m_items[size_t(i)]->log.join("\n") : QString());
}

void QueueDock::startNext() {
    if (m_running >= 0) return;
    int next = -1;
    for (size_t i = 0; i < m_items.size(); ++i)
        if (m_items[i]->state == QueueItem::State::Waiting) {
            next = int(i);
            break;
        }
    if (next < 0) {
        emit progress(0, false);
        return;
    }
    m_running = next;
    auto item = m_items[size_t(next)];
    item->state = QueueItem::State::Rendering;
    item->log << QDateTime::currentDateTime().toString(Qt::ISODate) + tr(" started");
    refreshList();
    m_cancel = std::make_shared<std::atomic<bool>>(false);
    auto cancel = m_cancel;
    QPointer<QueueDock> self(this);
    auto outcome = std::make_shared<RenderOutcome>();
    QThread* t = QThread::create([item, cancel, self, outcome] {
        *outcome = runRender(item->job, [&](const RenderProgress& p) {
            const QString stage = QString::fromStdString(p.stage);
            const double fps = p.fps;
            const double frac = p.fraction;
            QMetaObject::invokeMethod(
                self,
                [self, item, stage, fps, frac] {
                    if (!self) return;
                    item->progress = frac;
                    item->status = fps > 0 ? QObject::tr("%1, %2 fps").arg(stage).arg(fps, 0, 'f', 1) : stage;
                    self->refreshList();
                    emit self->progress(frac, true);
                },
                Qt::QueuedConnection);
            return !*cancel;
        });
    });
    connect(t, &QThread::finished, this, [this, t, item, outcome] {
        t->deleteLater();
        const RenderOutcome& o = *outcome;
        for (const std::string& n : o.notes) item->log << QString::fromStdString(n);
        if (o.ok) {
            item->state = QueueItem::State::Done;
            item->log << tr("Done in %1 s: %2 video frames, %3 audio samples").arg(o.seconds, 0, 'f', 1).arg(o.videoFrames).arg(o.audioSamples);
            item->log << QString::fromStdString(o.outPath);
            if (!o.sidecar.empty()) item->log << QString::fromStdString(o.sidecar);
        } else if (o.cancelled) {
            item->state = QueueItem::State::Cancelled;
            item->log << tr("Stopped. The partial file was removed.");
        } else {
            item->state = QueueItem::State::Failed;
            item->log << tr("Failed: ") + QString::fromStdString(o.error);
        }
        m_running = -1;
        m_cancel.reset();
        refreshList();
        showLog();
        emit progress(0, false);
        emit finished(QString::fromStdString(o.outPath), o.ok);
        startNext();
    });
    t->start();
}

}  // namespace ud
